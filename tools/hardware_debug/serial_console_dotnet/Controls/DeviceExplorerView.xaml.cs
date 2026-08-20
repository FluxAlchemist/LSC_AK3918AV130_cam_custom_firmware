using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using Windows.Storage;
using Windows.Storage.Pickers;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using serial_console_dotnet.Services;
using System.Linq;

namespace serial_console_dotnet.Controls
{
    public sealed partial class DeviceExplorerView : UserControl
    {
        private ShellExecutionService? _shell;
        private ConnectionService? _connectionService;
        private FileTransferService? _fileTransfer;

        private string _currentPath = "/";
        private readonly List<string> _backStack = new List<string>();
        private readonly List<string> _forwardStack = new List<string>();
        private readonly List<string> _copiedSourcePaths = new List<string>();
        private bool _isNavigating = false;

        public string CurrentPath => _currentPath;

        public event Action? ExplorerClosed;
        public event Action<string>? ExecuteFileRequested;

        public IntPtr ParentWindowHwnd { get; set; } = IntPtr.Zero;

        public DeviceExplorerView()
        {
            this.InitializeComponent();

            // Wire up Sub-control events
            ExplorerAddressBar.BackClicked += AddressBar_BackClicked;
            ExplorerAddressBar.ForwardClicked += AddressBar_ForwardClicked;
            ExplorerAddressBar.UpClicked += AddressBar_UpClicked;
            ExplorerAddressBar.RefreshClicked += AddressBar_RefreshClicked;
            ExplorerAddressBar.PathSubmitted += AddressBar_PathSubmitted;
            ExplorerAddressBar.ViewModeChanged += AddressBar_ViewModeChanged;

            ExplorerSidebar.FolderSelected += Sidebar_FolderSelected;

            ExplorerContents.DirectoryDoubleTapped += Contents_DirectoryDoubleTapped;
            ExplorerContents.SelectionChanged += Contents_SelectionChanged;
            ExplorerContents.DownloadRequested += Contents_DownloadRequested;
            ExplorerContents.CopyRequested += Contents_CopyRequested;
            ExplorerContents.DeleteRequested += Contents_DeleteRequested;
            ExplorerContents.PropertiesRequested += Contents_PropertiesRequested;
            ExplorerContents.PasteRequested += Contents_PasteRequested;
            ExplorerContents.NewFolderRequested += Contents_NewFolderRequested;
            ExplorerContents.ItemsDropped += Contents_ItemsDropped;
            ExplorerContents.AddToFavoritesRequested += Contents_AddToFavoritesRequested;
            ExplorerContents.EditFileRequested += Contents_EditFileRequested;
            ExplorerContents.CopyPathRequested += Contents_CopyPathRequested;
            ExplorerContents.ExecuteRequested += Contents_ExecuteRequested;
            ExplorerContents.RenameCommitted += Contents_RenameCommitted;
        }

        public void Initialize(ShellExecutionService shell, ConnectionService connection, FileTransferService transfer)
        {
            _shell = shell;
            _connectionService = connection;
            _fileTransfer = transfer;
        }

        public void SetFtpCredentials(string username, string password)
        {
            FtpUsernameTextBox.Text = string.IsNullOrEmpty(username) ? "root" : username;
            FtpPasswordBox.Password = password ?? "";
        }

        public async Task ShowAsync(string initialPath)
        {
            this.Visibility = Visibility.Visible;
            _backStack.Clear();
            _forwardStack.Clear();
            ExplorerAddressBar.SetNavigationState(false, false);
            
            ExplorerSidebar.LoadFromSettings();
            
            await NavigateToAsync(initialPath);
        }

        public void Hide()
        {
            this.Visibility = Visibility.Collapsed;
        }

        public void SetStatus(string status)
        {
            TransferProgressPanel.Visibility = Visibility.Visible;
            TransferStatusText.Text = status;
        }

        public void SetProgress(double progress)
        {
            TransferProgressPanel.Visibility = Visibility.Visible;
            if (progress < 0)
            {
                TransferProgressBar.IsIndeterminate = true;
            }
            else
            {
                TransferProgressBar.IsIndeterminate = false;
                TransferProgressBar.Value = progress;
            }
        }

        private void SetTransferringState(bool isTransferring)
        {
            UploadFileBtn.IsEnabled = !isTransferring;
            UploadFolderBtn.IsEnabled = !isTransferring;
            DownloadBtn.IsEnabled = !isTransferring && ExplorerContents.GetSelectedItems().Count > 0;
            CloseBtn.IsEnabled = !isTransferring;
            
            ExplorerContents.IsEnabled = !isTransferring;
            ExplorerSidebar.SetEnabled(!isTransferring);
            ExplorerAddressBar.SetEnabled(!isTransferring);

            if (!isTransferring)
            {
                TransferProgressPanel.Visibility = Visibility.Collapsed;
            }
        }

        public async Task NavigateToAsync(string path, bool addToHistory = true)
        {
            if (_shell == null) return;
            if (_isNavigating) return;
            _isNavigating = true;

            try
            {
                // Normalize path
                if (string.IsNullOrEmpty(path)) path = "/";
                path = path.Replace("\\", "/");
                if (!path.StartsWith("/")) path = "/" + path;

                // Resolve canonical paths (dereference symlinks to prevent circular navigation loops like /data/data)
                string canonical = await _shell.GetCanonicalPathAsync(path);
                if (!string.IsNullOrEmpty(canonical))
                {
                    path = canonical;
                }

                // Verify directory existence before updating path state or history
                bool exists = await DirectoryExistsAsync(path);
                if (!exists)
                {
                    _shell.LogMessage($"Navigation failed: Directory does not exist: {path}", "error");
                    return;
                }

                if (addToHistory && path != _currentPath)
                {
                    _backStack.Add(_currentPath);
                    _forwardStack.Clear();
                }

                _currentPath = path;
                
                // Sync with sub-controls
                ExplorerAddressBar.SetNavigationState(_backStack.Count > 0, _forwardStack.Count > 0);
                ExplorerAddressBar.SetCurrentPath(_currentPath);

                ExplorerContents.SetLoading(true);

                var items = await _shell.ListDirectoryContentsAsync(path);
                items.Sort((a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));

                var displayItems = new List<ExplorerItem>();
                foreach (var item in items)
                {
                    if (item.IsDirectory)
                    {
                        displayItems.Add(new ExplorerItem
                        {
                            IsDirectory = true,
                            Name = item.Name,
                            Size = item.Size,
                            IsSymlink = item.IsSymlink,
                            SymlinkTarget = item.SymlinkTarget
                        });
                    }
                }
                foreach (var item in items)
                {
                    if (!item.IsDirectory)
                    {
                        displayItems.Add(new ExplorerItem
                        {
                            IsDirectory = false,
                            Name = item.Name,
                            Size = item.Size,
                            IsSymlink = item.IsSymlink,
                            SymlinkTarget = item.SymlinkTarget
                        });
                    }
                }

                ExplorerContents.SetItems(displayItems);

                // Add current folder to recent folders list
                ExplorerSidebar.AddRecentFolder(_currentPath);

                UpdateActionsState();
            }
            catch (Exception ex)
            {
                _shell.LogMessage($"Navigation failed: {ex.Message}", "error");
            }
            finally
            {
                _isNavigating = false;
                ExplorerContents.SetLoading(false);
            }
        }

        private void UpdateActionsState()
        {
            int selectedCount = ExplorerContents.GetSelectedItems().Count;
            DownloadBtn.IsEnabled = selectedCount > 0;
        }

        // ================= ADDRESS BAR DELEGATES =================

        private async void AddressBar_BackClicked()
        {
            if (_backStack.Count > 0)
            {
                string prev = _backStack[_backStack.Count - 1];
                _backStack.RemoveAt(_backStack.Count - 1);
                _forwardStack.Add(_currentPath);
                await NavigateToAsync(prev, false);
            }
        }

        private async void AddressBar_ForwardClicked()
        {
            if (_forwardStack.Count > 0)
            {
                string next = _forwardStack[_forwardStack.Count - 1];
                _forwardStack.RemoveAt(_forwardStack.Count - 1);
                _backStack.Add(_currentPath);
                await NavigateToAsync(next, false);
            }
        }

        private async void AddressBar_UpClicked()
        {
            if (_currentPath == "/") return;
            string parent = System.IO.Path.GetDirectoryName(_currentPath) ?? "/";
            parent = parent.Replace("\\", "/");
            if (string.IsNullOrEmpty(parent)) parent = "/";
            await NavigateToAsync(parent);
        }

        private async void AddressBar_RefreshClicked()
        {
            await NavigateToAsync(_currentPath, false);
        }

        private async void AddressBar_PathSubmitted(string newPath)
        {
            await NavigateToAsync(newPath);
        }

        private void AddressBar_ViewModeChanged(string mode)
        {
            ExplorerContents.SetViewMode(mode);
        }

        // ================= SIDEBAR DELEGATES =================

        private async void Sidebar_FolderSelected(string path)
        {
            await NavigateToAsync(path);
        }

        // ================= CONTENTS VIEW DELEGATES =================

        private async void Contents_DirectoryDoubleTapped(ExplorerItem item)
        {
            if (item.IsDirectory)
            {
                string target = _currentPath.EndsWith("/") ? _currentPath + item.Name : _currentPath + "/" + item.Name;
                await NavigateToAsync(target);
            }
        }

        private void Contents_SelectionChanged()
        {
            UpdateActionsState();
        }

        private void Contents_DownloadRequested()
        {
            DownloadSelectedItemsAsync();
        }

        private void Contents_CopyRequested()
        {
            CopySelectedItems();
        }

        private void Contents_DeleteRequested()
        {
            DeleteSelectedItemsAsync();
        }

        private void Contents_PropertiesRequested()
        {
            ShowPropertiesSelectedAsync();
        }

        private void Contents_PasteRequested()
        {
            PasteCopiedItemsAsync();
        }

        private async void Contents_NewFolderRequested(string folderName)
        {
            if (string.IsNullOrEmpty(folderName) || _shell == null) return;

            ExplorerContents.SetLoading(true);
            try
            {
                string targetDir = _currentPath.EndsWith("/") ? _currentPath + folderName : _currentPath + "/" + folderName;
                string escapedPath = targetDir.Replace("'", "\\'");
                string cmd = $"echo ===MKDIR_ST\"\"ART=== ; mkdir -p '{escapedPath}' ; echo ===MKDIR_E\"\"ND===";
                await _shell.RunHiddenQueryAsync(cmd, "===MKDIR_START===", "===MKDIR_END===", 2000);
            }
            catch (Exception ex)
            {
                _shell.LogMessage($"Failed to create directory: {ex.Message}", "error");
            }
            finally
            {
                ExplorerContents.SetLoading(false);
                await NavigateToAsync(_currentPath, false);
            }
        }

        private async void Contents_ItemsDropped(IReadOnlyList<IStorageItem> items)
        {
            await UploadStorageItemsAsync(items);
        }

        // ================= CORE FILE OPERATIONS ACTIONS =================

        private async void DownloadSelectedItemsAsync()
        {
            var selectedItems = ExplorerContents.GetSelectedItems();
            if (selectedItems.Count == 0 || _fileTransfer == null || _connectionService == null || _shell == null) return;

            bool useFtp = true;
            string host = _connectionService.RemoteIp ?? "192.168.1.1";
            string username = FtpUsernameTextBox.Text;
            string password = FtpPasswordBox.Password;

            SetTransferringState(true);

            try
            {
                if (selectedItems.Count == 1)
                {
                    var item = selectedItems[0];
                    string remotePath = _currentPath.EndsWith("/") ? _currentPath + item.Name : _currentPath + "/" + item.Name;

                    if (item.IsDirectory)
                    {
                        if (!useFtp)
                        {
                            _shell.LogMessage("Folder downloads require FTP mode.", "error");
                            return;
                        }
                        await _fileTransfer.RunFtpDownloadOrchestrationAsync(host, username, password, remotePath, _shell, GetLocalPathCallback);
                    }
                    else
                    {
                        if (useFtp)
                        {
                            await _fileTransfer.RunFtpDownloadOrchestrationAsync(host, username, password, remotePath, _shell, GetLocalPathCallback);
                        }
                        else
                        {
                            // Serial UART file download
                            var picker = new FileSavePicker();
                            picker.SuggestedStartLocation = PickerLocationId.Downloads;
                            picker.SuggestedFileName = item.Name;
                            string ext = System.IO.Path.GetExtension(item.Name);
                            if (string.IsNullOrEmpty(ext)) ext = ".bin";
                            picker.FileTypeChoices.Add("File", new List<string> { ext });
                            if (ParentWindowHwnd != IntPtr.Zero)
                            {
                                WinRT.Interop.InitializeWithWindow.Initialize(picker, ParentWindowHwnd);
                            }
                            StorageFile localFile = await picker.PickSaveFileAsync();
                            if (localFile != null)
                            {
                                _fileTransfer.StartDownload(remotePath, localFile);
                            }
                        }
                    }
                }
                else
                {
                    // Multiple files selected - prompt for local destination folder
                    var picker = new FolderPicker();
                    picker.SuggestedStartLocation = PickerLocationId.Downloads;
                    picker.FileTypeFilter.Add("*");
                    if (ParentWindowHwnd != IntPtr.Zero)
                    {
                        WinRT.Interop.InitializeWithWindow.Initialize(picker, ParentWindowHwnd);
                    }
                    StorageFolder localFolder = await picker.PickSingleFolderAsync();
                    if (localFolder == null) return;

                    int count = 0;
                    foreach (var item in selectedItems)
                    {
                        count++;
                        string remotePath = _currentPath.EndsWith("/") ? _currentPath + item.Name : _currentPath + "/" + item.Name;
                        SetStatus($"Downloading item {count}/{selectedItems.Count}: {item.Name}...");

                        if (item.IsDirectory)
                        {
                            if (!useFtp)
                            {
                                _shell.LogMessage($"Skipping directory '{item.Name}' (requires FTP mode).", "error");
                                continue;
                            }
                            var filesList = await _fileTransfer.GetRemoteFilesListAsync(remotePath, _shell);
                            if (filesList.Count > 0)
                            {
                                await _fileTransfer.DownloadFtpDirectoryIterativeAsync(host, username, password, remotePath, localFolder.Path, filesList);
                            }
                        }
                        else
                        {
                            string localFilePath = Path.Combine(localFolder.Path, item.Name);
                            if (useFtp)
                            {
                                await _fileTransfer.DownloadFtpFileAsync(host, username, password, remotePath, localFilePath);
                            }
                            else
                            {
                                _shell.LogMessage("Batch downloading over UART serial is not supported. Please use FTP.", "error");
                                break;
                            }
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                _shell.LogMessage($"Download operation failed: {ex.Message}", "error");
            }
            finally
            {
                SetTransferringState(false);
            }
        }

        private async Task<string?> GetLocalPathCallback(bool isDirectory, string remotePath)
        {
            if (isDirectory)
            {
                var picker = new FolderPicker();
                picker.SuggestedStartLocation = PickerLocationId.Downloads;
                picker.FileTypeFilter.Add("*");
                if (ParentWindowHwnd != IntPtr.Zero)
                {
                    WinRT.Interop.InitializeWithWindow.Initialize(picker, ParentWindowHwnd);
                }
                StorageFolder localFolder = await picker.PickSingleFolderAsync();
                return localFolder?.Path;
            }
            else
            {
                var picker = new FileSavePicker();
                picker.SuggestedStartLocation = PickerLocationId.Downloads;
                string fileName = System.IO.Path.GetFileName(remotePath);
                picker.SuggestedFileName = fileName;
                string ext = System.IO.Path.GetExtension(fileName);
                if (string.IsNullOrEmpty(ext)) ext = ".bin";
                picker.FileTypeChoices.Add("File", new List<string> { ext });
                if (ParentWindowHwnd != IntPtr.Zero)
                {
                    WinRT.Interop.InitializeWithWindow.Initialize(picker, ParentWindowHwnd);
                }
                StorageFile localFile = await picker.PickSaveFileAsync();
                return localFile?.Path;
            }
        }

        private void CopySelectedItems()
        {
            var selectedItems = ExplorerContents.GetSelectedItems();
            if (selectedItems.Count == 0) return;

            _copiedSourcePaths.Clear();
            foreach (var item in selectedItems)
            {
                string remotePath = _currentPath.EndsWith("/") ? _currentPath + item.Name : _currentPath + "/" + item.Name;
                _copiedSourcePaths.Add(remotePath);
            }

            ExplorerContents.SetPasteEnabled(true);
            _shell?.LogMessage($"Copied {selectedItems.Count} items to remote clipboard.", "info");
        }

        private async void PasteCopiedItemsAsync()
        {
            if (_copiedSourcePaths.Count == 0 || _shell == null) return;

            ExplorerContents.SetLoading(true);
            try
            {
                var commands = new List<string>();
                foreach (var src in _copiedSourcePaths)
                {
                    string escapedSrc = src.Replace("'", "\\'");
                    string escapedDest = _currentPath.Replace("'", "\\'");
                    commands.Add($"cp -rf '{escapedSrc}' '{escapedDest}/'");
                }

                string fullCmd = $"echo ===CP_ST\"\"ART=== ; {string.Join(" ; ", commands)} ; echo ===CP_E\"\"ND===";
                await _shell.RunHiddenQueryAsync(fullCmd, "===CP_START===", "===CP_END===", 5000);
            }
            catch (Exception ex)
            {
                _shell.LogMessage($"Paste failed: {ex.Message}", "error");
            }
            finally
            {
                ExplorerContents.SetLoading(false);
                await NavigateToAsync(_currentPath, false);
            }
        }

        private async void DeleteSelectedItemsAsync()
        {
            var selectedItems = ExplorerContents.GetSelectedItems();
            if (selectedItems.Count == 0 || _shell == null) return;

            string message = selectedItems.Count == 1 
                ? $"Are you sure you want to permanently delete '{selectedItems[0].Name}'?"
                : $"Are you sure you want to permanently delete these {selectedItems.Count} items?";

            var dialog = new ContentDialog
            {
                Title = "Confirm Delete",
                Content = message,
                PrimaryButtonText = "Delete",
                CloseButtonText = "Cancel",
                DefaultButton = ContentDialogButton.Close,
                XamlRoot = this.XamlRoot
            };

            var result = await dialog.ShowAsync();
            if (result != ContentDialogResult.Primary) return;

            var paths = new List<string>();
            foreach (var item in selectedItems)
            {
                string itemPath = _currentPath.EndsWith("/") ? _currentPath + item.Name : _currentPath + "/" + item.Name;
                paths.Add($"'{itemPath.Replace("'", "\\'")}'");
            }

            string cmd = $"rm -rf {string.Join(" ", paths)}";

            ExplorerContents.SetLoading(true);
            try
            {
                string fullCmd = $"echo ===DEL_ST\"\"ART=== ; {cmd} ; echo ===DEL_E\"\"ND===";
                await _shell.RunHiddenQueryAsync(fullCmd, "===DEL_START===", "===DEL_END===", 3000);
            }
            catch (Exception ex)
            {
                _shell.LogMessage($"Delete failed: {ex.Message}", "error");
            }
            finally
            {
                ExplorerContents.SetLoading(false);
                await NavigateToAsync(_currentPath, false);
            }
        }

        private string ParseStatRegex(string output, string pattern)
        {
            var match = System.Text.RegularExpressions.Regex.Match(output, pattern);
            return match.Success ? match.Groups[1].Value.Trim() : "";
        }

        private string FormatByteSize(long size)
        {
            if (size < 1024) return $"{size} B";
            if (size < 1048576) return $"{(size / 1024.0):F1} KB";
            return $"{(size / 1048576.0):F1} MB";
        }

        private void AddPropertyRow(StackPanel container, string label, string value)
        {
            if (string.IsNullOrEmpty(value)) return;

            var grid = new Grid { Margin = new Thickness(0, 2, 0, 2) };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(100) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

            var labelBlock = new TextBlock
            {
                Text = label,
                Foreground = (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources["TextMutedBrush"],
                FontSize = 12,
                VerticalAlignment = VerticalAlignment.Center
            };
            Grid.SetColumn(labelBlock, 0);

            var valueBlock = new TextBlock
            {
                Text = value,
                Foreground = new Microsoft.UI.Xaml.Media.SolidColorBrush(Windows.UI.Color.FromArgb(255, 242, 244, 247)),
                FontSize = 12,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                TextWrapping = TextWrapping.Wrap,
                IsTextSelectionEnabled = true,
                VerticalAlignment = VerticalAlignment.Center
            };
            Grid.SetColumn(valueBlock, 1);

            grid.Children.Add(labelBlock);
            grid.Children.Add(valueBlock);
            container.Children.Add(grid);
        }

        private async Task<string?> GetFileMd5Async(string path)
        {
            if (_shell == null) return null;
            try
            {
                string escapedPath = path.Replace("'", "\\'");
                string cmd = $"echo ===MD5_ST\"\"ART=== ; md5sum '{escapedPath}' 2>/dev/null || echo NO ; echo ===MD5_E\"\"ND===";
                string result = await _shell.RunHiddenQueryAsync(cmd, "===MD5_START===", "===MD5_END===", 4000);
                var match = System.Text.RegularExpressions.Regex.Match(result, "[0-9a-fA-F]{32}");
                return match.Success ? match.Value.ToLowerInvariant() : null;
            }
            catch
            {
                return null;
            }
        }

        private async void ShowPropertiesSelectedAsync()
        {
            var selectedItems = ExplorerContents.GetSelectedItems();
            if (selectedItems.Count == 0 || _shell == null) return;

            PropertiesContainer.Children.Clear();
            var loadingText = new TextBlock { Text = "Retrieving item statistics...", Foreground = new Microsoft.UI.Xaml.Media.SolidColorBrush(Windows.UI.Color.FromArgb(255, 242, 244, 247)), FontSize = 12 };
            PropertiesContainer.Children.Add(loadingText);
            PropertiesPopup.IsOpen = true;

            try
            {
                if (selectedItems.Count == 1)
                {
                    var item = selectedItems[0];
                    string itemPath = BuildFullPath(item);
                    string escapedPath = itemPath.Replace("'", "\\'");

                    string cmd = $"echo ===STAT_ST\"\"ART=== ; stat '{escapedPath}' 2>/dev/null || ls -la '{escapedPath}' ; echo ===STAT_E\"\"ND===";
                    string statOutput = await _shell.RunHiddenQueryAsync(cmd, "===STAT_START===", "===STAT_END===", 2000);

                    PropertiesContainer.Children.Clear();

                    string sizeVal = ParseStatRegex(statOutput, @"Size:\s*(\d+)");
                    string typeVal = ParseStatRegex(statOutput, @"IO Block:\s*\d+\s+([^\n\t]+)");
                    string deviceVal = ParseStatRegex(statOutput, @"Device:\s*([^\s\t]+)");
                    string inodeVal = ParseStatRegex(statOutput, @"Inode:\s*(\d+)");
                    string linksVal = ParseStatRegex(statOutput, @"Links:\s*(\d+)");
                    string accessPerms = ParseStatRegex(statOutput, @"Access:\s*\(([^)]+)\)");
                    string uidVal = ParseStatRegex(statOutput, @"Uid:\s*\(([^)]+)\)");
                    string gidVal = ParseStatRegex(statOutput, @"Gid:\s*\(([^)]+)\)");
                    string accessTime = ParseStatRegex(statOutput, @"Access:\s*(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})");
                    string modifyTime = ParseStatRegex(statOutput, @"Modify:\s*(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})");
                    string changeTime = ParseStatRegex(statOutput, @"Change:\s*(\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2})");

                    AddPropertyRow(PropertiesContainer, "Path", itemPath);

                    if (!string.IsNullOrEmpty(typeVal))
                    {
                        AddPropertyRow(PropertiesContainer, "Type", typeVal);
                    }
                    else
                    {
                        AddPropertyRow(PropertiesContainer, "Type", item.IsDirectory ? "Directory" : "File");
                    }

                    if (long.TryParse(sizeVal, out long bytes))
                    {
                        AddPropertyRow(PropertiesContainer, "Size", $"{FormatByteSize(bytes)} ({bytes:N0} bytes)");
                    }
                    else
                    {
                        AddPropertyRow(PropertiesContainer, "Size", item.IsDirectory ? "" : FormatByteSize(item.Size));
                    }

                    if (!string.IsNullOrEmpty(accessPerms))
                    {
                        AddPropertyRow(PropertiesContainer, "Permissions", accessPerms);
                    }

                    if (!string.IsNullOrEmpty(uidVal))
                    {
                        string cleanedUid = System.Text.RegularExpressions.Regex.Replace(uidVal, @"\s+", " ");
                        AddPropertyRow(PropertiesContainer, "Owner", cleanedUid);
                    }

                    if (!string.IsNullOrEmpty(gidVal))
                    {
                        string cleanedGid = System.Text.RegularExpressions.Regex.Replace(gidVal, @"\s+", " ");
                        AddPropertyRow(PropertiesContainer, "Group", cleanedGid);
                    }

                    if (!string.IsNullOrEmpty(modifyTime))
                    {
                        AddPropertyRow(PropertiesContainer, "Modified", modifyTime);
                    }

                    if (!string.IsNullOrEmpty(accessTime))
                    {
                        AddPropertyRow(PropertiesContainer, "Accessed", accessTime);
                    }

                    if (!item.IsDirectory)
                    {
                        AddPropertyRow(PropertiesContainer, "MD5 Checksum", "Calculating checksum...");
                        
                        DispatcherQueue.TryEnqueue(async () =>
                        {
                            string? md5 = await GetFileMd5Async(itemPath);
                            
                            foreach (var child in PropertiesContainer.Children)
                            {
                                if (child is Grid grid && grid.Children.Count == 2)
                                {
                                    var labelBlock = grid.Children[0] as TextBlock;
                                    var valueBlock = grid.Children[1] as TextBlock;
                                    if (labelBlock != null && labelBlock.Text == "MD5 Checksum" && valueBlock != null)
                                    {
                                        valueBlock.Text = string.IsNullOrEmpty(md5) ? "Failed to calculate" : md5;
                                        break;
                                    }
                                }
                            }
                        });
                    }
                }
                else
                {
                    PropertiesContainer.Children.Clear();
                    AddPropertyRow(PropertiesContainer, "Selection", $"{selectedItems.Count} items selected");

                    long totalSize = 0;
                    int dirs = 0;
                    int files = 0;

                    foreach (var item in selectedItems)
                    {
                        if (item.IsDirectory) dirs++;
                        else
                        {
                            files++;
                            totalSize += item.Size;
                        }
                    }

                    if (dirs > 0)
                    {
                        AddPropertyRow(PropertiesContainer, "Directories", dirs.ToString());
                    }
                    if (files > 0)
                    {
                        AddPropertyRow(PropertiesContainer, "Files", files.ToString());
                        AddPropertyRow(PropertiesContainer, "Total Size", $"{FormatByteSize(totalSize)} ({totalSize:N0} bytes)");
                    }

                    string pathsList = string.Join("\n", selectedItems.Select(i => BuildFullPath(i)));
                    AddPropertyRow(PropertiesContainer, "Paths", pathsList);
                }
            }
            catch (Exception ex)
            {
                PropertiesContainer.Children.Clear();
                var errorText = new TextBlock { Text = $"Failed to retrieve properties: {ex.Message}", Foreground = new Microsoft.UI.Xaml.Media.SolidColorBrush(Windows.UI.Color.FromArgb(255, 244, 67, 54)), FontSize = 12 };
                PropertiesContainer.Children.Add(errorText);
            }
        }

        private void CloseProperties_Click(object sender, RoutedEventArgs e)
        {
            PropertiesPopup.IsOpen = false;
        }

        // ================= NEW FOLDER LOGIC =================

        private void CancelNewFolder_Click(object sender, RoutedEventArgs e)
        {
            NewFolderPopup.IsOpen = false;
        }

        private async void CreateNewFolder_Click(object sender, RoutedEventArgs e)
        {
            string folderName = NewFolderNameTextBox.Text.Trim();
            NewFolderPopup.IsOpen = false;

            if (string.IsNullOrEmpty(folderName) || _shell == null) return;

            ExplorerContents.SetLoading(true);
            try
            {
                string targetDir = _currentPath.EndsWith("/") ? _currentPath + folderName : _currentPath + "/" + folderName;
                string escapedPath = targetDir.Replace("'", "\\'");
                string cmd = $"mkdir -p '{escapedPath}' && echo ===MKDIR_OK===";
                await _shell.RunHiddenQueryAsync(cmd, "===MKDIR_OK===", "===MKDIR_OK===", 2000);
            }
            catch (Exception ex)
            {
                _shell.LogMessage($"Failed to create directory: {ex.Message}", "error");
            }
            finally
            {
                ExplorerContents.SetLoading(false);
                await NavigateToAsync(_currentPath, false);
            }
        }

        private void NewFolderNameTextBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == Windows.System.VirtualKey.Enter)
            {
                CreateNewFolder_Click(sender, e);
                e.Handled = true;
            }
        }

        // ================= DRAG AND DROP HANDLERS =================

        private async Task UploadStorageItemsAsync(IReadOnlyList<IStorageItem> storageItems)
        {
            if (_shell == null || _fileTransfer == null || _connectionService == null) return;

            bool useFtp = true;
            string host = _connectionService.RemoteIp ?? "192.168.1.1";
            string username = FtpUsernameTextBox.Text;
            string password = FtpPasswordBox.Password;

            SetTransferringState(true);
            SetStatus("Uploading dropped items...");

            bool? applyToAllOverwrite = null; // null = prompt, true = overwrite all, false = skip all

            try
            {
                int count = 0;
                foreach (var item in storageItems)
                {
                    count++;

                    // Check for naming collision
                    if (ExplorerContents.ContainsItem(item.Name))
                    {
                        if (applyToAllOverwrite == false)
                        {
                            // Skip all remaining
                            continue;
                        }

                        if (applyToAllOverwrite == null)
                        {
                            // Prompt conflict resolution on UI thread
                            var checkBox = new CheckBox
                            {
                                Content = "Do this for all current conflicts",
                                Margin = new Thickness(0, 12, 0, 0)
                            };
                            var panel = new StackPanel();
                            panel.Children.Add(new TextBlock 
                            { 
                                Text = $"An item named '{item.Name}' already exists in this folder. What would you like to do?", 
                                TextWrapping = TextWrapping.Wrap 
                            });
                            panel.Children.Add(checkBox);

                            var dialog = new ContentDialog
                            {
                                Title = "File Conflict",
                                Content = panel,
                                PrimaryButtonText = "Overwrite",
                                SecondaryButtonText = "Skip",
                                CloseButtonText = "Cancel",
                                DefaultButton = ContentDialogButton.Primary,
                                XamlRoot = this.XamlRoot
                            };

                            var result = await dialog.ShowAsync();
                            if (result == ContentDialogResult.Primary)
                            {
                                if (checkBox.IsChecked == true)
                                {
                                    applyToAllOverwrite = true;
                                }
                            }
                            else if (result == ContentDialogResult.Secondary)
                            {
                                if (checkBox.IsChecked == true)
                                {
                                    applyToAllOverwrite = false;
                                }
                                continue;
                            }
                            else
                            {
                                // Cancel remains and aborts
                                _shell.LogMessage("Upload canceled by user.", "info");
                                break;
                            }
                        }
                    }

                    string remotePath = _currentPath.EndsWith("/") ? _currentPath + item.Name : _currentPath + "/" + item.Name;
                    SetStatus($"Uploading item {count}/{storageItems.Count}: {item.Name}...");

                    if (item is StorageFolder folder)
                    {
                        if (!useFtp)
                        {
                            _shell.LogMessage("Folder uploads require FTP mode.", "error");
                            continue;
                        }
                        await _fileTransfer.RunFtpUploadOrchestrationAsync(host, username, password, remotePath, null, folder.Path);
                    }
                    else if (item is StorageFile file)
                    {
                        if (useFtp)
                        {
                            await _fileTransfer.RunFtpUploadOrchestrationAsync(host, username, password, remotePath, file.Path, null);
                        }
                        else
                        {
                            await _fileTransfer.StartUploadAsync(file, remotePath);
                        }
                    }
                }

                await NavigateToAsync(_currentPath, false);
            }
            catch (Exception ex)
            {
                _shell.LogMessage($"Upload failed: {ex.Message}", "error");
            }
            finally
            {
                SetTransferringState(false);
            }
        }

        private async void UploadFileBtn_Click(object sender, RoutedEventArgs e)
        {
            if (_shell == null || _connectionService == null || _fileTransfer == null) return;

            var picker = new FileOpenPicker();
            picker.ViewMode = PickerViewMode.List;
            picker.SuggestedStartLocation = PickerLocationId.ComputerFolder;
            picker.FileTypeFilter.Add("*");

            if (ParentWindowHwnd != IntPtr.Zero)
            {
                WinRT.Interop.InitializeWithWindow.Initialize(picker, ParentWindowHwnd);
            }

            StorageFile file = await picker.PickSingleFileAsync();
            if (file != null)
            {
                var list = new List<IStorageItem> { file };
                await UploadStorageItemsAsync(list);
            }
        }

        private async void UploadFolderBtn_Click(object sender, RoutedEventArgs e)
        {
            if (_shell == null || _connectionService == null || _fileTransfer == null) return;

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
                var list = new List<IStorageItem> { folder };
                await UploadStorageItemsAsync(list);
            }
        }

        private void DownloadBtn_Click(object sender, RoutedEventArgs e)
        {
            DownloadSelectedItemsAsync();
        }

        private void ToggleCredentialsBtn_Click(object sender, RoutedEventArgs e)
        {
            if (FtpSettingsPanel.Visibility == Visibility.Visible)
            {
                FtpSettingsPanel.Visibility = Visibility.Collapsed;
            }
            else
            {
                FtpSettingsPanel.Visibility = Visibility.Visible;
            }
        }

        private void Contents_AddToFavoritesRequested(ExplorerItem item)
        {
            if (item == null || !item.IsDirectory) return;

            string fullPath = System.IO.Path.Combine(_currentPath, item.Name).Replace("\\", "/");
            if (fullPath.Length > 1 && fullPath.EndsWith("/")) fullPath = fullPath.TrimEnd('/');

            ExplorerSidebar.AddFavoriteFolder(item.Name, fullPath);
        }

        private async void Contents_EditFileRequested(ExplorerItem item)
        {
            if (item == null || item.IsDirectory || _fileTransfer == null || _connectionService == null) return;

            string host = _connectionService.RemoteIp ?? "192.168.1.1";
            string hostIp = host;
            int colon = host.IndexOf(':');
            if (colon != -1)
            {
                hostIp = host.Substring(0, colon);
            }
            string username = FtpUsernameTextBox.Text;
            string password = FtpPasswordBox.Password;

            string remotePath = _currentPath.EndsWith("/") ? _currentPath + item.Name : _currentPath + "/" + item.Name;
            string tempPath = System.IO.Path.Combine(System.IO.Path.GetTempPath(), item.Name);

            ExplorerContents.SetLoading(true);
            try
            {
                await _fileTransfer.DownloadFtpFileAsync(hostIp, username, password, remotePath, tempPath);
                
                string content = "";
                if (System.IO.File.Exists(tempPath))
                {
                    content = System.IO.File.ReadAllText(tempPath);
                }

                var parentWindowId = new Microsoft.UI.WindowId { Value = (ulong)ParentWindowHwnd };
                var editorWindow = new TextEditorWindow(parentWindowId, item.Name, content, async (newText) =>
                {
                    try
                    {
                        System.IO.File.WriteAllText(tempPath, newText);
                        await _fileTransfer.UploadFtpFileAsync(hostIp, username, password, tempPath, remotePath);
                        
                        this.DispatcherQueue.TryEnqueue(async () =>
                        {
                            await NavigateToAsync(_currentPath, false);
                        });
                        return true;
                    }
                    catch (Exception ex)
                    {
                        System.Diagnostics.Debug.WriteLine($"Error uploading edited file: {ex.Message}");
                        return false;
                    }
                });

                editorWindow.Activate();
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error opening file editor: {ex.Message}");
            }
            finally
            {
                ExplorerContents.SetLoading(false);
            }
        }

        private string BuildFullPath(ExplorerItem item)
        {
            string path = _currentPath.EndsWith("/") ? _currentPath + item.Name : _currentPath + "/" + item.Name;
            return path;
        }

        private async Task<bool> DirectoryExistsAsync(string path)
        {
            if (_shell == null) return false;
            if (path == "/") return true;
            try
            {
                string escapedPath = path.Replace("'", "\\'");
                string cmd = $"echo ===EX_ST\"\"ART=== ; [ -d '{escapedPath}' ] && echo YES || echo NO ; echo ===EX_E\"\"ND===";
                string output = await _shell.RunHiddenQueryAsync(cmd, "===EX_START===", "===EX_END===", 2000);
                return output.Trim() == "YES";
            }
            catch
            {
                return false;
            }
        }

        private void Contents_CopyPathRequested(ExplorerItem item)
        {
            if (item == null) return;

            string fullPath = BuildFullPath(item);
            var dataPackage = new Windows.ApplicationModel.DataTransfer.DataPackage();
            dataPackage.SetText(fullPath);
            Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(dataPackage);
        }

        private void Contents_ExecuteRequested(ExplorerItem item)
        {
            if (item == null || item.IsDirectory) return;

            string fullPath = BuildFullPath(item);
            string command = fullPath.EndsWith(".sh") ? $"sh {fullPath}" : fullPath;

            ExecuteFileRequested?.Invoke(command);

            this.Visibility = Visibility.Collapsed;
            ExplorerClosed?.Invoke();
        }

        private async void Contents_RenameCommitted(ExplorerItem item, string oldName, string newName)
        {
            if (_shell == null) return;
            ExplorerContents.SetLoading(true);
            try
            {
                string oldPath = _currentPath.EndsWith("/") ? _currentPath + oldName : _currentPath + "/" + oldName;
                string newPath = _currentPath.EndsWith("/") ? _currentPath + newName : _currentPath + "/" + newName;

                string escapedOld = oldPath.Replace("'", "\\'");
                string escapedNew = newPath.Replace("'", "\\'");

                string cmd = $"echo ===MV_ST\"\"ART=== ; mv '{escapedOld}' '{escapedNew}' ; echo ===MV_E\"\"ND===";
                await _shell.RunHiddenQueryAsync(cmd, "===MV_START===", "===MV_END===", 3000);
            }
            catch (Exception ex)
            {
                _shell.LogMessage($"Rename failed: {ex.Message}", "error");
            }
            finally
            {
                ExplorerContents.SetLoading(false);
                await NavigateToAsync(_currentPath, false); // Refresh directory
            }
        }

        private void CloseExplorer_Click(object sender, RoutedEventArgs e)
        {
            this.Visibility = Visibility.Collapsed;
            ExplorerClosed?.Invoke();
        }
    }
}
