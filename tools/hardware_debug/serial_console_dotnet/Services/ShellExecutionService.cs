using System;
using System.Collections.Generic;
using System.Threading.Tasks;

namespace serial_console_dotnet.Services
{
    public class ShellExecutionService
    {
        private readonly ConnectionService _serialService;
        private TaskCompletionSource<string>? _currentTaskTcs;
        private string _captureBuffer = "";
        private string _startMarker = "";
        private string _endMarker = "";
        private bool _isExecuting = false;

        public bool IsExecuting => _isExecuting;

        public event Action<bool>? ExecutionStateChanged;

        public ShellExecutionService(ConnectionService serialService)
        {
            _serialService = serialService;
        }

        public async Task<string> RunHiddenQueryAsync(string command, string startMarker, string endMarker, int timeoutMs = 3000)
        {
            if (_isExecuting)
            {
                throw new InvalidOperationException("Another operation is already running.");
            }

            _isExecuting = true;
            ExecutionStateChanged?.Invoke(true);

            _captureBuffer = "";
            _startMarker = startMarker;
            _endMarker = endMarker;
            _currentTaskTcs = new TaskCompletionSource<string>();

            // Send command
            _serialService.Write(command, "lf");

            try
            {
                var completedTask = await Task.WhenAny(_currentTaskTcs.Task, Task.Delay(timeoutMs));
                if (completedTask == _currentTaskTcs.Task)
                {
                    return await _currentTaskTcs.Task;
                }
                else
                {
                    throw new TimeoutException("Device did not respond to the query command.");
                }
            }
            finally
            {
                _isExecuting = false;
                ExecutionStateChanged?.Invoke(false);
                _currentTaskTcs = null;
            }
        }

        /// <summary>
        /// Feeds incoming text chunk. Returns true if it was intercepted/handled silently.
        /// </summary>
        public bool HandleIncomingText(string text)
        {
            if (!_isExecuting || _currentTaskTcs == null) return false;

            _captureBuffer += text;

            int startIdx = _captureBuffer.IndexOf(_startMarker);
            int endIdx = _captureBuffer.IndexOf(_endMarker, startIdx != -1 ? startIdx : 0);

            if (startIdx != -1 && endIdx != -1 && endIdx > startIdx)
            {
                string content = _captureBuffer.Substring(startIdx + _startMarker.Length, endIdx - (startIdx + _startMarker.Length)).Trim();
                _currentTaskTcs.TrySetResult(content);
            }

            return true; // Suppress output to terminal while query is running
        }
        public async Task<string> GetCurrentDirectoryAsync()
        {
            try
            {
                string dir = await RunHiddenQueryAsync("echo ===PWD_ST\"\"ART=== ; pwd ; echo ===PWD_E\"\"ND===", "===PWD_START===", "===PWD_END===", 1000);
                dir = System.Text.RegularExpressions.Regex.Replace(dir, @"\[\d{1,2}:\d{2}:\d{2}\s*(?:AM|PM)?\]", "", System.Text.RegularExpressions.RegexOptions.IgnoreCase).Trim();
                if (!string.IsNullOrEmpty(dir))
                {
                    if (!dir.EndsWith("/")) dir += "/";
                    return dir;
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to query directory: {ex.Message}");
            }
            return "/tmp/";
        }

        public async Task<List<(bool IsDirectory, string Name, long Size, bool IsSymlink, string SymlinkTarget)>> ListDirectoryContentsAsync(string path)
        {
            var items = new List<(bool IsDirectory, string Name, long Size, bool IsSymlink, string SymlinkTarget)>();
            try
            {
                string escapedPath = path.Replace("'", "\\'");
                // Use ls -lp to get file sizes as well as trailing slashes for directories
                string cmd = $"echo ===LS_ST\"\"ART=== ; \\ls -lp --color=never '{escapedPath}' 2>/dev/null || \\ls -lp '{escapedPath}' ; echo ===LS_E\"\"ND===";
                string output = await RunHiddenQueryAsync(cmd, "===LS_START===", "===LS_END===", 2000);
                
                var lines = output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
                foreach (var line in lines)
                {
                    string rawLine = line.Trim();
                    if (string.IsNullOrEmpty(rawLine) || rawLine.StartsWith("===") || rawLine.StartsWith("total ") || rawLine.StartsWith("ls: ")) continue;

                    // Strip ANSI escape sequences
                    string cleanLine = System.Text.RegularExpressions.Regex.Replace(rawLine, @"\x1b\[[0-9;]*[a-zA-Z]", "");
                    cleanLine = System.Text.RegularExpressions.Regex.Replace(cleanLine, @"\u001b\[[0-9;]*[a-zA-Z]", "");
                    cleanLine = cleanLine.Trim();

                    if (string.IsNullOrEmpty(cleanLine)) continue;

                    // Parse long listing columns
                    var tokens = cleanLine.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                    if (tokens.Length < 5) continue;

                    bool isDir = tokens[0].StartsWith("d");

                    // Find the date/time column to locate the size column
                    int dateIndex = -1;
                    for (int i = 1; i < tokens.Length - 1; i++)
                    {
                        if (IsDateToken(tokens, i))
                        {
                            dateIndex = i;
                            break;
                        }
                    }

                    long size = 0;
                    int nameStartIndex = -1;

                    if (dateIndex >= 3)
                    {
                        if (tokens[dateIndex - 1].Contains("-"))
                        {
                            long.TryParse(tokens[dateIndex - 2], out size);
                        }
                        else
                        {
                            long.TryParse(tokens[dateIndex - 3], out size);
                        }
                        nameStartIndex = dateIndex + 1;
                    }
                    else
                    {
                        // Fallback if date/time parsing failed
                        // Try to find the first numeric token from the right (excluding name)
                        for (int i = tokens.Length - 2; i >= 1; i--)
                        {
                            if (long.TryParse(tokens[i], out size))
                            {
                                nameStartIndex = i + 1;
                                break;
                            }
                        }
                    }

                    if (nameStartIndex == -1 || nameStartIndex >= tokens.Length) continue;

                    string nameToken = tokens[nameStartIndex];
                    int indexInOriginal = cleanLine.IndexOf(nameToken);
                    string name = indexInOriginal != -1 ? cleanLine.Substring(indexInOriginal) : nameToken;
                    name = name.Trim();

                    bool isSymlink = tokens[0].StartsWith("l");
                    string symlinkTarget = "";

                    if (name.EndsWith("/"))
                    {
                        name = name.Substring(0, name.Length - 1);
                        isDir = true;
                    }

                    if (name.Contains(" -> "))
                    {
                        var parts = name.Split(new[] { " -> " }, 2, StringSplitOptions.None);
                        name = parts[0].Trim();
                        symlinkTarget = parts[1].Trim();
                        isSymlink = true;
                    }

                    if (name == "." || name == "..") continue;

                    items.Add((isDir, name, isDir ? 0 : size, isSymlink, symlinkTarget));
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to list directory contents: {ex.Message}");
            }
            return items;
        }

        public event Action<string, string>? InfoMessageLogged;

        public void LogMessage(string message, string level = "info")
        {
            InfoMessageLogged?.Invoke(message, level);
        }

        public async Task<string> GetCanonicalPathAsync(string path)
        {
            try
            {
                string escapedPath = path.Replace("'", "\\'");
                // Use a subshell to cd -P (physically resolve symlinks) and pwd -P (print physical path).
                // This is a shell-builtin sequence supported by POSIX sh/busybox, avoiding dependencies on readlink.
                string cmd = $"echo ===CAN_ST\"\"ART=== ; (cd -P '{escapedPath}' && pwd -P) 2>/dev/null || (cd '{escapedPath}' && pwd) 2>/dev/null || echo '{escapedPath}' ; echo ===CAN_E\"\"ND===";
                string output = await RunHiddenQueryAsync(cmd, "===CAN_START===", "===CAN_END===", 2000);
                string resolved = output.Trim();
                if (!string.IsNullOrEmpty(resolved) && resolved.StartsWith("/"))
                {
                    return resolved;
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to resolve canonical path: {ex.Message}");
            }
            return path;
        }

        private static readonly string[] Months = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

        private static bool IsDateToken(string[] tokens, int i)
        {
            if (i < 2) return false;

            // Case 1: Time format (e.g., "00:01")
            if (tokens[i].Contains(":"))
            {
                // Must be preceded by day number and month name (e.g., "Jan 1 00:01")
                if (i >= 2)
                {
                    string m = tokens[i - 2];
                    foreach (var month in Months)
                    {
                        if (string.Equals(month, m, StringComparison.OrdinalIgnoreCase)) return true;
                    }
                }
                // Or preceded by date string (e.g., "2000-01-01 00:01")
                if (i >= 1 && tokens[i - 1].Contains("-"))
                {
                    return true;
                }
            }

            // Case 2: Year format (e.g., "2026")
            if (tokens[i].Length == 4 && int.TryParse(tokens[i], out int year) && year >= 1970 && year <= 2099)
            {
                // Must be preceded by day number and month name (e.g., "Jan 1 2026")
                if (i >= 2)
                {
                    string m = tokens[i - 2];
                    foreach (var month in Months)
                    {
                        if (string.Equals(month, m, StringComparison.OrdinalIgnoreCase)) return true;
                    }
                }
            }

            return false;
        }
    }
}
