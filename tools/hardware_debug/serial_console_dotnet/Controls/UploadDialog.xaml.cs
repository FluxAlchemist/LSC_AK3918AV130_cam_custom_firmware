using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace serial_console_dotnet.Controls
{
    public sealed partial class UploadDialog : UserControl
    {
        // Non-FTP (base64-over-UART/Telnet) uploads support multi-select; FTP folder
        // upload keeps its own single _selectedFolder path below and is untouched.
        private List<StorageFile> _selectedFiles = new();
        private StorageFolder? _selectedFolder = null;
        private bool _isUploading = false;

        // Single-file exact-path upload — fired when exactly one file is selected/dropped
        // (also used by ShowWithFile's terminal-drag-drop flow, which always targets an
        // exact remote file path rather than a directory). Last bool is VerifyMd5.
        public event Action<StorageFile, string, bool>? UploadRequested;
        // Multi-file upload — fired when 2+ files are selected/dropped. targetPath is
        // treated as a remote directory; FileTransferService packages+gzips them together.
        // Last bool is VerifyMd5.
        public event Action<IReadOnlyList<StorageFile>, string, bool>? MultiUploadRequested;
        public event Action<StorageFile?, StorageFolder?, string, string, string>? FtpUploadRequested;
        public event Action? UploadCancelled;

        public IntPtr ParentWindowHwnd { get; set; } = IntPtr.Zero;

        public bool IsFtpMode => UseFtpToggle.IsOn;
        public string FtpUsername => FtpUsernameTextBox.Text;
        public string FtpPassword => FtpPasswordBox.Password;
        public bool VerifyMd5 => VerifyMd5Toggle.IsOn;

        public UploadDialog()
        {
            this.InitializeComponent();
        }

        public void Show()
        {
            _selectedFiles.Clear();
            _selectedFolder = null;
            _isUploading = false;
            UseFtpToggle.IsOn = (FtpSwitchPanel.Visibility == Visibility.Visible);
            UploadFileNameText.Text = "Click to select file/folder...";
            UploadFileSizeText.Text = "Drag & drop here";
            UploadProgressPanel.Visibility = Visibility.Collapsed;
            UploadProgressBar.Value = 0;
            StartUploadBtn.IsEnabled = false;
            UploadTargetPathTextBox.IsEnabled = true;
            UploadDropZoneBorder.IsHitTestVisible = true;

            this.Visibility = Visibility.Visible;
        }

        public void Hide()
        {
            this.Visibility = Visibility.Collapsed;
        }

        public async void ShowWithFile(StorageFile file, string targetPath)
        {
            _selectedFiles = new List<StorageFile> { file };
            _selectedFolder = null;
            _isUploading = false;
            UseFtpToggle.IsOn = (FtpSwitchPanel.Visibility == Visibility.Visible);
            UploadFileNameText.Text = file.Name;

            try
            {
                var props = await file.GetBasicPropertiesAsync();
                UploadFileSizeText.Text = $"({Math.Round(props.Size / 1024.0, 1)} KB) - Click to change";
            }
            catch
            {
                UploadFileSizeText.Text = "Click to change";
            }

            UploadTargetPathTextBox.Text = targetPath;
            UploadProgressPanel.Visibility = Visibility.Collapsed;
            UploadProgressBar.Value = 0;
            StartUploadBtn.IsEnabled = true;
            UploadTargetPathTextBox.IsEnabled = true;
            UploadDropZoneBorder.IsHitTestVisible = true;

            this.Visibility = Visibility.Visible;
        }

        // Multi-file drop-onto-terminal entry point (mirrors ShowWithFile above, but for
        // 2+ files at once). targetDir is a directory (always trailing-slash terminated
        // by ShellExecutionService.GetCurrentDirectoryAsync) — files keep their own names
        // inside it, same as the dialog's own multi-select dropzone.
        public async void ShowWithFiles(IReadOnlyList<StorageFile> files, string targetDir)
        {
            _selectedFolder = null;
            _isUploading = false;
            UseFtpToggle.IsOn = (FtpSwitchPanel.Visibility == Visibility.Visible);
            UploadTargetPathTextBox.Text = targetDir;
            UploadProgressPanel.Visibility = Visibility.Collapsed;
            UploadProgressBar.Value = 0;
            UploadTargetPathTextBox.IsEnabled = true;
            UploadDropZoneBorder.IsHitTestVisible = true;

            await SelectUploadFilesAsync(files);

            this.Visibility = Visibility.Visible;
        }

        public void SetTargetPath(string path)
        {
            UploadTargetPathTextBox.Text = path;
        }

        public void SetProgress(double progress)
        {
            UploadProgressPanel.Visibility = Visibility.Visible;
            if (progress < 0)
            {
                UploadProgressBar.IsIndeterminate = true;
            }
            else
            {
                UploadProgressBar.IsIndeterminate = false;
                UploadProgressBar.Value = progress;
            }
        }

        public void SetStatus(string status)
        {
            UploadProgressPanel.Visibility = Visibility.Visible;
            UploadStatusText.Text = status;
        }

        public void SetUploadingState(bool isUploading)
        {
            _isUploading = isUploading;
            UploadTargetPathTextBox.IsEnabled = !isUploading;
            UploadDropZoneBorder.IsHitTestVisible = !isUploading;
            UseFtpToggle.IsEnabled = !isUploading;
            FtpUsernameTextBox.IsEnabled = !isUploading;
            FtpPasswordBox.IsEnabled = !isUploading;
            StartUploadBtn.IsEnabled = !isUploading && (_selectedFiles.Count > 0 || _selectedFolder != null);
        }

        public void SetIsTelnet(bool isTelnet)
        {
            FtpSwitchPanel.Visibility = isTelnet ? Visibility.Visible : Visibility.Collapsed;
            if (!isTelnet)
            {
                UseFtpToggle.IsOn = false;
			}
			else
			{
				UseFtpToggle.IsOn = true;
			}
        }

        public void SetFtpCredentials(string username, string password)
        {
            FtpUsernameTextBox.Text = string.IsNullOrEmpty(username) ? "root" : username;
            FtpPasswordBox.Password = password ?? "";
        }

        private void UseFtpToggle_Toggled(object sender, RoutedEventArgs e)
        {
            FtpCredentialsPanel.Visibility = UseFtpToggle.IsOn ? Visibility.Visible : Visibility.Collapsed;
            // FTP already rides reliable TCP and doesn't go through the base64/md5 path at all.
            VerifyMd5Panel.Visibility = UseFtpToggle.IsOn ? Visibility.Collapsed : Visibility.Visible;

            // Adjust texts
            if (UseFtpToggle.IsOn)
            {
                UploadFileNameText.Text = _selectedFiles.Count > 0 ? DescribeSelectedFiles() : (_selectedFolder != null ? _selectedFolder.Name : "Click to select file/folder...");
                UploadFileSizeText.Text = _selectedFolder != null ? "(Directory) - Click to change" : "Drag & drop files or folders here";
            }
            else
            {
                _selectedFolder = null;
                StartUploadBtn.IsEnabled = _selectedFiles.Count > 0;
                UploadFileNameText.Text = _selectedFiles.Count > 0 ? DescribeSelectedFiles() : "Click to select file(s) for upload...";
                UploadFileSizeText.Text = "Drag & drop one or more files here";
            }
        }

        private string DescribeSelectedFiles()
        {
            return _selectedFiles.Count == 1 ? _selectedFiles[0].Name : $"{_selectedFiles.Count} files selected";
        }

        private void CloseUploadDialog_Click(object sender, RoutedEventArgs e)
        {
            if (_isUploading) return;
            UploadCancelled?.Invoke();
            Hide();
        }

        private void DropZone_Click(object sender, PointerRoutedEventArgs e)
        {
            if (_isUploading) return;

            if (UseFtpToggle.IsOn)
            {
                // Show flyout menu choice
                FlyoutBase.ShowAttachedFlyout(UploadDropZoneBorder);
            }
            else
            {
                PickFiles();
            }
        }

        private async void PickFiles()
        {
            var picker = new FileOpenPicker();
            picker.ViewMode = PickerViewMode.List;
            picker.SuggestedStartLocation = PickerLocationId.ComputerFolder;
            picker.FileTypeFilter.Add("*");

            if (ParentWindowHwnd != IntPtr.Zero)
            {
                WinRT.Interop.InitializeWithWindow.Initialize(picker, ParentWindowHwnd);
            }

            IReadOnlyList<StorageFile> files = await picker.PickMultipleFilesAsync();
            if (files != null && files.Count > 0)
            {
                await SelectUploadFilesAsync(files);
            }
        }

        private async void SelectFile_Click(object sender, RoutedEventArgs e)
        {
            PickFiles();
        }

        private async void SelectFolder_Click(object sender, RoutedEventArgs e)
        {
            var picker = new FolderPicker();
            picker.SuggestedStartLocation = PickerLocationId.ComputerFolder;
            picker.FileTypeFilter.Add("*");

            if (ParentWindowHwnd != IntPtr.Zero)
            {
                WinRT.Interop.InitializeWithWindow.Initialize(picker, ParentWindowHwnd);
            }

            StorageFolder folder = await picker.PickSingleFolderAsync();
            if (folder != null)
            {
                SelectUploadFolder(folder);
            }
        }

        private void DropZone_DragOver(object sender, DragEventArgs e)
        {
            if (!_isUploading)
            {
                e.AcceptedOperation = DataPackageOperation.Copy;
            }
        }

        private async void DropZone_Drop(object sender, DragEventArgs e)
        {
            if (_isUploading) return;

            if (e.DataView.Contains(StandardDataFormats.StorageItems))
            {
                var items = await e.DataView.GetStorageItemsAsync();
                if (items.Count == 0) return;

                if (items.Count == 1 && items[0] is StorageFolder folder && UseFtpToggle.IsOn)
                {
                    SelectUploadFolder(folder);
                    return;
                }

                // Multiple items, or FTP is off (folders require FTP): take every
                // dropped StorageFile and ignore any folders in the mix.
                var files = items.OfType<StorageFile>().ToList();
                if (files.Count > 0)
                {
                    await SelectUploadFilesAsync(files);
                }
            }
        }

        private async Task SelectUploadFilesAsync(IReadOnlyList<StorageFile> files)
        {
            _selectedFiles = files.ToList();
            _selectedFolder = null;
            UploadFileNameText.Text = DescribeSelectedFiles();

            try
            {
                long totalBytes = 0;
                foreach (var f in _selectedFiles)
                {
                    var props = await f.GetBasicPropertiesAsync();
                    totalBytes += (long)props.Size;
                }
                UploadFileSizeText.Text = $"({Math.Round(totalBytes / 1024.0, 1)} KB total) - Click to change";
            }
            catch
            {
                UploadFileSizeText.Text = "Click to change";
            }

            StartUploadBtn.IsEnabled = true;

            // Single file: prefill with the exact remote file path, matching the
            // pre-existing behavior. Multiple files: the target path is treated as a
            // remote directory (files keep their own names inside it), so leave it alone.
            if (_selectedFiles.Count == 1)
            {
                string currentPath = UploadTargetPathTextBox.Text;
                int lastSlash = currentPath.LastIndexOf('/');
                if (lastSlash != -1)
                {
                    UploadTargetPathTextBox.Text = currentPath.Substring(0, lastSlash + 1) + _selectedFiles[0].Name;
                }
            }
        }

        private void SelectUploadFolder(StorageFolder folder)
        {
            _selectedFolder = folder;
            _selectedFiles.Clear();
            UploadFileNameText.Text = folder.Name;
            UploadFileSizeText.Text = "(Directory) - Click to change";
            StartUploadBtn.IsEnabled = true;

            // Prefill path with foldername
            string currentPath = UploadTargetPathTextBox.Text;
            int lastSlash = currentPath.LastIndexOf('/');
            if (lastSlash != -1)
            {
                UploadTargetPathTextBox.Text = currentPath.Substring(0, lastSlash + 1) + folder.Name;
            }
        }

        private void StartUpload_Click(object sender, RoutedEventArgs e)
        {
            if (_selectedFiles.Count == 0 && _selectedFolder == null) return;
            if (_isUploading) return;
            string targetPath = UploadTargetPathTextBox.Text;
            if (string.IsNullOrWhiteSpace(targetPath)) return;

            SetUploadingState(true);

            if (UseFtpToggle.IsOn)
            {
                FtpUploadRequested?.Invoke(_selectedFiles.Count > 0 ? _selectedFiles[0] : null, _selectedFolder, targetPath, FtpUsername, FtpPassword);
            }
            else if (_selectedFiles.Count == 1)
            {
                UploadRequested?.Invoke(_selectedFiles[0], targetPath, VerifyMd5);
            }
            else
            {
                MultiUploadRequested?.Invoke(_selectedFiles, targetPath, VerifyMd5);
            }
        }
    }
}
