using System;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace serial_console_dotnet.Services
{
    /// <summary>
    /// TCP client for ak_rtsp's control/tuning server (port 8091 by default —
    /// see ak_rtsp/control.h for the line
    /// protocol: LIST / GET &lt;param&gt; / SET &lt;param&gt; &lt;value&gt;,
    /// with async "LOG " lines interleaved).
    ///
    /// Deliberately a standalone class, not an extension of
    /// <see cref="ConnectionService"/> — the Camera Tuning tab needs its own
    /// concurrent socket while the main Serial/Telnet terminal connection
    /// stays independently connected (both are commonly open at once:
    /// terminal for boot logs, tuning tab for live parameter changes).
    /// </summary>
    public class CameraControlService : IDisposable
    {
        private TcpClient? _tcpClient;
        private NetworkStream? _stream;
        private Thread? _readThread;
        private bool _isConnected;

        /// <summary>Raised once per received line, raw (no LOG-prefix routing —
        /// the ViewModel decides how to interpret each line).</summary>
        public event Action<string>? LineReceived;
        public event Action<string>? ErrorOccurred;
        public event Action<bool>? ConnectionStateChanged;

        public bool IsConnected => _isConnected && _tcpClient != null && _tcpClient.Connected;

        public void Connect(string host, int port)
        {
            if (_isConnected) return;

            try
            {
                _tcpClient = new TcpClient();

                var result = _tcpClient.BeginConnect(host, port, null, null);
                var success = result.AsyncWaitHandle.WaitOne(TimeSpan.FromSeconds(5));
                if (!success)
                {
                    throw new Exception("Connection timed out.");
                }
                _tcpClient.EndConnect(result);

                _stream = _tcpClient.GetStream();
                _isConnected = true;
                ConnectionStateChanged?.Invoke(true);

                _readThread = new Thread(ReadLoop) { IsBackground = true };
                _readThread.Start();
            }
            catch (Exception ex)
            {
                Disconnect();
                throw new Exception($"Failed to connect to control server {host}:{port}: {ex.Message}", ex);
            }
        }

        public void Disconnect()
        {
            if (!_isConnected) return;
            _isConnected = false;

            try
            {
                _stream?.Close();
                _tcpClient?.Close();
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error closing control connection: {ex.Message}");
            }
            finally
            {
                _stream = null;
                _tcpClient = null;
                _readThread = null;
            }

            ConnectionStateChanged?.Invoke(false);
        }

        /// <summary>Fire-and-forget line send — appends the trailing newline
        /// the protocol expects.</summary>
        public void Send(string line)
        {
            if (!IsConnected || _stream == null)
            {
                throw new InvalidOperationException("Control server is disconnected.");
            }

            try
            {
                byte[] buffer = Encoding.ASCII.GetBytes(line + "\n");
                _stream.Write(buffer, 0, buffer.Length);
            }
            catch (Exception ex)
            {
                throw new Exception($"Control send failed: {ex.Message}", ex);
            }
        }

        private void ReadLoop()
        {
            byte[] buffer = new byte[4096];
            var lineBuf = new StringBuilder();

            while (_isConnected && _tcpClient != null && _tcpClient.Connected)
            {
                try
                {
                    int bytesRead = _stream?.Read(buffer, 0, buffer.Length) ?? 0;
                    if (bytesRead <= 0)
                    {
                        ErrorOccurred?.Invoke("Control connection closed by remote host.");
                        Disconnect();
                        break;
                    }

                    for (int i = 0; i < bytesRead; i++)
                    {
                        char c = (char)buffer[i];
                        if (c == '\r') continue;
                        if (c == '\n')
                        {
                            LineReceived?.Invoke(lineBuf.ToString());
                            lineBuf.Clear();
                        }
                        else
                        {
                            lineBuf.Append(c);
                        }
                    }
                }
                catch (Exception ex)
                {
                    if (_isConnected)
                    {
                        ErrorOccurred?.Invoke($"Control read error: {ex.Message}");
                        Disconnect();
                    }
                    break;
                }
            }
        }

        public void Dispose() => Disconnect();
    }
}
