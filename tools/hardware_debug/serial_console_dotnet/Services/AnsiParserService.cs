using System;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;
using Windows.UI;

namespace serial_console_dotnet.Services
{
    public class TextSegment
    {
        public string Text { get; set; } = "";
        public Color FgColor { get; set; }
        public Color? BgColor { get; set; }
        public bool IsBold { get; set; }
    }

    public class AnsiParserService
    {
        private Color _activeFgColor = ColorFromHex("#CCCCCC"); // default Light Gray
        private Color? _activeBgColor = null;
        private bool _activeBold = false;
        private string _streamBuffer = "";

        private static readonly Color DefaultFgColor = ColorFromHex("#CCCCCC");

        private static readonly Dictionary<int, Color> AnsiColorMap = new Dictionary<int, Color>
        {
            { 30, ColorFromHex("#0C0C0C") }, { 31, ColorFromHex("#E74856") },
            { 32, ColorFromHex("#16C60C") }, { 33, ColorFromHex("#F9F1A5") },
            { 34, ColorFromHex("#3B78FF") }, { 35, ColorFromHex("#B4009E") },
            { 36, ColorFromHex("#61D6D6") }, { 37, ColorFromHex("#CCCCCC") },
            { 90, ColorFromHex("#767676") }, { 91, ColorFromHex("#E74856") },
            { 92, ColorFromHex("#16C60C") }, { 93, ColorFromHex("#F9F1A5") },
            { 94, ColorFromHex("#3B78FF") }, { 95, ColorFromHex("#B4009E") },
            { 96, ColorFromHex("#61D6D6") }, { 97, ColorFromHex("#F2F2F2") }
        };

        private static readonly Dictionary<int, Color> AnsiBgMap = new Dictionary<int, Color>
        {
            { 40, ColorFromHex("#0C0C0C") }, { 41, ColorFromHex("#C50F1F") },
            { 42, ColorFromHex("#13A10E") }, { 43, ColorFromHex("#C19C00") },
            { 44, ColorFromHex("#0037DA") }, { 45, ColorFromHex("#881798") },
            { 46, ColorFromHex("#3A96DD") }, { 47, ColorFromHex("#CCCCCC") }
        };

        private static Color ColorFromHex(string hex)
        {
            hex = hex.Replace("#", "");
            if (hex.Length == 6)
            {
                byte r = Convert.ToByte(hex.Substring(0, 2), 16);
                byte g = Convert.ToByte(hex.Substring(2, 2), 16);
                byte b = Convert.ToByte(hex.Substring(4, 2), 16);
                return Color.FromArgb(255, r, g, b);
            }
            return Color.FromArgb(255, 204, 204, 204); // Default gray
        }

        public void Reset()
        {
            _activeFgColor = DefaultFgColor;
            _activeBgColor = null;
            _activeBold = false;
            _streamBuffer = "";
        }

        public List<List<TextSegment>> ProcessIncomingText(string text, out string cleanTextForLogs)
        {
            // Combine with buffered partial escape codes
            string fullText = _streamBuffer + text;

            // Strip partial escape sequence at the end
            int lastEsc = fullText.LastIndexOf('\x1b');
            if (lastEsc != -1)
            {
                string tail = fullText.Substring(lastEsc);
                bool matchesComplete = Regex.IsMatch(tail, @"^\x1b\[[0-9;?]*[a-zA-Z]");
                if (!matchesComplete)
                {
                    if (Regex.IsMatch(tail, @"^\x1b(\[([0-9;?]*)?)?$"))
                    {
                        _streamBuffer = tail;
                        fullText = fullText.Substring(0, lastEsc);
                    }
                    else
                    {
                        _streamBuffer = "";
                    }
                }
                else
                {
                    _streamBuffer = "";
                }
            }
            else
            {
                _streamBuffer = "";
            }

            string cleanTextCombined = fullText.Replace("\r\n", "\n");

            var linesResult = new List<List<TextSegment>>();
            string[] parts = cleanTextCombined.Split('\n');
            var ansiRegex = new Regex(@"\x1b\[([0-9;?]*)([a-zA-Z])");

            var logSb = new StringBuilder();

            for (int i = 0; i < parts.Length; i++)
            {
                string part = parts[i];
                var currentLine = new List<TextSegment>();

                if (part.Contains('\x1b'))
                {
                    int lastIdx = 0;
                    var matches = ansiRegex.Matches(part);
                    foreach (Match match in matches)
                    {
                        string before = part.Substring(lastIdx, match.Index - lastIdx);
                        if (before.Length > 0)
                        {
                            AddSegmentsToLine(currentLine, before);
                            logSb.Append(before);
                        }

                        string paramVal = match.Groups[1].Value;
                        string command = match.Groups[2].Value;

                        if (command == "m")
                        {
                            UpdateAnsiStyles(paramVal);
                        }

                        lastIdx = match.Index + match.Length;
                    }

                    string remaining = part.Substring(lastIdx);
                    if (remaining.Length > 0)
                    {
                        AddSegmentsToLine(currentLine, remaining);
                        logSb.Append(remaining);
                    }
                }
                else
                {
                    if (part.Length > 0)
                    {
                        AddSegmentsToLine(currentLine, part);
                        logSb.Append(part);
                    }
                }

                linesResult.Add(currentLine);
                if (i < parts.Length - 1)
                {
                    logSb.Append("\n");
                }
            }

            cleanTextForLogs = logSb.ToString();
            return linesResult;
        }

        private void AddSegmentsToLine(List<TextSegment> line, string text)
        {
            if (string.IsNullOrEmpty(text)) return;

            // Split by \r to identify carriage returns
            var subParts = text.Split('\r');
            for (int i = 0; i < subParts.Length; i++)
            {
                if (i > 0)
                {
                    // Add carriage return marker segment
                    line.Add(new TextSegment { Text = "\r" });
                }
                if (subParts[i].Length > 0)
                {
                    line.Add(new TextSegment
                    {
                        Text = subParts[i],
                        FgColor = _activeFgColor,
                        BgColor = _activeBgColor,
                        IsBold = _activeBold
                    });
                }
            }
        }

        private void UpdateAnsiStyles(string paramsStr)
        {
            if (string.IsNullOrEmpty(paramsStr))
            {
                _activeFgColor = DefaultFgColor;
                _activeBgColor = null;
                _activeBold = false;
                return;
            }

            var parts = paramsStr.Split(';');
            foreach (var part in parts)
            {
                if (int.TryParse(part, out int param))
                {
                    if (param == 0)
                    {
                        _activeFgColor = DefaultFgColor;
                        _activeBgColor = null;
                        _activeBold = false;
                    }
                    else if (param == 1)
                    {
                        _activeBold = true;
                    }
                    else if (AnsiColorMap.TryGetValue(param, out var fg))
                    {
                        _activeFgColor = fg;
                    }
                    else if (AnsiBgMap.TryGetValue(param, out var bg))
                    {
                        _activeBgColor = bg;
                    }
                    else if (param == 39)
                    {
                        _activeFgColor = DefaultFgColor;
                    }
                    else if (param == 49)
                    {
                        _activeBgColor = null;
                    }
                }
            }
        }
    }
}
