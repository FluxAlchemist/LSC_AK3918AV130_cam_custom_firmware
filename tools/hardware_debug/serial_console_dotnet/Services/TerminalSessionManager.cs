using System;
using System.Collections.Concurrent;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace serial_console_dotnet.Services
{
    public class TerminalSessionManager : IDisposable
    {
        private readonly ConnectionService _connectionService;
        private readonly AnsiParserService _ansiParser;
        private readonly ShellExecutionService _shellExecution;
        private readonly FileTransferService _fileTransfer;

        private readonly ConcurrentQueue<string> _incomingQueue = new ConcurrentQueue<string>();
        private readonly CancellationTokenSource _cts = new CancellationTokenSource();
        private Task? _flushTask;

        // Fired when terminal output is ready to be appended to the console
        public event Action<string>? RawDataReceived;
        
        public TerminalSessionManager(
            ConnectionService connectionService,
            AnsiParserService ansiParser,
            ShellExecutionService shellExecution,
            FileTransferService fileTransfer)
        {
            _connectionService = connectionService;
            _ansiParser = ansiParser;
            _shellExecution = shellExecution;
            _fileTransfer = fileTransfer;

            _connectionService.DataReceived += ConnectionService_DataReceived;
            
            // Start background flush loop
            _flushTask = Task.Run(FlushLoopAsync);
        }

        private void ConnectionService_DataReceived(string data)
        {
            _incomingQueue.Enqueue(data);
        }

        private async Task FlushLoopAsync()
        {
            while (!_cts.Token.IsCancellationRequested)
            {
                if (!_incomingQueue.IsEmpty)
                {
                    var sb = new StringBuilder();
                    while (_incomingQueue.TryDequeue(out string? chunk))
                    {
                        sb.Append(chunk);
                    }

                    string incomingText = sb.ToString();

                    if (_shellExecution.IsExecuting)
                    {
                        _shellExecution.HandleIncomingText(incomingText);
                    }
                    else
                    {
                        // Fire raw output event so UI can display it
                        RawDataReceived?.Invoke(incomingText);
                        
                        // Also feed to file transfer
                        _fileTransfer.FeedIncomingText(incomingText);
                    }
                }

                try
                {
                    await Task.Delay(50, _cts.Token);
                }
                catch (TaskCanceledException)
                {
                    break;
                }
            }
        }

        public void Dispose()
        {
            _connectionService.DataReceived -= ConnectionService_DataReceived;
            _cts.Cancel();
            try
            {
                _flushTask?.Wait(500);
            }
            catch { }
            _cts.Dispose();
        }
    }
}
