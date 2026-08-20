using System.Collections.Generic;

namespace serial_console_dotnet.Services
{
    public class HistoryService
    {
        private readonly List<string> _commandHistory = new List<string>();
        private int _historyIndex = -1;
        private string _tempCommand = "";
        private readonly int _maxHistory;

        public List<string> Items => _commandHistory;

        public HistoryService(int maxHistory = 50)
        {
            _maxHistory = maxHistory;
        }

        public void Seed(IEnumerable<string> items)
        {
            _commandHistory.Clear();
            if (items != null)
            {
                foreach (var item in items)
                {
                    _commandHistory.Add(item);
                }
            }
            _historyIndex = -1;
        }

        public void Add(string command)
        {
            if (string.IsNullOrWhiteSpace(command)) return;

            // Don't add duplicate of the last command
            if (_commandHistory.Count > 0 && _commandHistory[_commandHistory.Count - 1] == command)
            {
                _historyIndex = -1;
                return;
            }

            _commandHistory.Add(command);
            if (_commandHistory.Count > _maxHistory)
            {
                _commandHistory.RemoveAt(0);
            }
            _historyIndex = -1;
        }

        public string GetPrevious(string currentInput)
        {
            if (_commandHistory.Count == 0) return currentInput;

            if (_historyIndex == -1)
            {
                _tempCommand = currentInput;
                _historyIndex = _commandHistory.Count - 1;
            }
            else if (_historyIndex > 0)
            {
                _historyIndex--;
            }

            return _commandHistory[_historyIndex];
        }

        public string GetNext()
        {
            if (_historyIndex == -1) return _tempCommand;

            _historyIndex++;
            if (_historyIndex >= _commandHistory.Count)
            {
                _historyIndex = -1;
                return _tempCommand;
            }

            return _commandHistory[_historyIndex];
        }

        public void ResetIndex()
        {
            _historyIndex = -1;
        }

        public void Clear()
        {
            _commandHistory.Clear();
            _historyIndex = -1;
            _tempCommand = "";
        }
    }
}
