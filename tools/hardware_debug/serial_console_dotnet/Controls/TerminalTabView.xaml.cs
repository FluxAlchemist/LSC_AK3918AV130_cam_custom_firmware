using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using System.ComponentModel;
using System.Text;
using Windows.Storage;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using serial_console_dotnet.Services;
using serial_console_dotnet.ViewModels;

namespace serial_console_dotnet.Controls
{
    // One fully independent terminal workspace: its own connection (serial/telnet), sidebar,
    // console view, command bar and upload/download dialogs. MainWindow hosts an arbitrary number
    // of these side by side as TabViewItems — nothing here is shared between instances (other than
    // the app-wide SettingsService file, whose "last saved wins" behavior across tabs is harmless
    // since it only seeds the *next* tab's defaults).
    public sealed partial class TerminalTabView : UserControl
    {
        public MainViewModel ViewModel { get; }

        private readonly ConnectionService _serialService;
        private readonly HistoryService _historyService;
        private readonly AnsiParserService _ansiParser;
        private readonly FileTransferService _fileTransfer;
        private readonly ShellExecutionService _shellExecution;
        private readonly BootExploitService _bootExploit;
        private readonly SdCardService _sdCard;
        private readonly AnsiInputEncoder _ansiEncoder;
        private readonly ConnectionOrchestrator _connectionOrchestrator;
        private readonly TerminalSessionManager _sessionManager;

        public IntPtr ParentWindowHwnd { get; set; } = IntPtr.Zero;

        public TerminalTabView(IntPtr hwnd)
        {
            this.InitializeComponent();

            ParentWindowHwnd = hwnd;

            _serialService = new ConnectionService();
            _historyService = new HistoryService();
            _ansiParser = new AnsiParserService();
            _shellExecution = new ShellExecutionService(_serialService);
            _fileTransfer = new FileTransferService(_serialService);
            _bootExploit = new BootExploitService(_serialService, _shellExecution);
            _sdCard = new SdCardService(_serialService, _shellExecution);
            _ansiEncoder = new AnsiInputEncoder();
            _connectionOrchestrator = new ConnectionOrchestrator(_serialService);

            var dialogService = new WindowsDialogService(hwnd);
            _fileTransfer.InitializeDependencies(dialogService, _shellExecution);

            ViewModel = new MainViewModel(
                _serialService,
                _shellExecution,
                _fileTransfer,
                _bootExploit,
                _sdCard,
                _historyService,
                _connectionOrchestrator
            );

            _sessionManager = new TerminalSessionManager(
                _serialService,
                _ansiParser,
                _shellExecution,
                _fileTransfer
            );

            _sessionManager.RawDataReceived += SessionManager_RawDataReceived;

            _shellExecution.InfoMessageLogged += (msg, lvl) =>
            {
                DispatcherQueue.TryEnqueue(() =>
                {
                    ConsoleTerminal.AppendSpecialLine(msg, lvl);
                });
            };

            _fileTransfer.StatusChanged += FileTransfer_StatusChanged;
            _fileTransfer.ProgressChanged += FileTransfer_ProgressChanged;
            _fileTransfer.DownloadComplete += FileTransfer_DownloadComplete;
            _fileTransfer.TransferError += FileTransfer_TransferError;
            _fileTransfer.UploadComplete += FileTransfer_UploadComplete;

            ViewModel.ShowDirectoryDialogRequested += ViewModel_ShowDirectoryDialogRequested;
            ViewModel.PropertyChanged += ViewModel_PropertyChanged;

            UploadOverlayDialog.ParentWindowHwnd = hwnd;
            ConsoleTerminal.ParentWindowHwnd = hwnd;

            UploadOverlayDialog.UploadRequested += UploadDialog_UploadRequested;
            UploadOverlayDialog.MultiUploadRequested += UploadDialog_MultiUploadRequested;
            DownloadOverlayDialog.DownloadRequested += DownloadDialog_DownloadRequested;
            UploadOverlayDialog.FtpUploadRequested += UploadDialog_FtpUploadRequested;
            DownloadOverlayDialog.FtpDownloadRequested += DownloadDialog_FtpDownloadRequested;

            ConsoleTerminal.FileDropped += ConsoleTerminal_FileDropped;

            ConsoleTerminal.ToggleTimestamps(ViewModel.ShowTimestamps);
            ConsoleTerminal.SetTitle(ViewModel.ConnectionHeaderTitle);

            // PasswordBox.Password isn't a DependencyProperty, so it can't be the target of an
            // x:Bind TwoWay binding like the other fields — seed it manually from the setting
            // loaded by MainViewModel's constructor, and push edits back via PasswordChanged below.
            TelnetPasswordBox.Password = ViewModel.TelnetPassword;

            UpdateSidebarButtonsVisibility();
        }

        // Called by MainWindow when this tab is closed. Tears down the connection and background
        // threads deterministically instead of relying on GC/finalizers to eventually get to it.
        public void Cleanup()
        {
            ViewModel.SaveSettings();
            _sessionManager.Dispose();
            _serialService.Dispose();

            if (_explorerWindow != null)
            {
                try { _explorerWindow.Close(); } catch {}
                _explorerWindow = null;
            }
        }

        private void ViewModel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
        {
            if (e.PropertyName == nameof(MainViewModel.ShowTimestamps))
            {
                ConsoleTerminal.ToggleTimestamps(ViewModel.ShowTimestamps);
            }
            else if (e.PropertyName == nameof(MainViewModel.ConnectionHeaderTitle))
            {
                ConsoleTerminal.SetTitle(ViewModel.ConnectionHeaderTitle);
            }
            else if (e.PropertyName == nameof(MainViewModel.IsConnectedAndIdle) ||
                     e.PropertyName == nameof(MainViewModel.SelectedConnectionType))
            {
                UpdateSidebarButtonsVisibility();
            }
        }

        private void SessionManager_RawDataReceived(string incomingText)
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                var lines = _ansiParser.ProcessIncomingText(incomingText, out string logsSegment);
                ConsoleTerminal.AppendText(lines, logsSegment);
            });
        }

        private void ViewModel_ShowDirectoryDialogRequested(string dialogType, string targetDir, IReadOnlyList<StorageFile>? droppedFiles)
        {
            bool isTelnet = _serialService.Mode == ConnectionMode.Telnet;
            UploadOverlayDialog.SetIsTelnet(isTelnet);
            DownloadOverlayDialog.SetIsTelnet(isTelnet);

            UploadOverlayDialog.SetFtpCredentials(ViewModel.FtpUsername, ViewModel.FtpPassword);
            DownloadOverlayDialog.SetFtpCredentials(ViewModel.FtpUsername, ViewModel.FtpPassword);

            if (dialogType == "upload")
            {
                if (droppedFiles != null && droppedFiles.Count == 1)
                {
                    string fullPath = targetDir + droppedFiles[0].Name;
                    UploadOverlayDialog.ShowWithFile(droppedFiles[0], fullPath);
                }
                else if (droppedFiles != null && droppedFiles.Count > 1)
                {
                    UploadOverlayDialog.ShowWithFiles(droppedFiles, targetDir);
                }
                else
                {
                    UploadOverlayDialog.SetTargetPath(targetDir);
                    UploadOverlayDialog.Show();
                }
            }
            else if (dialogType == "download")
            {
                DownloadOverlayDialog.SetTargetPath(targetDir + "wpa_supplicant.conf");
                DownloadOverlayDialog.Show();
            }
        }

        // ================= BUTTONS & INPUT EVENTS =================

        private void RefreshPorts_Click(object sender, RoutedEventArgs e)
        {
            ViewModel.ScanPorts();
        }

        private void TelnetPasswordBox_PasswordChanged(object sender, RoutedEventArgs e)
        {
            ViewModel.TelnetPassword = TelnetPasswordBox.Password;
        }

        private void ConnectButton_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                ViewModel.ConnectDisconnect();
            }
            catch (Exception ex)
            {
                _shellExecution.LogMessage(ex.Message, "error");
            }
        }

        private async void BootExploitButton_Click(object sender, RoutedEventArgs e)
        {
            await ViewModel.RunBootExploitAsync();
        }

        private async void MountSdCardButton_Click(object sender, RoutedEventArgs e)
        {
            await ViewModel.MountSdCardAsync();
        }

        private async void UnmountSdButton_Click(object sender, RoutedEventArgs e)
        {
            await ViewModel.UnmountSdCardAsync();
        }

        private void SendCtrlC_Click(object sender, RoutedEventArgs e)
        {
            if (!_serialService.IsConnected) return;
            try
            {
                _serialService.WriteRaw(new byte[] { 3 });
            }
            catch (Exception ex)
            {
                _shellExecution.LogMessage($"Failed to send Ctrl+C: {ex.Message}", "error");
            }
        }

        private void ClearConsole_Click(object sender, RoutedEventArgs e)
        {
            ConsoleTerminal.Clear();
        }

        private void ExportLog_Click(object sender, RoutedEventArgs e)
        {
            ConsoleTerminal.ExportLog();
        }

        private DeviceExplorerWindow? _explorerWindow;

        private async void OpenExplorer_Click(object sender, RoutedEventArgs e)
        {
            if (!_serialService.IsConnected) return;

            if (_explorerWindow != null)
            {
                _explorerWindow.Activate();
                return;
            }

            Microsoft.UI.WindowId parentWindowId = new Microsoft.UI.WindowId { Value = (ulong)ParentWindowHwnd };
            _explorerWindow = new DeviceExplorerWindow(parentWindowId);

            var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(_explorerWindow);
            _explorerWindow.Explorer.ParentWindowHwnd = hwnd;
            _explorerWindow.Explorer.Initialize(_shellExecution, _serialService, _fileTransfer);
            _explorerWindow.Explorer.SetFtpCredentials(ViewModel.FtpUsername, ViewModel.FtpPassword);

            _explorerWindow.Closed += (s, ev) =>
            {
                _explorerWindow = null;
            };

            _explorerWindow.ExecuteFileRequested += path =>
            {
                CommandTextBox.Text = path;
                CommandTextBox.SelectionStart = CommandTextBox.Text.Length;
                CommandTextBox.Focus(FocusState.Programmatic);
            };

            _explorerWindow.Activate();

            string initialPath = await _shellExecution.GetCurrentDirectoryAsync();
            await _explorerWindow.Explorer.ShowAsync(initialPath);
        }

        private void OpenUploadDialog_Click(object sender, RoutedEventArgs e)
        {
            _ = ViewModel.QueryDeviceDirectoryAsync("upload");
        }

        private void OpenDownloadDialog_Click(object sender, RoutedEventArgs e)
        {
            _ = ViewModel.QueryDeviceDirectoryAsync("download");
        }

        private void SendCommand_Click(object sender, RoutedEventArgs e)
        {
            SubmitCommand();
        }

        private void CommandTextBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == Windows.System.VirtualKey.Enter)
            {
                SubmitCommand();
                e.Handled = true;
            }
        }

        private void SubmitCommand()
        {
            string cmd = CommandTextBox.Text;
            if (string.IsNullOrWhiteSpace(cmd) || !ViewModel.IsConnected) return;

            ViewModel.SubmitCommand(cmd);
            CommandTextBox.Text = "";
        }

        private void CommandTextBox_PreviewKeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (ViewModel.DirectMode)
            {
                if (!ViewModel.IsConnected) return;

                byte[]? bytes = _ansiEncoder.EncodeKey(e.Key, ViewModel.SelectedLineEnding);
                if (bytes != null)
                {
                    try
                    {
                        _serialService.WriteRaw(bytes);
                    }
                    catch (Exception ex)
                    {
                        System.Diagnostics.Debug.WriteLine($"Direct send error: {ex.Message}");
                    }
                    e.Handled = true;
                }
            }
            else
            {
                if (e.Key == Windows.System.VirtualKey.Up)
                {
                    CommandTextBox.Text = ViewModel.GetHistoryPrevious(CommandTextBox.Text);
                    CommandTextBox.SelectionStart = CommandTextBox.Text.Length;
                    e.Handled = true;
                }
                else if (e.Key == Windows.System.VirtualKey.Down)
                {
                    CommandTextBox.Text = ViewModel.GetHistoryNext();
                    CommandTextBox.SelectionStart = CommandTextBox.Text.Length;
                    e.Handled = true;
                }
            }
        }

        private void CommandTextBox_TextChanging(TextBox sender, TextBoxTextChangingEventArgs args)
        {
            if (ViewModel.DirectMode && ViewModel.IsConnected)
            {
                string txt = sender.Text;
                if (!string.IsNullOrEmpty(txt))
                {
                    byte[] bytes = Encoding.UTF8.GetBytes(txt);
                    try
                    {
                        _serialService.WriteRaw(bytes);
                    }
                    catch (Exception ex)
                    {
                        System.Diagnostics.Debug.WriteLine($"Direct write error: {ex.Message}");
                    }
                    sender.Text = "";
                }
            }
        }

        private void ConsoleTerminal_FileDropped(IReadOnlyList<StorageFile> files)
        {
            ViewModel.HandleFileDrop(files);
        }

        // ================= DIALOG / TRANSFER EVENTS =================

        private async void UploadDialog_UploadRequested(StorageFile file, string remotePath, bool verifyMd5)
        {
            await _fileTransfer.StartUploadAsync(file, remotePath, verifyMd5);
        }

        private async void UploadDialog_MultiUploadRequested(IReadOnlyList<StorageFile> files, string remoteDir, bool verifyMd5)
        {
            await _fileTransfer.StartMultiUploadAsync(files, remoteDir, verifyMd5);
        }

        private async void DownloadDialog_DownloadRequested(string remotePath)
        {
            try
            {
                string fileName = System.IO.Path.GetFileName(remotePath);
                if (string.IsNullOrEmpty(fileName)) fileName = "downloaded_file";
                string ext = System.IO.Path.GetExtension(fileName);
                if (string.IsNullOrEmpty(ext)) ext = ".bin";

                var dialogService = new WindowsDialogService(ParentWindowHwnd);
                string? localPath = await dialogService.PickSaveFileAsync(fileName, ext);
                if (localPath != null)
                {
                    StorageFile file = await StorageFile.GetFileFromPathAsync(localPath);
                    _fileTransfer.StartDownload(remotePath, file);
                }
            }
            catch (Exception ex)
            {
                _shellExecution.LogMessage($"Download initialization failed: {ex.Message}", "error");
            }
        }

        private async void UploadDialog_FtpUploadRequested(StorageFile? file, StorageFolder? folder, string remotePath, string username, string password)
        {
            try
            {
                await ViewModel.RunFtpUploadAsync(file, folder, remotePath, username, password);
            }
            finally
            {
                UploadOverlayDialog.Hide();
            }
        }

        private async void DownloadDialog_FtpDownloadRequested(string remotePath, string username, string password)
        {
            try
            {
                await ViewModel.RunFtpDownloadAsync(remotePath, username, password);
            }
            finally
            {
                DownloadOverlayDialog.Hide();
            }
        }

        // FileTransferService's FTP-backed methods (UploadFtpFileAsync, DownloadFtpFileAsync, etc.)
        // raise these events from inside a raw Task.Run background thread, not the UI thread —
        // every handler here must marshal onto DispatcherQueue before touching any UI element, or
        // WinUI throws RPC_E_WRONG_THREAD (0x8001010E) deep inside the UIElement property setter.
        private void FileTransfer_UploadComplete()
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                if (_explorerWindow != null)
                {
                    _ = _explorerWindow.Explorer.NavigateToAsync(_explorerWindow.Explorer.CurrentPath, false);
                }
                else
                {
                    UploadOverlayDialog.Hide();
                }
            });
        }

        private void FileTransfer_StatusChanged(string status)
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                if (_explorerWindow != null)
                {
                    _explorerWindow.Explorer.SetStatus(status);
                }
                else if (_fileTransfer.IsUploading)
                {
                    UploadOverlayDialog.SetStatus(status);
                }
                else if (_fileTransfer.IsDownloading || _fileTransfer.IsCapturingSize)
                {
                    DownloadOverlayDialog.SetStatus(status);
                }
            });
        }

        private void FileTransfer_ProgressChanged(double progress)
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                if (_explorerWindow != null)
                {
                    _explorerWindow.Explorer.SetProgress(progress);
                }
                else if (_fileTransfer.IsUploading)
                {
                    UploadOverlayDialog.SetProgress(progress);
                }
                else if (_fileTransfer.IsDownloading || _fileTransfer.IsCapturingSize)
                {
                    DownloadOverlayDialog.SetProgress(progress);
                }
            });
        }

        private void FileTransfer_DownloadComplete()
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                _shellExecution.LogMessage("--- File download successful ---", "info");
                if (_explorerWindow != null)
                {
                    _explorerWindow.Explorer.SetStatus("Download Complete!");
                    _explorerWindow.Explorer.SetProgress(100);
                    Task.Delay(1500).ContinueWith(t => {
                        DispatcherQueue.TryEnqueue(() => {
                            if (_explorerWindow != null)
                            {
                                _explorerWindow.Explorer.SetStatus("");
                            }
                        });
                    });
                }
                else
                {
                    DownloadOverlayDialog.Hide();
                    DownloadOverlayDialog.SetDownloadingState(false);
                }
            });
        }

        private void FileTransfer_TransferError(string error)
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                _shellExecution.LogMessage(error, "error");
                if (_explorerWindow != null)
                {
                    _explorerWindow.Explorer.SetStatus($"Error: {error}");
                    _explorerWindow.Explorer.SetProgress(0);
                }
                else
                {
                    UploadOverlayDialog.SetUploadingState(false);
                    DownloadOverlayDialog.SetDownloadingState(false);
                    UploadOverlayDialog.Hide();
                    DownloadOverlayDialog.Hide();
                }
            });
        }

        private void UpdateSidebarButtonsVisibility()
        {
            bool connectedAndIdle = ViewModel.IsConnectedAndIdle;
            string connType = ViewModel.SelectedConnectionType;

            if (connectedAndIdle)
            {
                if (connType == "serial")
                {
                    OpenUploadDialogBtn.Visibility = Visibility.Visible;
                    OpenDownloadDialogBtn.Visibility = Visibility.Visible;
                    UartTransferGrid.Visibility = Visibility.Visible;
                    OpenExplorerBtn.Visibility = Visibility.Collapsed;
                }
                else // "telnet"
                {
                    OpenUploadDialogBtn.Visibility = Visibility.Collapsed;
                    OpenDownloadDialogBtn.Visibility = Visibility.Collapsed;
                    UartTransferGrid.Visibility = Visibility.Collapsed;
                    OpenExplorerBtn.Visibility = Visibility.Visible;
                }
            }
            else
            {
                OpenUploadDialogBtn.Visibility = Visibility.Collapsed;
                OpenDownloadDialogBtn.Visibility = Visibility.Collapsed;
                UartTransferGrid.Visibility = Visibility.Collapsed;
                OpenExplorerBtn.Visibility = Visibility.Collapsed;
            }
        }
    }
}
