using System;
using System.Collections.Generic;

namespace serial_console_dotnet.Services
{
    public class ConnectionConfig
    {
        public string ConnectionType { get; set; } = "serial";
        public string SerialPort { get; set; } = "";
        public int BaudRate { get; set; } = 115200;
        public string TelnetHost { get; set; } = "192.168.1.1";
        public string TelnetPort { get; set; } = "23";
        public string TelnetUsername { get; set; } = "root";
        public string TelnetPassword { get; set; } = "admin";
        public string FtpUsername { get; set; } = "root";
        public string FtpPassword { get; set; } = "";
    }

    public class ConnectionOrchestrator
    {
        private readonly ConnectionService _connectionService;

        public ConnectionOrchestrator(ConnectionService connectionService)
        {
            _connectionService = connectionService;
        }

        public string[] ScanPorts()
        {
            return ConnectionService.ScanSerialPorts();
        }

        public void ToggleConnection(ConnectionConfig config)
        {
            if (_connectionService.IsConnected)
            {
                _connectionService.Disconnect();
            }
            else
            {
                if (config.ConnectionType == "telnet")
                {
                    string host = config.TelnetHost.Trim();
                    if (string.IsNullOrEmpty(host))
                    {
                        throw new ArgumentException("IP address cannot be empty.");
                    }
                    if (!int.TryParse(config.TelnetPort, out int port))
                    {
                        port = 23;
                    }
                    _connectionService.ConnectTelnet(host, port, config.TelnetUsername.Trim(), config.TelnetPassword);
                }
                else
                {
                    if (string.IsNullOrEmpty(config.SerialPort))
                    {
                        throw new ArgumentException("Please select a valid COM port.");
                    }
                    _connectionService.ConnectSerial(config.SerialPort, config.BaudRate);
                }
            }
        }

        public ConnectionConfig LoadSettings(HistoryService historyService)
        {
            var settings = SettingsService.Load();
            if (settings.CommandHistory != null)
            {
                historyService.Seed(settings.CommandHistory);
            }

            int.TryParse(settings.BaudRate, out int baud);
            if (baud == 0) baud = 115200;

            return new ConnectionConfig
            {
                ConnectionType = settings.ConnectionType ?? "serial",
                SerialPort = settings.SerialPort ?? "",
                BaudRate = baud,
                TelnetHost = settings.TelnetHost ?? "192.168.1.1",
                TelnetPort = settings.TelnetPort ?? "23",
                TelnetUsername = settings.TelnetUsername ?? "root",
                TelnetPassword = settings.TelnetPassword ?? "admin",
                FtpUsername = settings.FtpUsername ?? "root",
                FtpPassword = settings.FtpPassword ?? ""
            };
        }

        public void SaveSettings(ConnectionConfig config, bool showTimestamps, IEnumerable<string> commandHistory)
        {
            var settings = SettingsService.Load();
            
            settings.ConnectionType = config.ConnectionType;
            settings.SerialPort = config.SerialPort;
            settings.BaudRate = config.BaudRate.ToString();
            settings.TelnetHost = config.TelnetHost;
            settings.TelnetPort = config.TelnetPort;
            settings.TelnetUsername = config.TelnetUsername;
            settings.TelnetPassword = config.TelnetPassword;
            settings.ShowTimestamps = showTimestamps;
            settings.FtpUsername = config.FtpUsername;
            settings.FtpPassword = config.FtpPassword;
            settings.CommandHistory = new List<string>(commandHistory);

            SettingsService.Save(settings);
        }
    }
}
