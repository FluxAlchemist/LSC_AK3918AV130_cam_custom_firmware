using System;
using System.IO.Ports;
using System.Text;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using System.Collections.Generic;
using System.Net;

namespace serial_console_dotnet.Services
{
    public enum ConnectionMode
    {
        Serial,
        Telnet
    }

    public class ConnectionService : IDisposable
    {
        public static string[] ScanSerialPorts()
        {
            return SerialPort.GetPortNames();
        }
        // Connection Mode
        public ConnectionMode Mode { get; private set; } = ConnectionMode.Serial;

        // Serial mode fields
        private SerialPort? _serialPort;

        // Telnet mode fields
        private TcpClient? _tcpClient;
        private NetworkStream? _networkStream;
        private Thread? _readThread;

        private bool _isConnected = false;

        public event Action<string>? DataReceived;
        public event Action<string>? ErrorOccurred;
        public event Action<bool, string>? ConnectionStateChanged;

        public bool IsConnected => _isConnected && (
            (Mode == ConnectionMode.Serial && _serialPort != null && _serialPort.IsOpen) ||
            (Mode == ConnectionMode.Telnet && _tcpClient != null && _tcpClient.Connected)
        );

        public string? PortName => Mode == ConnectionMode.Serial ? _serialPort?.PortName : null;
		public string? RemoteIp
		{
			get
			{
				if (Mode == ConnectionMode.Telnet && _tcpClient?.Client.RemoteEndPoint is IPEndPoint remoteEndPoint)
				{
					// If it's an IPv4-mapped IPv6 address, this converts it to native IPv4 (e.g., 192.168.50.4)
					var ipAddress = remoteEndPoint.Address.IsIPv4MappedToIPv6
						? remoteEndPoint.Address.MapToIPv4()
						: remoteEndPoint.Address;

					return $"{ipAddress}:{remoteEndPoint.Port}";
				}
				return null;
			}
		}

		public int? BaudRate => Mode == ConnectionMode.Serial ? _serialPort?.BaudRate : null;

        public void ConnectSerial(string portName, int baudRate)
        {
            if (_isConnected) return;

            Mode = ConnectionMode.Serial;
            var port = new SerialPort(portName, baudRate, Parity.None, 8, StopBits.One)
            {
                ReadTimeout = 500,
                WriteTimeout = 500,
                DtrEnable = true,
                RtsEnable = true
            };

            try
            {
                // SerialPort.Open() can hang indefinitely against a stale/ghost COM port (e.g.
                // one whose USB-serial adapter was just unplugged but still lingers in the port
                // enumeration), which used to freeze the UI thread forever with no way to recover.
                // Bound it with a timeout instead — the background Open() call is abandoned (and
                // left to fail/GC on its own) if it doesn't return in time.
                var openTask = Task.Run(() => port.Open());
                if (!openTask.Wait(TimeSpan.FromSeconds(3)))
                {
                    throw new TimeoutException($"Timed out opening {portName} — device may be unresponsive or disconnected.");
                }

                port.DataReceived += SerialPort_DataReceived;
                _serialPort = port;
                _isConnected = true;
                ConnectionStateChanged?.Invoke(true, portName);
            }
            catch (Exception ex)
            {
                _serialPort = null;
                // Don't close/dispose synchronously here — if we timed out above, the abandoned
                // Open() call may still be blocked in the driver holding this exact handle, and
                // Close() would just hang the UI thread in a different place. Let it clean up
                // in the background, same reasoning as Disconnect() below.
                Task.Run(() =>
                {
                    try { if (port.IsOpen) port.Close(); } catch { }
                    try { port.Dispose(); } catch { }
                });
                throw new Exception($"Failed to open port {portName}: {ex.Message}", ex);
            }
        }

        public void ConnectTelnet(string host, int port, string username = "", string password = "")
        {
            if (_isConnected) return;

            Mode = ConnectionMode.Telnet;
            try
            {
                _tcpClient = new TcpClient();

                // Synchronous connect with 5s timeout fallback
                var result = _tcpClient.BeginConnect(host, port, null, null);
                var success = result.AsyncWaitHandle.WaitOne(TimeSpan.FromSeconds(5));
                if (!success)
                {
                    throw new Exception("Connection timed out.");
                }
                _tcpClient.EndConnect(result);

                // Configure low-latency TCP Keep-Alives to detect sudden device power-off
                _tcpClient.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);
                _tcpClient.Client.SetSocketOption(SocketOptionLevel.Tcp, SocketOptionName.TcpKeepAliveTime, 3); // 3 seconds idle before first probe
                _tcpClient.Client.SetSocketOption(SocketOptionLevel.Tcp, SocketOptionName.TcpKeepAliveInterval, 1); // 1 second interval between retries

                _networkStream = _tcpClient.GetStream();

                // Subscribed BEFORE the read thread starts so the very first banner/prompt bytes
                // (which can arrive within milliseconds of connecting) aren't missed.
                if (!string.IsNullOrEmpty(username))
                {
                    StartTelnetAutoLogin(username, password);
                }

                _isConnected = true;
                ConnectionStateChanged?.Invoke(true, $"{host}:{port}");

                // Spin up reading thread
                _readThread = new Thread(ReadTelnetLoop)
                {
                    IsBackground = true
                };
                _readThread.Start();
            }
            catch (Exception ex)
            {
                Disconnect();
                throw new Exception($"Failed to connect to Telnet host {host}:{port}: {ex.Message}", ex);
            }
        }

        // Watches the incoming Telnet stream for the "login:"/"Password:" prompts (BusyBox
        // login, what this camera's telnetd presents) and answers them automatically, the same
        // way a human would type into the terminal. Runs on top of the normal DataReceived
        // pipeline, so the exchange still shows up in the console like any other output.
        private string? _pendingLoginUsername;
        private string? _pendingLoginPassword;
        private bool _sentLoginUsername;

        private void StartTelnetAutoLogin(string username, string password)
        {
            _pendingLoginUsername = username;
            _pendingLoginPassword = password;
            _sentLoginUsername = false;
            DataReceived += TelnetAutoLogin_DataReceived;
        }

        private void StopTelnetAutoLogin()
        {
            DataReceived -= TelnetAutoLogin_DataReceived;
            _pendingLoginUsername = null;
            _pendingLoginPassword = null;
        }

        private void TelnetAutoLogin_DataReceived(string text)
        {
            string lower = text.ToLowerInvariant();
            try
            {
                if (!_sentLoginUsername && lower.Contains("login:"))
                {
                    _sentLoginUsername = true;
                    Write(_pendingLoginUsername ?? "", "crlf");
                }
                else if (_sentLoginUsername && lower.Contains("password:"))
                {
                    Write(_pendingLoginPassword ?? "", "crlf");
                    // One shot — a second "login:"/"Password:" prompt after this almost always
                    // means the credentials were wrong, and retrying automatically would just
                    // spam a bad password at the device forever.
                    StopTelnetAutoLogin();
                }
            }
            catch (Exception ex)
            {
                ErrorOccurred?.Invoke($"Auto-login failed: {ex.Message}");
                StopTelnetAutoLogin();
            }
        }

        public void Disconnect()
        {
            if (!_isConnected) return;

            StopTelnetAutoLogin();
            _isConnected = false;
            string details = Mode == ConnectionMode.Serial 
                ? (_serialPort?.PortName ?? "") 
                : (_tcpClient != null ? "Telnet" : "");

            if (Mode == ConnectionMode.Serial && _serialPort != null)
            {
                var portToClose = _serialPort;
                _serialPort = null;
                portToClose.DataReceived -= SerialPort_DataReceived;

                // SerialPort.Close() can hang indefinitely if the underlying USB-serial device
                // was physically unplugged (a long-standing System.IO.Ports bug — the internal
                // read thread never unblocks waiting on a handle that no longer refers to a real
                // device). That used to make the Disconnect button permanently unresponsive.
                // Flip our own state and notify listeners immediately regardless, and let the
                // actual OS-level close happen on a detached background thread — a leaked close
                // thread is far preferable to a frozen UI.
                Task.Run(() =>
                {
                    try { if (portToClose.IsOpen) portToClose.Close(); }
                    catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"Error closing port: {ex.Message}"); }
                    finally { try { portToClose.Dispose(); } catch { } }
                });
            }
            else if (Mode == ConnectionMode.Telnet && _tcpClient != null)
            {
                try
                {
                    _networkStream?.Close();
                    _tcpClient.Close();
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine($"Error closing TCP client: {ex.Message}");
                }
                finally
                {
                    _networkStream = null;
                    _tcpClient = null;
                    _readThread = null;
                }
            }

            ConnectionStateChanged?.Invoke(false, details);
        }

        private void SerialPort_DataReceived(object sender, SerialDataReceivedEventArgs e)
        {
            if (_serialPort == null || !_serialPort.IsOpen) return;

            try
            {
                int bytesToRead = _serialPort.BytesToRead;
                byte[] buffer = new byte[bytesToRead];
                _serialPort.Read(buffer, 0, bytesToRead);
                string text = Encoding.UTF8.GetString(buffer);
                DataReceived?.Invoke(text);
            }
            catch (Exception ex)
            {
                ErrorOccurred?.Invoke($"Read error: {ex.Message}");
                // A read failure on an already-open handle almost always means the underlying
                // USB-serial device just disappeared (cable pulled, device powered off), not a
                // transient hiccup — disconnect immediately instead of leaving the app believing
                // it's still connected to a port that no longer exists.
                Disconnect();
            }
        }

        private void ReadTelnetLoop()
        {
            byte[] buffer = new byte[4096];
            List<byte> outputBuffer = new List<byte>();

            // Telnet Option Bytes
            const byte IAC = 255;
            const byte DONT = 254;
            const byte DO = 253;
            const byte WONT = 252;
            const byte WILL = 251;
            const byte SB = 250;
            const byte SE = 240;

            int telnetState = 0; // 0=normal, 1=IAC, 2=Command(DO/DONT/WILL/WONT), 4=Subnegotiation
            byte currentCommand = 0;

            while (_isConnected && _tcpClient != null && _tcpClient.Connected)
            {
                try
                {
                    int bytesRead = _networkStream?.Read(buffer, 0, buffer.Length) ?? 0;
                    if (bytesRead <= 0)
                    {
                        // Clean disconnect on host EOF
                        ErrorOccurred?.Invoke("Telnet connection closed by remote host.");
                        Disconnect();
                        break;
                    }

                    outputBuffer.Clear();
                    for (int i = 0; i < bytesRead; i++)
                    {
                        byte b = buffer[i];

                        if (telnetState == 0) // Normal text routing
                        {
                            if (b == IAC)
                            {
                                telnetState = 1;
                            }
                            else
                            {
                                outputBuffer.Add(b);
                            }
                        }
                        else if (telnetState == 1) // Command negotiation
                        {
                            if (b == WILL || b == WONT || b == DO || b == DONT)
                            {
                                currentCommand = b;
                                telnetState = 2; // Expecting option byte
                            }
                            else if (b == SB)
                            {
                                telnetState = 4; // Subnegotiation start
                            }
                            else if (b == IAC)
                            {
                                outputBuffer.Add(IAC); // Escaped IAC
                                telnetState = 0;
                            }
                            else
                            {
                                telnetState = 0; // Single byte commands
                            }
                        }
                        else if (telnetState == 2) // Option byte
                        {
                            byte option = b;
                            telnetState = 0;

                            // Auto-respond to negotiation: Reject everything (WONT/DONT) to keep simple
                            byte responseCmd = 0;
                            if (currentCommand == DO) responseCmd = WONT;
                            else if (currentCommand == DONT) responseCmd = WONT;
                            else if (currentCommand == WILL) responseCmd = DONT;
                            else if (currentCommand == WONT) responseCmd = DONT;

                            if (responseCmd != 0 && _networkStream != null)
                            {
                                byte[] reply = new byte[] { IAC, responseCmd, option };
                                _networkStream.Write(reply, 0, reply.Length);
                            }
                        }
                        else if (telnetState == 4) // Subnegotiation
                        {
                            if (b == SE)
                            {
                                if (i > 0 && buffer[i - 1] == IAC)
                                {
                                    telnetState = 0;
                                }
                            }
                        }
                    }

                    if (outputBuffer.Count > 0)
                    {
                        string text = Encoding.UTF8.GetString(outputBuffer.ToArray());
                        DataReceived?.Invoke(text);
                    }
                }
                catch (Exception ex)
                {
                    if (_isConnected)
                    {
                        ErrorOccurred?.Invoke($"Telnet read error: {ex.Message}");
                        Disconnect();
                    }
                    break;
                }
            }
        }

        public void Write(string command, string ending)
        {
            if (!IsConnected)
            {
                throw new InvalidOperationException("Device is disconnected.");
            }

            try
            {
                string payload = command;
                if (ending == "crlf") payload += "\r\n";
                else if (ending == "lf") payload += "\n";
                else if (ending == "cr") payload += "\r";

                byte[] buffer = Encoding.UTF8.GetBytes(payload);
                if (Mode == ConnectionMode.Serial && _serialPort != null)
                {
                    _serialPort.Write(buffer, 0, buffer.Length);
                }
                else if (Mode == ConnectionMode.Telnet && _networkStream != null)
                {
                    _networkStream.Write(buffer, 0, buffer.Length);
                }
            }
            catch (Exception ex)
            {
                throw new Exception($"Write failed: {ex.Message}", ex);
            }
        }

        public void WriteRaw(byte[] bytes)
        {
            if (!IsConnected)
            {
                throw new InvalidOperationException("Device is disconnected.");
            }

            try
            {
                if (Mode == ConnectionMode.Serial && _serialPort != null)
                {
                    _serialPort.Write(bytes, 0, bytes.Length);
                }
                else if (Mode == ConnectionMode.Telnet && _networkStream != null)
                {
                    _networkStream.Write(bytes, 0, bytes.Length);
                }
            }
            catch (Exception ex)
            {
                throw new Exception($"Raw write failed: {ex.Message}", ex);
            }
        }

        public void Dispose()
        {
            Disconnect();
        }
    }
}
