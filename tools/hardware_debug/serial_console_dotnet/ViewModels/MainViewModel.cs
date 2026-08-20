using System;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using Windows.Storage;
using serial_console_dotnet.Services;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Media;

namespace serial_console_dotnet.ViewModels
{
    public class MainViewModel : INotifyPropertyChanged
    {
        private readonly ConnectionService _connectionService;
        private readonly ShellExecutionService _shellExecution;
        private readonly FileTransferService _fileTransfer;
        private readonly BootExploitService _bootExploit;
        private readonly SdCardService _sdCard;
        private readonly HistoryService _historyService;
        private readonly ConnectionOrchestrator _connectionOrchestrator;

        // Captured on the UI thread at construction time. ConnectionStateChanged/ExecutionStateChanged
        // can fire from background threads (e.g. ConnectionService.ReadTelnetLoop on a remote-host
        // disconnect), and the handlers below touch XAML objects (SolidColorBrush) which throw
        // RPC_E_WRONG_THREAD (0x8001010E) if constructed off the UI thread.
        private readonly DispatcherQueue _dispatcherQueue;

        public event PropertyChangedEventHandler? PropertyChanged;

        // Connection State Properties
        private bool _isConnected;
        public bool IsConnected
        {
            get => _isConnected;
            set => SetProperty(ref _isConnected, value);
        }

        private string _statusText = "Disconnected";
        public string StatusText
        {
            get => _statusText;
            set => SetProperty(ref _statusText, value);
        }

        private SolidColorBrush _statusColor = new SolidColorBrush(Colors.Red);
        public SolidColorBrush StatusColor
        {
            get => _statusColor;
            set => SetProperty(ref _statusColor, value);
        }

        private string _connectionHeaderTitle = "Terminal Console (Offline)";
        public string ConnectionHeaderTitle
        {
            get => _connectionHeaderTitle;
            set => SetProperty(ref _connectionHeaderTitle, value);
        }

        // Connection Setup Properties
        private string _selectedConnectionType = "serial";
        public string SelectedConnectionType
        {
            get => _selectedConnectionType;
            set
            {
                if (SetProperty(ref _selectedConnectionType, value))
                {
                    OnPropertyChanged(nameof(IsTelnetSelected));
                    OnPropertyChanged(nameof(IsSerialSelected));
                }
            }
        }

        public bool IsTelnetSelected => _selectedConnectionType == "telnet";
        public bool IsSerialSelected => _selectedConnectionType == "serial";

        private string _selectedPort = "";
        public string SelectedPort
        {
            get => _selectedPort;
            set => SetProperty(ref _selectedPort, value);
        }

        private int _selectedBaudRate = 115200;
        public int SelectedBaudRate
        {
            get => _selectedBaudRate;
            set => SetProperty(ref _selectedBaudRate, value);
        }

        private string _telnetHost = "192.168.1.1";
        public string TelnetHost
        {
            get => _telnetHost;
            set => SetProperty(ref _telnetHost, value);
        }

        private string _telnetPort = "23";
        public string TelnetPort
        {
            get => _telnetPort;
            set => SetProperty(ref _telnetPort, value);
        }

        private string _telnetUsername = "root";
        public string TelnetUsername
        {
            get => _telnetUsername;
            set => SetProperty(ref _telnetUsername, value);
        }

        private string _telnetPassword = "admin";
        public string TelnetPassword
        {
            get => _telnetPassword;
            set => SetProperty(ref _telnetPassword, value);
        }

        // Execution overlay block state
        private bool _isExecuting;
        public bool IsExecuting
        {
            get => _isExecuting;
            set => SetProperty(ref _isExecuting, value);
        }

        // Terminal Mode properties
        private bool _directMode;
        public bool DirectMode
        {
            get => _directMode;
            set => SetProperty(ref _directMode, value);
        }

        private string _selectedLineEnding = "lf";
        public string SelectedLineEnding
        {
            get => _selectedLineEnding;
            set => SetProperty(ref _selectedLineEnding, value);
        }

        private bool _showTimestamps = true;
        public bool ShowTimestamps
        {
            get => _showTimestamps;
            set
            {
                if (SetProperty(ref _showTimestamps, value))
                {
                    SaveSettings();
                }
            }
        }

        // FTP credentials
        private string _ftpUsername = "root";
        public string FtpUsername
        {
            get => _ftpUsername;
            set => SetProperty(ref _ftpUsername, value);
        }

        private string _ftpPassword = "";
        public string FtpPassword
        {
            get => _ftpPassword;
            set => SetProperty(ref _ftpPassword, value);
        }

        public ObservableCollection<string> AvailablePorts { get; } = new ObservableCollection<string>();

        // Helper UI binding properties
        public bool CanConnect => !IsExecuting;
        public string ConnectButtonText => IsConnected ? "Disconnect" : "Connect Device";
        public string ConnectButtonIcon => IsConnected ? "\uE71A" : "\uE768";
        public bool IsConnectedAndIdle => IsConnected && !IsExecuting;

        public MainViewModel(
            ConnectionService connectionService,
            ShellExecutionService shellExecution,
            FileTransferService fileTransfer,
            BootExploitService bootExploit,
            SdCardService sdCard,
            HistoryService historyService,
            ConnectionOrchestrator connectionOrchestrator)
        {
            _connectionService = connectionService;
            _shellExecution = shellExecution;
            _fileTransfer = fileTransfer;
            _bootExploit = bootExploit;
            _sdCard = sdCard;
            _historyService = historyService;
            _connectionOrchestrator = connectionOrchestrator;

            _dispatcherQueue = DispatcherQueue.GetForCurrentThread();

            _connectionService.ConnectionStateChanged += (isConnected, details) =>
                _dispatcherQueue.TryEnqueue(() => ConnectionService_ConnectionStateChanged(isConnected, details));
            _shellExecution.ExecutionStateChanged += (isExecuting) =>
                _dispatcherQueue.TryEnqueue(() => ShellExecution_ExecutionStateChanged(isExecuting));

            LoadSettings();
            ScanPorts();
        }

        public void ScanPorts()
        {
            AvailablePorts.Clear();
            var ports = _connectionOrchestrator.ScanPorts();
            foreach (var port in ports)
            {
                AvailablePorts.Add(port);
            }

            if (AvailablePorts.Count > 0)
            {
                if (string.IsNullOrEmpty(SelectedPort) || !AvailablePorts.Contains(SelectedPort))
                {
                    int idx = AvailablePorts.IndexOf("COM4");
                    SelectedPort = idx != -1 ? AvailablePorts[idx] : AvailablePorts[0];
                }
            }
        }

        public void ConnectDisconnect()
        {
            var config = GetCurrentConfig();
            _connectionOrchestrator.ToggleConnection(config);
            SaveSettings();
        }

        public async Task RunBootExploitAsync()
        {
            if (!IsConnected) return;
            await _bootExploit.StartBootExploitAsync();
        }

        public async Task MountSdCardAsync()
        {
            if (!IsConnected) return;
            await _sdCard.MountSdCardAsync();
        }

        public async Task UnmountSdCardAsync()
        {
            if (!IsConnected) return;
            await _sdCard.UnmountSdCardAsync();
        }

        public void SubmitCommand(string cmd)
        {
            if (string.IsNullOrWhiteSpace(cmd) || !IsConnected) return;

            _connectionService.Write(cmd, "lf");
            _historyService.Add(cmd);
            SaveSettings();
        }

        public string GetHistoryPrevious(string currentText)
        {
            return _historyService.GetPrevious(currentText);
        }

        public string GetHistoryNext()
        {
            return _historyService.GetNext();
        }

        public async Task RunFtpUploadAsync(StorageFile? file, StorageFolder? folder, string remotePath, string username, string password)
        {
            FtpUsername = username;
            FtpPassword = password;
            SaveSettings();

            string host = _connectionService.RemoteIp ?? "192.168.1.1";
            await _fileTransfer.RunFtpUploadOrchestrationAsync(host, username, password, remotePath, file?.Path, folder?.Path);
        }

        public async Task RunFtpDownloadAsync(string remotePath, string username, string password)
        {
            FtpUsername = username;
            FtpPassword = password;
            SaveSettings();

            string host = _connectionService.RemoteIp ?? "192.168.1.1";
            await _fileTransfer.StartInteractiveFtpDownloadAsync(host, username, password, remotePath);
        }

        public event Action<string, string, IReadOnlyList<StorageFile>?>? ShowDirectoryDialogRequested;

        private IReadOnlyList<StorageFile>? _droppedFilesForUpload = null;

        public async Task QueryDeviceDirectoryAsync(string dialogType)
        {
            if (!IsConnected) return;

            string targetDir = await _shellExecution.GetCurrentDirectoryAsync();
            ShowDirectoryDialogRequested?.Invoke(dialogType, targetDir, _droppedFilesForUpload);
            _droppedFilesForUpload = null;
        }

        public void HandleFileDrop(IReadOnlyList<StorageFile> files)
        {
            _droppedFilesForUpload = files;
            _ = QueryDeviceDirectoryAsync("upload");
        }

        public void LoadSettings()
        {
            var config = _connectionOrchestrator.LoadSettings(_historyService);
            SelectedConnectionType = config.ConnectionType;
            SelectedPort = config.SerialPort;
            SelectedBaudRate = config.BaudRate;
            TelnetHost = config.TelnetHost;
            TelnetPort = config.TelnetPort;
            TelnetUsername = config.TelnetUsername;
            TelnetPassword = config.TelnetPassword;
            FtpUsername = config.FtpUsername;
            FtpPassword = config.FtpPassword;

            var settings = SettingsService.Load();
            ShowTimestamps = settings.ShowTimestamps;
        }

        public void SaveSettings()
        {
            var config = GetCurrentConfig();
            _connectionOrchestrator.SaveSettings(config, ShowTimestamps, _historyService.Items);
        }

        private ConnectionConfig GetCurrentConfig()
        {
            return new ConnectionConfig
            {
                ConnectionType = SelectedConnectionType,
                SerialPort = SelectedPort,
                BaudRate = SelectedBaudRate,
                TelnetHost = TelnetHost,
                TelnetPort = TelnetPort,
                TelnetUsername = TelnetUsername,
                TelnetPassword = TelnetPassword,
                FtpUsername = FtpUsername,
                FtpPassword = FtpPassword
            };
        }

        private void ConnectionService_ConnectionStateChanged(bool isConnected, string details)
        {
            IsConnected = isConnected;
            if (isConnected)
            {
                StatusText = $"Connected ({details})";
                StatusColor = new SolidColorBrush(Colors.Green);
                ConnectionHeaderTitle = _connectionService.Mode == ConnectionMode.Serial
                    ? $"Terminal Console ({_connectionService.BaudRate} Baud)"
                    : $"Terminal Console (Telnet {details})";
            }
            else
            {
                StatusText = "Disconnected";
                StatusColor = new SolidColorBrush(Colors.Red);
                ConnectionHeaderTitle = "Terminal Console (Offline)";
            }

            OnPropertyChanged(nameof(CanConnect));
            OnPropertyChanged(nameof(ConnectButtonText));
            OnPropertyChanged(nameof(ConnectButtonIcon));
            OnPropertyChanged(nameof(IsConnectedAndIdle));
        }

        private void ShellExecution_ExecutionStateChanged(bool isExecuting)
        {
            IsExecuting = isExecuting;
            if (isExecuting)
            {
                StatusText = "Running query...";
                StatusColor = new SolidColorBrush(Colors.Orange);
            }
            else
            {
                ConnectionService_ConnectionStateChanged(_connectionService.IsConnected, _connectionService.IsConnected ? (_connectionService.Mode == ConnectionMode.Serial ? _connectionService.PortName ?? "" : _connectionService.RemoteIp ?? "") : "");
            }

            OnPropertyChanged(nameof(CanConnect));
            OnPropertyChanged(nameof(ConnectButtonText));
            OnPropertyChanged(nameof(ConnectButtonIcon));
            OnPropertyChanged(nameof(IsConnectedAndIdle));
        }

        protected bool SetProperty<T>(ref T storage, T value, [CallerMemberName] string? propertyName = null)
        {
            if (EqualityComparer<T>.Default.Equals(storage, value)) return false;
            storage = value;
            OnPropertyChanged(propertyName);
            return true;
        }

        protected void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        {
			PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
}
