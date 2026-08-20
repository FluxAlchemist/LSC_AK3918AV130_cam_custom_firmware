using System;
using System.IO;
using System.IO.Compression;
using System.Formats.Tar;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using Windows.Storage;
using System.Net;
using System.Collections.Generic;

namespace serial_console_dotnet.Services
{
    public class FileTransferService
    {
        private readonly ConnectionService _serialService;

        // Transfer states
        private bool _isUploading = false;
        private bool _isCapturingSize = false;
        private bool _isDownloading = false;

        private string _downloadPath = "";
        private int _expectedDownloadSize = 0;
        private int _expectedBase64Size = 0;
        private bool _hasExpectedSize = false;

        private readonly StringBuilder _sizeBuffer = new StringBuilder();
        private readonly StringBuilder _downloadBuffer = new StringBuilder();
        private Task? _sizeTimeoutTask;

        // Streaming download fields
        private StorageFile? _localDownloadFile;
        private FileStream? _localWriteStream;
        private readonly StringBuilder _base64CharBuffer = new StringBuilder();
        private int _downloadedBase64Length = 0;

        // Events for UI communication
        public event Action<string>? StatusChanged;
        public event Action<double>? ProgressChanged;
        public event Action? DownloadComplete;
        public event Action<string>? TransferError;
        public event Action? UploadComplete;

        private IDialogService? _dialogService;
        private ShellExecutionService? _shellExecutionService;

        public bool IsUploading => _isUploading;
        public bool IsDownloading => _isDownloading;
        public bool IsCapturingSize => _isCapturingSize;

        public FileTransferService(ConnectionService serialService)
        {
            _serialService = serialService;
        }

        public void InitializeDependencies(IDialogService dialogService, ShellExecutionService shellExecutionService)
        {
            _dialogService = dialogService;
            _shellExecutionService = shellExecutionService;
        }

        private void RaiseError(string error)
        {
            _shellExecutionService?.LogMessage(error, "error");
            TransferError?.Invoke(error);
        }

        // Hardware-confirmed ceiling (2026-07-06): a single raw Write() burst above this
        // reliably truncates/corrupts on this device's actual UART driver — well below
        // the textbook 4096-byte tty canonical-line limit this was originally guessed
        // from. Do not raise this without re-testing on real hardware; a prior attempt
        // at 2048/8192 produced silent "base64: truncated base64 input" corruption that
        // only the md5sum check below caught. Applies to both Serial and Telnet, since
        // Telnet still rides a pty with its own line buffer on the device side.
        private const int SafeLineLength = 512;

        // Picks the inter-line delay for the current transport. Telnet rides over TCP,
        // which already guarantees reliable, flow-controlled delivery, so a small fixed
        // delay is enough. Serial has no RTS/CTS on this hardware: outrunning the
        // receiver's buffer silently corrupts the transfer with no error, so pace lines
        // to roughly the wire time at the actual baud rate plus a safety margin.
        private int GetLineDelayMs()
        {
            if (_serialService.Mode == ConnectionMode.Telnet)
            {
                return 20;
            }

            int baud = _serialService.BaudRate ?? 115200;
            double wireTimeMs = SafeLineLength * 10.0 / baud * 1000.0; // 8N1 = 10 bits/byte
            return Math.Max(30, (int)Math.Ceiling(wireTimeMs * 1.5));
        }

        // Streams raw bytes to the device as a base64 sidecar file (`<remotePath>.b64`),
        // decodes it into remotePath, then confirms the decode landed intact via the
        // device's own `md5sum` — cheap insurance against the silent corruption risk
        // noted above. Shared by both the single-file and packaged multi-file (tar.gz)
        // upload paths.
        //
        // Keeps exactly one `cat >> ... << 'EOF'` heredoc open for the entire transfer
        // (one shell fork total) instead of forking a new `cat` per chunk, which used to
        // dominate transfer time. Each individual line written to it is still capped at
        // SafeLineLength bytes with a delay in between — that per-burst pacing, not the
        // number of shell processes, is what keeps the receiver's buffer from overrunning.
        private async Task UploadBytesAsBase64Async(byte[] fileBytes, string remotePath, double progressFloor, double progressCeiling, bool verifyMd5)
        {
            string base64Str = Convert.ToBase64String(fileBytes);
            string b64Path = $"{remotePath}.b64";

            _serialService.Write($"> \"{b64Path}\"", "lf");
            await Task.Delay(200);

            int lineDelayMs = GetLineDelayMs();
            int totalLines = Math.Max(1, (int)Math.Ceiling(base64Str.Length / (double)SafeLineLength));

            _serialService.Write($"cat >> \"{b64Path}\" << 'EOF'", "lf");
            await Task.Delay(Math.Max(100, lineDelayMs));

            for (int i = 0; i < totalLines; i++)
            {
                int start = i * SafeLineLength;
                int length = Math.Min(SafeLineLength, base64Str.Length - start);
                string line = base64Str.Substring(start, length);

                StatusChanged?.Invoke($"Uploading line {i + 1}/{totalLines}...");
                double frac = (i + 1) / (double)totalLines;
                ProgressChanged?.Invoke(Math.Round(progressFloor + frac * (progressCeiling - progressFloor)));

                _serialService.Write(line, "lf");
                await Task.Delay(lineDelayMs);
            }

            _serialService.Write("EOF", "lf");
            await Task.Delay(300);

            StatusChanged?.Invoke("Reconstructing file on device...");
            _serialService.Write($"base64 -d \"{b64Path}\" > \"{remotePath}\" && rm -f \"{b64Path}\"", "lf");
            await Task.Delay(300);

            if (!verifyMd5) return;

            StatusChanged?.Invoke("Verifying transfer (md5sum)...");
            string? remoteMd5 = await GetRemoteMd5Async(remotePath);
            if (remoteMd5 == null)
            {
                throw new Exception($"Could not read md5sum for \"{remotePath}\" on device — assuming a corrupted or incomplete transfer.");
            }

            string localMd5 = Convert.ToHexString(MD5.HashData(fileBytes)).ToLowerInvariant();
            if (!string.Equals(remoteMd5, localMd5, StringComparison.OrdinalIgnoreCase))
            {
                throw new Exception($"MD5 mismatch after upload to \"{remotePath}\" (local {localMd5}, device {remoteMd5}) — transfer was corrupted.");
            }
        }

        // Runs `md5sum` on the device and pulls the 32 hex-char digest back out via
        // ShellExecutionService's marker-delimited hidden-query mechanism (same pattern
        // as GetRemoteFilesListAsync/CheckIfRemotePathIsDirectoryAsync below).
        private async Task<string?> GetRemoteMd5Async(string remotePath)
        {
            if (_shellExecutionService == null) return null;

            string escapedPath = remotePath.Replace("'", "\\'");
            string cmd = $"echo \"===MD5_ST\"\"ART===\" ; md5sum '{escapedPath}' ; echo \"===MD5_E\"\"ND===\"";

            string result;
            try
            {
                result = await _shellExecutionService.RunHiddenQueryAsync(cmd, "===MD5_START===", "===MD5_END===", 8000);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"md5sum query failed: {ex.Message}");
                return null;
            }

            var match = Regex.Match(result, "[0-9a-fA-F]{32}");
            return match.Success ? match.Value.ToLowerInvariant() : null;
        }

        public async Task StartUploadAsync(StorageFile file, string remotePath, bool verifyMd5 = true)
        {
            if (_isUploading) return;
            _isUploading = true;

            try
            {
                StatusChanged?.Invoke("Reading local file...");
                ProgressChanged?.Invoke(0);

                byte[] fileBytes;
                using (var stream = await file.OpenStreamForReadAsync())
                {
                    fileBytes = new byte[stream.Length];
                    await stream.ReadExactlyAsync(fileBytes, 0, fileBytes.Length);
                }

                StatusChanged?.Invoke("Clearing target path...");
                await UploadBytesAsBase64Async(fileBytes, remotePath, 0, 90, verifyMd5);

                ProgressChanged?.Invoke(100);
                StatusChanged?.Invoke("Upload Complete!");
                _shellExecutionService?.LogMessage($"--- File upload successful: {file.Name} -> {remotePath} ---", "info");
                UploadComplete?.Invoke();
            }
            catch (Exception ex)
            {
                RaiseError($"Upload failed: {ex.Message}");
            }
            finally
            {
                _isUploading = false;
            }
        }

        // Packages multiple local files into a single client-side gzip'd tar archive,
        // uploads that one archive via the chunked base64 path, then unpacks it on the
        // device. The device has `gunzip` (busybox) and a real `tar` but no `gzip`/`xz`,
        // so decompression must be piped: `gunzip -c archive | tar -xf - -C dir`.
        // This also gives ordinary text/config files real compression for free, unlike
        // uploading each one separately as raw base64.
        public async Task StartMultiUploadAsync(IReadOnlyList<StorageFile> files, string remoteDir, bool verifyMd5 = true)
        {
            if (_isUploading) return;
            if (files == null || files.Count == 0) return;
            _isUploading = true;

            string tempArchivePath = Path.Combine(Path.GetTempPath(), $"upload_{Guid.NewGuid():N}.tar.gz");
            string? remoteArchivePathForCleanup = null;

            try
            {
                StatusChanged?.Invoke($"Packaging {files.Count} file(s)...");
                ProgressChanged?.Invoke(0);

                using (var archiveFileStream = new FileStream(tempArchivePath, FileMode.Create, FileAccess.Write))
                using (var gzipStream = new GZipStream(archiveFileStream, CompressionLevel.Optimal))
                using (var tarWriter = new TarWriter(gzipStream, TarEntryFormat.Ustar, leaveOpen: true))
                {
                    foreach (var file in files)
                    {
                        using var readStream = await file.OpenStreamForReadAsync();
                        var entry = new UstarTarEntry(TarEntryType.RegularFile, file.Name)
                        {
                            DataStream = readStream
                        };
                        await tarWriter.WriteEntryAsync(entry);
                    }
                }

                byte[] archiveBytes = await File.ReadAllBytesAsync(tempArchivePath);

                string dir = remoteDir.TrimEnd('/');
                string remoteArchivePath = $"{dir}/_upload_{Guid.NewGuid():N}.tar.gz";
                remoteArchivePathForCleanup = remoteArchivePath;

                StatusChanged?.Invoke($"Uploading package ({archiveBytes.Length / 1024.0:F1} KB compressed)...");
                await UploadBytesAsBase64Async(archiveBytes, remoteArchivePath, 0, 85, verifyMd5);

                StatusChanged?.Invoke("Extracting archive on device...");
                ProgressChanged?.Invoke(95);

                string escapedDir = dir.Replace("'", "\\'");
                string escapedArchive = remoteArchivePath.Replace("'", "\\'");
                _serialService.Write(
                    $"mkdir -p '{escapedDir}' && gunzip -c '{escapedArchive}' | tar -xf - -C '{escapedDir}' && rm -f '{escapedArchive}'",
                    "lf");
                await Task.Delay(500 + files.Count * 50);

                ProgressChanged?.Invoke(100);
                StatusChanged?.Invoke($"Upload Complete! ({files.Count} file(s))");
                _shellExecutionService?.LogMessage($"--- Multi-file upload successful: {files.Count} file(s) -> {remoteDir} ---", "info");
                UploadComplete?.Invoke();
            }
            catch (Exception ex)
            {
                RaiseError($"Multi-file upload failed: {ex.Message}");

                // Best-effort: remove a possibly corrupt/partial archive left on the
                // device (e.g. after an md5 mismatch aborted before extraction ran).
                if (remoteArchivePathForCleanup != null)
                {
                    string escaped = remoteArchivePathForCleanup.Replace("'", "\\'");
                    _serialService.Write($"rm -f '{escaped}'", "lf");
                }
            }
            finally
            {
                _isUploading = false;
                try { if (File.Exists(tempArchivePath)) File.Delete(tempArchivePath); } catch { /* best-effort cleanup */ }
            }
        }

        public void StartDownload(string remotePath, StorageFile localFile)
        {
            if (_isDownloading || _isCapturingSize) return;

            _downloadPath = remotePath;
            _localDownloadFile = localFile;
            _expectedDownloadSize = 0;
            _expectedBase64Size = 0;
            _hasExpectedSize = false;
            _downloadedBase64Length = 0;

            _isCapturingSize = true;
            _sizeBuffer.Clear();

            StatusChanged?.Invoke("Retrieving file size...");
            ProgressChanged?.Invoke(-1); // Indeterminate progress

            // Set fallback timeout (3s) to proceed if size check hangs
            _sizeTimeoutTask = Task.Delay(3000).ContinueWith(t =>
            {
                if (_isCapturingSize)
                {
                    System.Diagnostics.Debug.WriteLine("Size retrieval timed out. Downloading directly.");
                    _isCapturingSize = false;
                    TriggerBase64Download();
                }
            });

            // Semicolon ensures following command prints even if preceding ls fails
            _serialService.Write($"echo \"===SIZE\"\"_START===\" ; ls -l \"{_downloadPath}\" ; echo \"===SIZE\"\"_END===\"", "lf");
        }

        private void TriggerBase64Download()
        {
            try
            {
                _localWriteStream = new FileStream(_localDownloadFile!.Path, FileMode.Create, FileAccess.Write, FileShare.Read);
            }
            catch (Exception ex)
            {
                RaiseError($"Failed to create local destination file: {ex.Message}");
                Cancel();
                return;
            }

            _isDownloading = true;
            _downloadBuffer.Clear();
            _base64CharBuffer.Clear();

            string status = _expectedDownloadSize > 0
                ? $"Requesting stream ({(double)_expectedDownloadSize / 1024.0:F1} KB)..."
                : "Requesting file stream...";
            StatusChanged?.Invoke(status);

            if (_hasExpectedSize)
            {
                ProgressChanged?.Invoke(0);
            }
            else
            {
                ProgressChanged?.Invoke(-1);
            }

            // Semicolon ensures download boundary end tag prints even if base64 fails
            _serialService.Write($"echo \"===DOWNLOAD\"\"_START===\" ; base64 \"{_downloadPath}\" ; echo \"===DOWNLOAD\"\"_END===\"", "lf");
        }

        public void FeedIncomingText(string text)
        {
            string cleanText = text.Replace("\r", "");

            if (_isCapturingSize)
            {
                _sizeBuffer.Append(cleanText);
                string buf = _sizeBuffer.ToString();
                int start = buf.LastIndexOf("===SIZE_START===");
                int end = buf.LastIndexOf("===SIZE_END===");
                if (start != -1 && end != -1 && end > start)
                {
                    _isCapturingSize = false;
                    string segment = buf.Substring(start + "===SIZE_START===".Length, end - (start + "===SIZE_START===".Length));
                    string clean = Regex.Replace(segment, @"\[\d{1,2}:\d{2}:\d{2}\s*(?:AM|PM)?\]", "", RegexOptions.IgnoreCase).Trim();

                    int size = -1;
                    var lines = clean.Split(new[] { '\n', '\r' }, StringSplitOptions.RemoveEmptyEntries);
                    foreach (var line in lines)
                    {
                        var parts = line.Trim().Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                        if (parts.Length >= 5 && Regex.IsMatch(parts[0], @"^[-dlrwxstST]{10}$"))
                        {
                            if (int.TryParse(parts[4], out int sizeCandidate) && sizeCandidate >= 0)
                            {
                                size = sizeCandidate;
                                break;
                            }
                        }
                    }

                    if (size >= 0)
                    {
                        _expectedDownloadSize = size;
                        _expectedBase64Size = (int)Math.Ceiling(size / 3.0) * 4;
                        _hasExpectedSize = true;
                    }
                    _sizeBuffer.Clear();
                    TriggerBase64Download();
                }
            }

            if (_isDownloading)
            {
                _downloadBuffer.Append(cleanText);
                string buf = _downloadBuffer.ToString();
                int start = buf.IndexOf("===DOWNLOAD_START===");
                if (start != -1)
                {
                    int contentStart = start + "===DOWNLOAD_START===".Length;
                    int end = buf.IndexOf("===DOWNLOAD_END===", contentStart);

                    if (end != -1)
                    {
                        _isDownloading = false;
                        StatusChanged?.Invoke("Processing final bytes...");
                        ProgressChanged?.Invoke(100);

                        string finalSegment = buf.Substring(contentStart, end - contentStart);
                        WriteBase64ToStream(finalSegment);

                        CloseWriteStream();
                        _shellExecutionService?.LogMessage("--- File download successful ---", "info");
                        DownloadComplete?.Invoke();

                        _downloadBuffer.Clear();
                    }
                    else
                    {
                        // We leave the last 40 characters in the buffer to avoid cutting the split end tag
                        int availableLength = buf.Length - contentStart;
                        if (availableLength > 40)
                        {
                            int processLength = availableLength - 40;
                            string segment = buf.Substring(contentStart, processLength);
                            WriteBase64ToStream(segment);

                            // Remove processed part from the buffer
                            _downloadBuffer.Remove(contentStart, processLength);

                            // Update streaming statistics progress
                            int received = _downloadedBase64Length;
                            if (_expectedBase64Size > 0)
                            {
                                int pct = Math.Min(100, (int)Math.Round((received / (double)_expectedBase64Size) * 100));
                                int decodedSize = Math.Min(_expectedDownloadSize, (int)Math.Round(received * 3.0 / 4.0));
                                ProgressChanged?.Invoke(pct);

                                if (_expectedDownloadSize < 1024)
                                {
                                    StatusChanged?.Invoke($"Streaming data: {decodedSize} B / {_expectedDownloadSize} B ({pct}%)");
                                }
                                else
                                {
                                    StatusChanged?.Invoke($"Streaming data: {Math.Round(decodedSize / 1024.0, 1)} KB / {Math.Round(_expectedDownloadSize / 1024.0, 1)} KB ({pct}%)");
                                }
                            }
                            else
                            {
                                ProgressChanged?.Invoke(-1);
                                StatusChanged?.Invoke($"Streaming data: {Math.Round(received / 1024.0, 1)} KB received...");
                            }
                        }
                    }
                }
            }
        }

        private void WriteBase64ToStream(string rawChunk)
        {
            // Remove timestamp annotations
            string stripped = Regex.Replace(rawChunk, @"\[\d{1,2}:\d{2}:\d{2}\s*(?:AM|PM)?\]", "", RegexOptions.IgnoreCase);
            // Remove everything except valid base64 chars
            string base64 = Regex.Replace(stripped, @"[^A-Za-z0-9+/=]", "");

            _base64CharBuffer.Append(base64);
            _downloadedBase64Length += base64.Length;

            while (_base64CharBuffer.Length >= 4)
            {
                string block = _base64CharBuffer.ToString(0, 4);
                try
                {
                    byte[] decoded = Convert.FromBase64String(block);
                    _localWriteStream?.Write(decoded, 0, decoded.Length);
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine($"Streaming base64 chunk decode error: {ex.Message}");
                }
                _base64CharBuffer.Remove(0, 4);
            }
        }

        private void CloseWriteStream()
        {
            try
            {
                if (_base64CharBuffer.Length > 0)
                {
                    string remaining = _base64CharBuffer.ToString();
                    if (remaining.Length % 4 != 0)
                    {
                        remaining = remaining.PadRight(remaining.Length + (4 - remaining.Length % 4), '=');
                    }
                    byte[] decoded = Convert.FromBase64String(remaining);
                    _localWriteStream?.Write(decoded, 0, decoded.Length);
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Final block decode error: {ex.Message}");
            }
            finally
            {
                if (_localWriteStream != null)
                {
                    _localWriteStream.Flush();
                    _localWriteStream.Close();
                    _localWriteStream.Dispose();
                    _localWriteStream = null;
                }
                _base64CharBuffer.Clear();
                _localDownloadFile = null;
            }
        }

        public void Cancel()
        {
            _isUploading = false;
            _isDownloading = false;
            _isCapturingSize = false;
            _sizeBuffer.Clear();
            _downloadBuffer.Clear();
            CloseWriteStream();
        }

#pragma warning disable SYSLIB0014 // Disable WebRequest obsolescence warning
        
        public async Task UploadFtpFileAsync(string host, string username, string password, string localPath, string remotePath)
        {
            StatusChanged?.Invoke("Uploading file via FTP...");
            ProgressChanged?.Invoke(-1);

            await Task.Run(() =>
            {
                var request = (FtpWebRequest)WebRequest.Create($"ftp://{host}{NormalizeFtpPath(remotePath)}");
                request.Method = WebRequestMethods.Ftp.UploadFile;
                request.Credentials = new NetworkCredential(username, password);
                request.UseBinary = true;
                request.KeepAlive = false;

                using (var fileStream = new FileStream(localPath, FileMode.Open, FileAccess.Read))
                using (var requestStream = request.GetRequestStream())
                {
                    byte[] buffer = new byte[8192];
                    int bytesRead;
                    long totalBytesRead = 0;
                    long fileLength = fileStream.Length;

                    while ((bytesRead = fileStream.Read(buffer, 0, buffer.Length)) > 0)
                    {
                        requestStream.Write(buffer, 0, bytesRead);
                        totalBytesRead += bytesRead;
                        if (fileLength > 0)
                        {
                            int pct = (int)((totalBytesRead / (double)fileLength) * 100);
                            ProgressChanged?.Invoke(pct);
                        }
                    }
                }

                using (var response = (FtpWebResponse)request.GetResponse())
                {
                    System.Diagnostics.Debug.WriteLine($"Upload response: {response.StatusDescription}");
                }
            });

            StatusChanged?.Invoke("Upload Complete!");
            ProgressChanged?.Invoke(100);
        }

        public async Task DownloadFtpFileAsync(string host, string username, string password, string remotePath, string localPath)
        {
            StatusChanged?.Invoke("Downloading file via FTP...");
            ProgressChanged?.Invoke(-1);

            await Task.Run(() =>
            {
                var request = (FtpWebRequest)WebRequest.Create($"ftp://{host}{NormalizeFtpPath(remotePath)}");
                request.Method = WebRequestMethods.Ftp.DownloadFile;
                request.Credentials = new NetworkCredential(username, password);
                request.UseBinary = true;
                request.KeepAlive = false;

                using (var response = (FtpWebResponse)request.GetResponse())
                using (var responseStream = response.GetResponseStream())
                using (var fileStream = new FileStream(localPath, FileMode.Create, FileAccess.Write, FileShare.Read))
                {
                    byte[] buffer = new byte[8192];
                    int bytesRead;
                    while ((bytesRead = responseStream.Read(buffer, 0, buffer.Length)) > 0)
                    {
                        fileStream.Write(buffer, 0, bytesRead);
                    }
                }
            });

            StatusChanged?.Invoke("Download Complete!");
            ProgressChanged?.Invoke(100);
        }

        public async Task UploadFtpDirectoryAsync(string host, string username, string password, string localDirPath, string remoteDirPath)
        {
            StatusChanged?.Invoke($"Uploading directory to {remoteDirPath}...");
            ProgressChanged?.Invoke(-1);

            await Task.Run(async () =>
            {
                await UploadFtpDirectoryRecursive(host, username, password, localDirPath, remoteDirPath);
            });

            StatusChanged?.Invoke("Directory Upload Complete!");
            ProgressChanged?.Invoke(100);
        }

        private async Task UploadFtpDirectoryRecursive(string host, string username, string password, string localDirPath, string remoteDirPath)
        {
            try
            {
                var request = (FtpWebRequest)WebRequest.Create($"ftp://{host}{NormalizeFtpPath(remoteDirPath)}");
                request.Method = WebRequestMethods.Ftp.MakeDirectory;
                request.Credentials = new NetworkCredential(username, password);
                using (var response = (FtpWebResponse)request.GetResponse()) { }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"MakeDirectory warning: {ex.Message}");
            }

            var files = Directory.GetFiles(localDirPath);
            foreach (var file in files)
            {
                string fileName = Path.GetFileName(file);
                string remoteFilePath = remoteDirPath + (remoteDirPath.EndsWith("/") ? "" : "/") + fileName;
                StatusChanged?.Invoke($"Uploading: {fileName}...");

                var request = (FtpWebRequest)WebRequest.Create($"ftp://{host}{NormalizeFtpPath(remoteFilePath)}");
                request.Method = WebRequestMethods.Ftp.UploadFile;
                request.Credentials = new NetworkCredential(username, password);
                request.UseBinary = true;
                request.KeepAlive = false;

                using (var fileStream = new FileStream(file, FileMode.Open, FileAccess.Read))
                using (var requestStream = request.GetRequestStream())
                {
                    byte[] buffer = new byte[8192];
                    int bytesRead;
                    while ((bytesRead = fileStream.Read(buffer, 0, buffer.Length)) > 0)
                    {
                        requestStream.Write(buffer, 0, bytesRead);
                    }
                }

                using (var response = (FtpWebResponse)request.GetResponse()) { }
            }

            var subdirs = Directory.GetDirectories(localDirPath);
            foreach (var subdir in subdirs)
            {
                string dirName = Path.GetFileName(subdir);
                string remoteSubdirPath = remoteDirPath + (remoteDirPath.EndsWith("/") ? "" : "/") + dirName;
                await UploadFtpDirectoryRecursive(host, username, password, subdir, remoteSubdirPath);
            }
        }

        public async Task DownloadFtpDirectoryAsync(string host, string username, string password, string remoteDirPath, string localDirPath)
        {
            StatusChanged?.Invoke($"Downloading directory from {remoteDirPath}...");
            ProgressChanged?.Invoke(-1);

            await Task.Run(async () =>
            {
                await DownloadFtpDirectoryRecursive(host, username, password, remoteDirPath, localDirPath);
            });

            StatusChanged?.Invoke("Directory Download Complete!");
            ProgressChanged?.Invoke(100);
        }

        private async Task DownloadFtpDirectoryRecursive(string host, string username, string password, string remoteDirPath, string localDirPath)
        {
            if (!remoteDirPath.EndsWith("/"))
            {
                remoteDirPath += "/";
            }

            if (!Directory.Exists(localDirPath))
            {
                Directory.CreateDirectory(localDirPath);
            }

            var items = new List<(bool IsDirectory, string Name)>();
            try
            {
                var request = (FtpWebRequest)WebRequest.Create($"ftp://{host}{NormalizeFtpPath(remoteDirPath)}");
                request.Method = WebRequestMethods.Ftp.ListDirectoryDetails;
                request.Credentials = new NetworkCredential(username, password);
                request.KeepAlive = false;

                using (var response = (FtpWebResponse)request.GetResponse())
                using (var reader = new StreamReader(response.GetResponseStream()))
                {
                    string? line;
                    while ((line = reader.ReadLine()) != null)
                    {
                        if (ParseFtpListLine(line, out bool isDir, out string name))
                        {
                            if (name == "." || name == "..") continue;
                            items.Add((isDir, name));
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                throw new Exception($"Failed to list remote directory {remoteDirPath}: {ex.Message}", ex);
            }

            foreach (var item in items)
            {
                string remoteItemPath = remoteDirPath + (remoteDirPath.EndsWith("/") ? "" : "/") + item.Name;
                string localItemPath = Path.Combine(localDirPath, item.Name);

                if (item.IsDirectory)
                {
                    StatusChanged?.Invoke($"Entering folder: {item.Name}...");
                    await DownloadFtpDirectoryRecursive(host, username, password, remoteItemPath, localItemPath);
                }
                else
                {
                    StatusChanged?.Invoke($"Downloading file: {item.Name}...");
                    var request = (FtpWebRequest)WebRequest.Create($"ftp://{host}{NormalizeFtpPath(remoteItemPath)}");
                    request.Method = WebRequestMethods.Ftp.DownloadFile;
                    request.Credentials = new NetworkCredential(username, password);
                    request.UseBinary = true;
                    request.KeepAlive = false;

                    using (var response = (FtpWebResponse)request.GetResponse())
                    using (var responseStream = response.GetResponseStream())
                    using (var fileStream = new FileStream(localItemPath, FileMode.Create, FileAccess.Write, FileShare.Read))
                    {
                        byte[] buffer = new byte[8192];
                        int bytesRead;
                        while ((bytesRead = responseStream.Read(buffer, 0, buffer.Length)) > 0)
                        {
                            fileStream.Write(buffer, 0, bytesRead);
                        }
                    }
                }
            }
        }

        private bool ParseFtpListLine(string line, out bool isDirectory, out string name)
        {
            isDirectory = false;
            name = "";
            if (string.IsNullOrWhiteSpace(line)) return false;

            isDirectory = line.StartsWith("d");

            var parts = line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length >= 9)
            {
                name = string.Join(" ", parts, 8, parts.Length - 8);
                return true;
            }
            else if (parts.Length > 0)
            {
                name = parts[parts.Length - 1];
                return true;
            }
            return false;
        }

        public async Task<bool> IsRemotePathDirectoryAsync(string host, string username, string password, string remotePath)
        {
            return await Task.Run(() =>
            {
                try
                {
                    // Attempt to get file size. On most FTP servers, this fails for directories with 550.
                    var request = (FtpWebRequest)WebRequest.Create($"ftp://{host}{NormalizeFtpPath(remotePath)}");
                    request.Method = WebRequestMethods.Ftp.GetFileSize;
                    request.Credentials = new NetworkCredential(username, password);
                    request.KeepAlive = false;

                    using (var response = (FtpWebResponse)request.GetResponse())
                    {
                        return false; // Success means it's a file
                    }
                }
                catch (WebException ex)
                {
                    if (ex.Response is FtpWebResponse response && response.StatusCode == FtpStatusCode.ActionNotTakenFileUnavailable)
                    {
                        try
                        {
                            var listRequest = (FtpWebRequest)WebRequest.Create($"ftp://{host}{NormalizeFtpPath(remotePath)}");
                            listRequest.Method = WebRequestMethods.Ftp.ListDirectoryDetails;
                            listRequest.Credentials = new NetworkCredential(username, password);
                            listRequest.KeepAlive = false;

                            using (var listResponse = (FtpWebResponse)listRequest.GetResponse())
                            {
                                return true; // Listing succeeds means it is a directory
                            }
                        }
                        catch
                        {
                            return false;
                        }
                    }
                    return false;
                }
                catch
                {
                    return false;
                }
            });
        }

        public async Task<bool> CheckIfRemotePathIsDirectoryAsync(string remotePath, ShellExecutionService shell)
        {
            string escapedPath = remotePath.Replace("'", "\\'");
            string cmd = $"echo ===TYPE_ST\"\"ART=== ; test -d '{escapedPath}' && echo \"DIR\" || echo \"FILE\" ; echo ===TYPE_E\"\"ND===";
            string result = await shell.RunHiddenQueryAsync(cmd, "===TYPE_START===", "===TYPE_END===", 1000);
            return result == "DIR";
        }

        public async Task<List<string>> GetRemoteFilesListAsync(string remotePath, ShellExecutionService shell)
        {
            string escapedPath = remotePath.Replace("'", "\\'");
            string cmd = $"echo ===FIND_ST\"\"ART=== ; find '{escapedPath}' -type f ; echo ===FIND_E\"\"ND===";
            string listResult = await shell.RunHiddenQueryAsync(cmd, "===FIND_START===", "===FIND_END===", 4000);

            var files = new List<string>();
            var lines = listResult.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
            foreach (var line in lines)
            {
                string trimmed = line.Trim();
                if (!string.IsNullOrEmpty(trimmed) && trimmed.StartsWith("/") && !trimmed.Contains("==="))
                {
                    files.Add(trimmed);
                }
            }
            return files;
        }

        public async Task DownloadFtpDirectoryIterativeAsync(string host, string username, string password, string remoteDirPath, string localDirPath, List<string> remoteFiles)
        {
            string remoteDirName = Path.GetFileName(remoteDirPath.TrimEnd('/'));
            if (string.IsNullOrEmpty(remoteDirName))
            {
                remoteDirName = "downloaded_folder";
            }
            string targetLocalDirPath = Path.Combine(localDirPath, remoteDirName);

            int count = 0;
            foreach (var remoteFile in remoteFiles)
            {
                count++;
                string relative = remoteFile;
                if (remoteFile.StartsWith(remoteDirPath))
                {
                    relative = remoteFile.Substring(remoteDirPath.Length);
                    if (relative.StartsWith("/")) relative = relative.Substring(1);
                }

                string localFilePath = Path.Combine(targetLocalDirPath, relative.Replace("/", "\\"));
                string localFileDir = Path.GetDirectoryName(localFilePath)!;

                if (!Directory.Exists(localFileDir))
                {
                    Directory.CreateDirectory(localFileDir);
                }

                StatusChanged?.Invoke($"Downloading file {count}/{remoteFiles.Count}: {Path.GetFileName(remoteFile)}...");
                ProgressChanged?.Invoke((count / (double)remoteFiles.Count) * 100);

                await DownloadFtpFileAsync(host, username, password, remoteFile, localFilePath);
            }
        }

        public async Task RunFtpDownloadOrchestrationAsync(
            string host,
            string username,
            string password,
            string remotePath,
            ShellExecutionService shell,
            Func<bool, string, Task<string?>> getLocalPathCallback)
        {
            string hostIp = host;
            int colon = host.IndexOf(':');
            if (colon != -1)
            {
                hostIp = host.Substring(0, colon);
            }

            StatusChanged?.Invoke("Checking remote path type...");
            ProgressChanged?.Invoke(-1);

            bool isDirectory = await CheckIfRemotePathIsDirectoryAsync(remotePath, shell);
            string? localPath = null;

            if (isDirectory)
            {
                StatusChanged?.Invoke("Listing remote files...");
                var remoteFiles = await GetRemoteFilesListAsync(remotePath, shell);
                if (remoteFiles.Count == 0)
                {
                    throw new Exception("No files found or file listing command timed out.");
                }

                localPath = await getLocalPathCallback(true, remotePath);
                if (string.IsNullOrEmpty(localPath))
                {
                    StatusChanged?.Invoke("Download cancelled by user.");
                    return;
                }

                StatusChanged?.Invoke($"Starting FTP Directory Download: {remotePath} ({remoteFiles.Count} files) -> {Path.GetFileName(localPath)}...");
                await DownloadFtpDirectoryIterativeAsync(hostIp, username, password, remotePath, localPath, remoteFiles);
            }
            else
            {
                localPath = await getLocalPathCallback(false, remotePath);
                if (string.IsNullOrEmpty(localPath))
                {
                    StatusChanged?.Invoke("Download cancelled by user.");
                    return;
                }

                StatusChanged?.Invoke($"Starting FTP File Download: {remotePath} -> {Path.GetFileName(localPath)}...");
                ProgressChanged?.Invoke(-1);
                await DownloadFtpFileAsync(hostIp, username, password, remotePath, localPath);
            }

            StatusChanged?.Invoke("FTP Download Successful!");
            ProgressChanged?.Invoke(100);
        }

        public async Task RunFtpUploadOrchestrationAsync(
            string host,
            string username,
            string password,
            string remotePath,
            string? localFilePath,
            string? localDirectoryPath)
        {
            try
            {
                string hostIp = host;
                int colon = host.IndexOf(':');
                if (colon != -1)
                {
                    hostIp = host.Substring(0, colon);
                }

                if (!string.IsNullOrEmpty(localDirectoryPath))
                {
                    StatusChanged?.Invoke($"Starting FTP Directory Upload: {Path.GetFileName(localDirectoryPath)} -> {remotePath}...");
                    await UploadFtpDirectoryAsync(hostIp, username, password, localDirectoryPath, remotePath);
                }
                else if (!string.IsNullOrEmpty(localFilePath))
                {
                    StatusChanged?.Invoke($"Starting FTP File Upload: {Path.GetFileName(localFilePath)} -> {remotePath}...");
                    await UploadFtpFileAsync(hostIp, username, password, localFilePath, remotePath);
                }
                else
                {
                    throw new ArgumentException("No file or directory specified for upload.");
                }

                StatusChanged?.Invoke("FTP Upload Successful!");
                ProgressChanged?.Invoke(100);

                string source = !string.IsNullOrEmpty(localDirectoryPath) ? localDirectoryPath : (localFilePath ?? "");
                _shellExecutionService?.LogMessage($"--- FTP Upload Successful: {Path.GetFileName(source)} -> {remotePath} ---", "info");
            }
            catch (Exception ex)
            {
                RaiseError($"FTP Upload failed: {ex.Message}");
            }
        }

        private string NormalizeFtpPath(string path)
        {
            path = path.Replace("\\", "/");
            // To specify an absolute path for FtpWebRequest, the first path segment must start with '//'.
            if (path.StartsWith("/"))
            {
                return "//" + path.Substring(1);
            }
            return "/" + path;
        }

        public async Task StartInteractiveFtpDownloadAsync(string host, string username, string password, string remotePath)
        {
            try
            {
                if (_dialogService == null || _shellExecutionService == null)
                {
                    throw new InvalidOperationException("Dependencies not initialized.");
                }

                string hostIp = host;
                int colon = host.IndexOf(':');
                if (colon != -1)
                {
                    hostIp = host.Substring(0, colon);
                }

                StatusChanged?.Invoke("Checking remote path type...");
                ProgressChanged?.Invoke(-1);

                bool isDirectory = await CheckIfRemotePathIsDirectoryAsync(remotePath, _shellExecutionService);
                string? localPath = null;

                if (isDirectory)
                {
                    StatusChanged?.Invoke("Listing remote files...");
                    var remoteFiles = await GetRemoteFilesListAsync(remotePath, _shellExecutionService);
                    if (remoteFiles.Count == 0)
                    {
                        throw new Exception("No files found or file listing command timed out.");
                    }

                    localPath = await _dialogService.PickFolderAsync();
                    if (string.IsNullOrEmpty(localPath))
                    {
                        StatusChanged?.Invoke("Download cancelled by user.");
                        return;
                    }

                    StatusChanged?.Invoke($"Starting FTP Directory Download: {remotePath} ({remoteFiles.Count} files) -> {Path.GetFileName(localPath)}...");
                    await DownloadFtpDirectoryIterativeAsync(hostIp, username, password, remotePath, localPath, remoteFiles);
                }
                else
                {
                    string fileName = Path.GetFileName(remotePath);
                    if (string.IsNullOrEmpty(fileName)) fileName = "downloaded_file";
                    string ext = Path.GetExtension(fileName);
                    if (string.IsNullOrEmpty(ext)) ext = ".bin";

                    localPath = await _dialogService.PickSaveFileAsync(fileName, ext);
                    if (string.IsNullOrEmpty(localPath))
                    {
                        StatusChanged?.Invoke("Download cancelled by user.");
                        return;
                    }

                    StatusChanged?.Invoke($"Starting FTP File Download: {remotePath} -> {Path.GetFileName(localPath)}...");
                    ProgressChanged?.Invoke(-1);
                    await DownloadFtpFileAsync(hostIp, username, password, remotePath, localPath);
                }

                StatusChanged?.Invoke("FTP Download Successful!");
                ProgressChanged?.Invoke(100);
                _shellExecutionService.LogMessage($"--- FTP Download Successful: {remotePath} ---", "info");
            }
            catch (Exception ex)
            {
                RaiseError($"FTP Download failed: {ex.Message}");
            }
        }

#pragma warning restore SYSLIB0014
    }
}
