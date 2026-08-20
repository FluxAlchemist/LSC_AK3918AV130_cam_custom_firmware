using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Text;
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Text;
using Windows.Storage;
using Windows.Storage.Pickers;
using Microsoft.UI;
using Windows.UI;
using serial_console_dotnet.Services;

namespace serial_console_dotnet.Controls
{
    // Per-line render range, derived from DocumentSelection for TextHighlighter consumption.
    public struct SelectionRange
    {
        public int Start { get; set; }
        public int Length { get; set; }

        public bool Equals(SelectionRange other)
        {
            return Start == other.Start && Length == other.Length;
        }
    }

    // A single position in the terminal's logical document: which line, and which char within it.
    public struct DocumentPosition : IComparable<DocumentPosition>, IEquatable<DocumentPosition>
    {
        public int Line;
        public int Char;

        public DocumentPosition(int line, int ch)
        {
            Line = line;
            Char = ch;
        }

        public int CompareTo(DocumentPosition other)
        {
            int cmp = Line.CompareTo(other.Line);
            return cmp != 0 ? cmp : Char.CompareTo(other.Char);
        }

        public bool Equals(DocumentPosition other) => Line == other.Line && Char == other.Char;
    }

    // The selection's source of truth: a fixed Anchor (where selection began) and a movable
    // Focus (the other end — follows drag or shift-click). Start/End are the normalized order.
    public struct DocumentSelection
    {
        public DocumentPosition Anchor;
        public DocumentPosition Focus;

        public DocumentPosition Start => Anchor.CompareTo(Focus) <= 0 ? Anchor : Focus;
        public DocumentPosition End => Anchor.CompareTo(Focus) <= 0 ? Focus : Anchor;
        public bool IsEmpty => Anchor.Equals(Focus);
    }

    public class TerminalLine : INotifyPropertyChanged
    {
        public string Timestamp { get; set; } = "";
        public bool ShowTimestamp { get; set; } = true;
        public List<TextSegment>? Segments { get; set; }
        public string Type { get; set; } = "";
        public string RawText { get; set; } = "";

        // Called when more data for this same logical line arrives in a later chunk (no '\n' seen
        // yet). Grows the existing visual line in place instead of the caller creating a new one,
        // so a message split across TCP reads doesn't render as multiple lines with duplicate timestamps.
        public void AppendSegments(List<TextSegment> newSegments)
        {
            Segments ??= new List<TextSegment>();
            foreach (var seg in newSegments)
            {
                if (seg.Text == "\r")
                {
                    Segments.Clear();
                }
                else
                {
                    Segments.Add(seg);
                }
            }
            OnPropertyChanged(nameof(Segments));
        }

        private SelectionRange? _selection;
        public SelectionRange? Selection
        {
            get => _selection;
            set
            {
                if (_selection.HasValue && value.HasValue && _selection.Value.Equals(value.Value)) return;
                if (!_selection.HasValue && !value.HasValue) return;

                _selection = value;
                OnPropertyChanged(nameof(Selection));
            }
        }

        public event PropertyChangedEventHandler? PropertyChanged;
        protected void OnPropertyChanged(string name)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }
    }

    public class TerminalLineControl : ContentControl
    {
        public static readonly DependencyProperty LineProperty =
            DependencyProperty.Register("Line", typeof(TerminalLine), typeof(TerminalLineControl), new PropertyMetadata(null, OnLineChanged));

        public TerminalLine Line
        {
            get => (TerminalLine)GetValue(LineProperty);
            set => SetValue(LineProperty, value);
        }

        private RichTextBlock? _richText;

        private static void OnLineChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            if (d is TerminalLineControl control)
            {
                if (e.OldValue is TerminalLine oldLine)
                {
                    control.DetachOldLine(oldLine);
                }
                if (e.NewValue is TerminalLine newLine)
                {
                    control.UpdateVisuals(newLine);
                }
            }
        }

        private void DetachOldLine(TerminalLine oldLine)
        {
            oldLine.PropertyChanged -= Line_PropertyChanged;
        }

        private void UpdateVisuals(TerminalLine line)
        {
            line.PropertyChanged += Line_PropertyChanged;

            _richText = new RichTextBlock
            {
                FontFamily = new FontFamily("Consolas"),
                FontSize = 13,
                Foreground = new SolidColorBrush(Colors.LightGray),
                IsTextSelectionEnabled = false,
                IsHitTestVisible = false,
                TextWrapping = TextWrapping.Wrap
            };
            this.Content = _richText;

            RebuildParagraph(line);
            UpdateSelectionHighlight();
        }

        // Rebuilds the paragraph in the existing RichTextBlock — used both for the initial render
        // and whenever a still-open line grows in place (Segments changed without swapping Line).
        private void RebuildParagraph(TerminalLine line)
        {
            if (_richText == null) return;
            _richText.Blocks.Clear();

            var paragraph = new Paragraph();

            // Add timestamp
            if (line.ShowTimestamp && !string.IsNullOrEmpty(line.Timestamp))
            {
                paragraph.Inlines.Add(new Run
                {
                    Text = line.Timestamp,
                    Foreground = new SolidColorBrush(Colors.Gray)
                });
            }

            // Add segments
            if (line.Segments != null)
            {
                foreach (var seg in line.Segments)
                {
                    var run = new Run
                    {
                        Text = seg.Text,
                        Foreground = new SolidColorBrush(seg.FgColor)
                    };
                    if (seg.IsBold) run.FontWeight = FontWeights.Bold;
                    paragraph.Inlines.Add(run);
                }
            }
            else if (!string.IsNullOrEmpty(line.RawText))
            {
                Color textColor = Colors.LightGray;
                if (line.Type == "info") textColor = ColorFromHex("#00D2FF");
                else if (line.Type == "error") textColor = ColorFromHex("#F43F5E");
                else if (line.Type == "out") textColor = ColorFromHex("#00D2FF");

                paragraph.Inlines.Add(new Run
                {
                    Text = line.RawText,
                    Foreground = new SolidColorBrush(textColor)
                });
            }

            _richText.Blocks.Add(paragraph);
        }

        private void Line_PropertyChanged(object? sender, PropertyChangedEventArgs e)
        {
            if (e.PropertyName == nameof(TerminalLine.Selection))
            {
                UpdateSelectionHighlight();
            }
            else if (e.PropertyName == nameof(TerminalLine.Segments) && Line != null)
            {
                RebuildParagraph(Line);
                UpdateSelectionHighlight();
            }
        }

        private void UpdateSelectionHighlight()
        {
            if (_richText == null || Line == null) return;
            _richText.TextHighlighters.Clear();

            if (Line.Selection.HasValue && Line.Selection.Value.Length > 0)
            {
                var highlighter = new TextHighlighter
                {
                    Background = new SolidColorBrush(ColorFromHex("#0078D7")), // Win32 native selection highlight color
                    Foreground = new SolidColorBrush(Colors.White)
                };

                highlighter.Ranges.Add(new TextRange 
                { 
                    StartIndex = Line.Selection.Value.Start, 
                    Length = Line.Selection.Value.Length 
                });
                _richText.TextHighlighters.Add(highlighter);
            }
        }

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
            return Colors.Gray;
        }
    }

    public sealed partial class TerminalView : UserControl
    {
        private bool _showTimestamps = true;
        private bool _autoScroll = true;
        private int _visualLineCount = 0;

        // Tracks the most recently added line that hasn't been terminated by a real '\n' yet, so a
        // logical line split across multiple TCP/serial reads keeps extending in place instead of
        // rendering as several separate lines with duplicate timestamps (the source of the excessive
        // blank-line-per-chunk look in raw device output).
        private TerminalLine? _openLine;

        private readonly ObservableCollection<TerminalLine> _linesCollection = new ObservableCollection<TerminalLine>();
        private ScrollViewer? _scrollViewer;

        private double _charWidth = 7.15; // default measured width of Consolas size 13
        private bool _isSelecting = false;
        private DocumentSelection? _selection;

        // Remembers the horizontal column Up/Down keyboard navigation is "aiming for" across
        // shorter lines, same as Notepad/Visual Studio — without this, arrowing down through a
        // blank line and back up would snap the column back to 0 instead of restoring it.
        // Reset to null on any horizontal move or mouse click so it's recomputed from scratch.
        private int? _desiredColumn = null;

        // The offset we last told the ScrollViewer to land on via our own ScrollToBottom() call.
        // Detachment is judged against this, NOT against the live ScrollableHeight — see the
        // comment in ScrollViewer_ViewChanged for why.
        private double _lastProgrammaticOffset = 0;

        public event Action<IReadOnlyList<StorageFile>>? FileDropped;

        public IntPtr ParentWindowHwnd { get; set; } = IntPtr.Zero;

        private string? _tempLogFilePath;
        private StreamWriter? _tempLogWriter;

        public TerminalView()
        {
            this.InitializeComponent();
            this.Unloaded += TerminalView_Unloaded;

            TerminalListView.ItemsSource = _linesCollection;
            TerminalListView.Loaded += TerminalListView_Loaded;

            TerminalListView.AddHandler(PointerPressedEvent, new PointerEventHandler(TerminalListView_PointerPressed), true);
            TerminalListView.AddHandler(PointerMovedEvent, new PointerEventHandler(TerminalListView_PointerMoved), true);
            TerminalListView.AddHandler(PointerReleasedEvent, new PointerEventHandler(TerminalListView_PointerReleased), true);
            TerminalListView.AddHandler(PointerWheelChangedEvent, new PointerEventHandler(TerminalListView_PointerWheelChanged), true);

            InitTempLogFile();
            MeasureCharacterWidth();
        }

        private void MeasureCharacterWidth()
        {
            try
            {
                var tb = new TextBlock { FontFamily = new FontFamily("Consolas"), FontSize = 13, Text = new string('W', 100) };
                tb.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
                if (tb.DesiredSize.Width > 0)
                {
                    _charWidth = tb.DesiredSize.Width / 100.0;
                }
            }
            catch
            {
                _charWidth = 7.15; // fallback
            }
        }

        private void TerminalListView_Loaded(object sender, RoutedEventArgs e)
        {
            FindScrollViewer();
        }

        private void FindScrollViewer()
        {
            if (_scrollViewer != null) return;
            _scrollViewer = FindVisualChild<ScrollViewer>(TerminalListView);
            if (_scrollViewer != null)
            {
                _scrollViewer.ViewChanged += ScrollViewer_ViewChanged;
            }
        }

        private void ScrollViewer_ViewChanged(object? sender, ScrollViewerViewChangedEventArgs e)
        {
            if (_scrollViewer == null) return;

            // Mid-animation/mid-manipulation samples report positions that aren't where the
            // scroll will actually settle (including during our own animated ScrollToBottom
            // calls), so reacting to them caused auto-scroll to spuriously detach. Only the
            // final, settled event reflects where the user (or we) actually landed.
            if (e.IsIntermediate) return;

            const double tolerance = 10.0;
            double offset = _scrollViewer.VerticalOffset;
            double scrollable = _scrollViewer.ScrollableHeight;
            bool isAtBottom = offset >= scrollable - tolerance;

            if (isAtBottom)
            {
                if (!_autoScroll)
                {
                    _autoScroll = true;
                    ScrollLockIndicator.Visibility = Visibility.Collapsed;
                }
                _lastProgrammaticOffset = offset;
            }
            else if (_autoScroll)
            {
                // Don't judge detachment against the live ScrollableHeight: under a burst of
                // fast-incoming lines, the extent can grow (raising ScrollableHeight) faster than
                // our own dispatched ScrollToBottom() calls catch up, so "offset >= scrollable -
                // tolerance" reads false for content WE haven't scrolled to yet, not content the
                // user scrolled away from — that was the "can't keep up, gets detached" bug.
                // Instead, only detach when the offset actually moved backward from where we last
                // parked it, i.e. the user (scrollbar/wheel/keys) actively pulled the view up.
                if (offset < _lastProgrammaticOffset - tolerance)
                {
                    _autoScroll = false;
                    ScrollLockIndicator.Visibility = Visibility.Visible;
                }
            }
        }

        private T? FindVisualChild<T>(DependencyObject obj) where T : DependencyObject
        {
            for (int i = 0; i < VisualTreeHelper.GetChildrenCount(obj); i++)
            {
                var child = VisualTreeHelper.GetChild(obj, i);
                if (child is T t) return t;
                var childOfChild = FindVisualChild<T>(child);
                if (childOfChild != null) return childOfChild;
            }
            return null;
        }

        private void InitTempLogFile()
        {
            try
            {
                CloseTempLog(deleteFile: true);

                string tempDir = Path.GetTempPath();
                string fileName = $"serial_session_{Guid.NewGuid():N}.log";
                _tempLogFilePath = Path.Combine(tempDir, fileName);

                _tempLogWriter = new StreamWriter(_tempLogFilePath, false, Encoding.UTF8)
                {
                    AutoFlush = true
                };
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to initialize temp log file: {ex.Message}");
            }
        }

        private void CloseTempLog(bool deleteFile = false)
        {
            try
            {
                if (_tempLogWriter != null)
                {
                    _tempLogWriter.Flush();
                    _tempLogWriter.Close();
                    _tempLogWriter.Dispose();
                    _tempLogWriter = null;
                }

                if (deleteFile && !string.IsNullOrEmpty(_tempLogFilePath) && File.Exists(_tempLogFilePath))
                {
                    File.Delete(_tempLogFilePath);
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error cleaning up temp log file: {ex.Message}");
            }
        }

        private void TerminalView_Unloaded(object sender, RoutedEventArgs e)
        {
            CloseTempLog(deleteFile: true);
        }

        public void SetTitle(string title)
        {
            TerminalTitleText.Text = title;
        }

        private void Reattach()
        {
            _autoScroll = true;
            ScrollLockIndicator.Visibility = Visibility.Collapsed;
            ScrollToBottom();
        }

        public void ToggleTimestamps(bool show)
        {
            _showTimestamps = show;
            ClearSelection();
            foreach (var line in _linesCollection)
            {
                line.ShowTimestamp = show;
            }

            // Force refresh of the items
            TerminalListView.ItemsSource = null;
            TerminalListView.ItemsSource = _linesCollection;

            ScrollToBottom();
        }

        public void AppendSpecialLine(string text, string type)
        {
            string ts = $"[{DateTime.Now.ToString("HH:mm:ss")}] ";
            _tempLogWriter?.WriteLine(ts + text);

            var terminalLine = new TerminalLine
            {
                Timestamp = ts,
                ShowTimestamp = _showTimestamps,
                RawText = text,
                Type = type
            };

            _linesCollection.Add(terminalLine);
            _visualLineCount++;

            // A special/info line always lands after whatever the device stream last wrote. If that
            // last line was still open (no '\n' seen yet), seal it off so a later continuation of it
            // doesn't get appended above this line, out of visual order.
            _openLine = null;

            LineCountTextBlock.Text = $"{_visualLineCount} lines logged";

            PruneTerminalBlocks();
            ScrollToBottom();
        }

        public void AppendText(List<List<TextSegment>> lines, string logsSegment)
        {
            _tempLogWriter?.Write(logsSegment);

            for (int i = 0; i < lines.Count; i++)
            {
                var segments = lines[i];
                bool isFirst = i == 0;
                bool hasMoreParts = lines.Count > 1;

                if (isFirst)
                {
                    // The first part always continues whatever came right before it in the stream.
                    if (_openLine != null)
                    {
                        _openLine.AppendSegments(segments);
                    }
                    else
                    {
                        StartNewLine(segments, keepOpen: true);
                    }

                    // If more parts follow in this chunk, a '\n' terminated this one — seal it.
                    if (hasMoreParts) _openLine = null;
                    continue;
                }

                bool isLast = i == lines.Count - 1;
                // Every part after the first always starts right after a '\n' seen earlier in this
                // chunk, so it's always a brand-new line. Only the last one stays open for the next read.
                StartNewLine(segments, keepOpen: isLast);
            }

            LineCountTextBlock.Text = $"{_visualLineCount} lines logged";

            PruneTerminalBlocks();
            ScrollToBottom();
        }

        private void StartNewLine(List<TextSegment> segments, bool keepOpen)
        {
            var processed = new List<TextSegment>();
            foreach (var seg in segments)
            {
                if (seg.Text == "\r")
                {
                    processed.Clear();
                }
                else
                {
                    processed.Add(seg);
                }
            }

            string ts = $"[{DateTime.Now.ToString("HH:mm:ss")}] ";
            var terminalLine = new TerminalLine
            {
                Timestamp = ts,
                ShowTimestamp = _showTimestamps,
                Segments = processed
            };

            _linesCollection.Add(terminalLine);
            _visualLineCount++;
            _openLine = keepOpen ? terminalLine : null;
        }

        private void PruneTerminalBlocks()
        {
            if (_linesCollection.Count <= 10000) return;

            int toRemove = _linesCollection.Count - 8000;
            for (int i = 0; i < toRemove; i++)
            {
                _linesCollection.RemoveAt(0);
            }

            _visualLineCount = _linesCollection.Count;
            LineCountTextBlock.Text = $"{_visualLineCount} lines logged";
        }

        private void ScrollToBottom()
        {
            if (!_autoScroll) return;

            DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
            {
                if (_linesCollection.Count == 0) return;

                // ScrollIntoView only scrolls the minimal amount needed to bring an item into the
                // viewport, which for variable-height wrapped RichTextBlock rows often stops short
                // of the true bottom edge — the gap then reads as "detached" on the next ViewChanged.
                // ChangeView with an offset past the real max gets clamped to the exact bottom, and
                // disableAnimation avoids emitting intermediate ViewChanged samples for the jump.
                if (_scrollViewer != null)
                {
                    double target = Math.Min(_scrollViewer.ExtentHeight, _scrollViewer.ScrollableHeight);
                    _lastProgrammaticOffset = target;
                    _scrollViewer.ChangeView(null, _scrollViewer.ExtentHeight, null, true);
                }
                else
                {
                    TerminalListView.ScrollIntoView(_linesCollection[_linesCollection.Count - 1]);
                }
            });
        }

        public void Clear()
        {
            // Clearing is a deliberate reset — always land back at (an empty) bottom, even if the
            // user had scrolled up and locked auto-scroll beforehand.
            _autoScroll = true;
            _lastProgrammaticOffset = 0;
            ScrollLockIndicator.Visibility = Visibility.Collapsed;

            _selection = null;
            _openLine = null;
            _linesCollection.Clear();
            _visualLineCount = 0;
            LineCountTextBlock.Text = "0 lines logged";

            InitTempLogFile();

            AppendSpecialLine("--- Terminal cleared ---", "info");
        }

        public async void ExportLog()
        {
            if (string.IsNullOrEmpty(_tempLogFilePath) || !File.Exists(_tempLogFilePath))
            {
                AppendSpecialLine("Export failed: No session log recorded.", "error");
                return;
            }

            var picker = new FileSavePicker();
            picker.SuggestedStartLocation = PickerLocationId.ComputerFolder;
            picker.SuggestedFileName = $"serial-console-log-{DateTime.Now:yyyy-MM-dd_HH-mm-ss}.txt";
            picker.FileTypeChoices.Add("Plain Text", new List<string> { ".txt" });

            if (ParentWindowHwnd != IntPtr.Zero)
            {
                WinRT.Interop.InitializeWithWindow.Initialize(picker, ParentWindowHwnd);
            }

            StorageFile file = await picker.PickSaveFileAsync();
            if (file != null)
            {
                try
                {
                    _tempLogWriter?.Flush();
                    File.Copy(_tempLogFilePath, file.Path, overwrite: true);
                    AppendSpecialLine($"--- Log successfully exported to {file.Name} ---", "info");
                }
                catch (Exception ex)
                {
                    AppendSpecialLine($"Export failed: {ex.Message}", "error");
                }
            }
        }

        private void ReleaseScrollLock_Click(object sender, RoutedEventArgs e)
        {
            Reattach();
        }

        private void Terminal_DragOver(object sender, DragEventArgs e)
        {
            e.AcceptedOperation = Windows.ApplicationModel.DataTransfer.DataPackageOperation.Copy;
        }

        private async void Terminal_Drop(object sender, DragEventArgs e)
        {
            if (e.DataView.Contains(Windows.ApplicationModel.DataTransfer.StandardDataFormats.StorageItems))
            {
                var items = await e.DataView.GetStorageItemsAsync();
                // Folders aren't supported by the base64-over-UART upload path, so only
                // pick out the dropped files — ignore any folders in the mix.
                var files = items.OfType<StorageFile>().ToList();
                if (files.Count > 0)
                {
                    FileDropped?.Invoke(files);
                }
            }
        }

        // ================= SELECTION & DRAG GESTURES =================

        private void TerminalListView_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            TerminalListView.Focus(FocusState.Pointer);

            var hostPoint = e.GetCurrentPoint(null);

            if (hostPoint.Properties.IsLeftButtonPressed)
            {
                var elements = VisualTreeHelper.FindElementsInHostCoordinates(hostPoint.Position, TerminalListView);
                var item = FindParentListViewItem(elements);
                if (item != null)
                {
                    int lineIndex = TerminalListView.IndexFromContainer(item);
                    if (lineIndex != -1)
                    {
                        var lineControl = FindVisualChild<TerminalLineControl>(item);
                        UIElement relativeTo = lineControl != null ? (UIElement)lineControl : (UIElement)item;
                        var relativePoint = e.GetCurrentPoint(relativeTo);
                        var line = _linesCollection[lineIndex];
                        int charIndex = GetCharIndexAtPosition(item, line, relativePoint.Position);
                        var clickPosition = new DocumentPosition(lineIndex, charIndex);

                        _isSelecting = true;
                        _desiredColumn = null;

                        if (IsShiftPressed() && _selection.HasValue)
                        {
                            // Shift-click: extend from the existing anchor to the click point,
                            // same as standard Windows text selection.
                            _selection = new DocumentSelection { Anchor = _selection.Value.Anchor, Focus = clickPosition };
                        }
                        else
                        {
                            // Plain click: start a fresh selection anchored at the click point.
                            _selection = new DocumentSelection { Anchor = clickPosition, Focus = clickPosition };
                        }

                        ApplySelectionToLines();
                        TerminalListView.CapturePointer(e.Pointer);
                        e.Handled = true;
                    }
                }
            }
        }

        private void TerminalListView_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (_isSelecting && _selection.HasValue)
            {
                var hostPoint = e.GetCurrentPoint(null);
                var elements = VisualTreeHelper.FindElementsInHostCoordinates(hostPoint.Position, TerminalListView);
                var item = FindParentListViewItem(elements);
                if (item != null)
                {
                    int lineIndex = TerminalListView.IndexFromContainer(item);
                    if (lineIndex != -1)
                    {
                        var lineControl = FindVisualChild<TerminalLineControl>(item);
                        UIElement relativeTo = lineControl != null ? (UIElement)lineControl : (UIElement)item;
                        var relativePoint = e.GetCurrentPoint(relativeTo);
                        var line = _linesCollection[lineIndex];
                        int currentCharIndex = GetCharIndexAtPosition(item, line, relativePoint.Position);

                        // Drag only ever moves the Focus end — the Anchor stays put.
                        var sel = _selection.Value;
                        sel.Focus = new DocumentPosition(lineIndex, currentCharIndex);
                        _selection = sel;

                        ApplySelectionToLines();
                    }
                }
                e.Handled = true;
            }
        }

        private void TerminalListView_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            if (_isSelecting)
            {
                TerminalListView.ReleasePointerCapture(e.Pointer);
                _isSelecting = false;
                e.Handled = true;
            }
        }

        // While a click-and-drag selection has captured the pointer, PointerCapture reroutes the
        // wheel event's source to TerminalListView itself instead of the point under the cursor,
        // so it never reaches the internal ScrollViewer nested below — the wheel silently does
        // nothing. Drive the scroll manually in that case so wheel + drag-select can be used together.
        private void TerminalListView_PointerWheelChanged(object sender, PointerRoutedEventArgs e)
        {
            if (!_isSelecting || _scrollViewer == null) return;

            var point = e.GetCurrentPoint(TerminalListView);
            int delta = point.Properties.MouseWheelDelta;
            double scrollAmount = -(delta / 120.0) * 48.0;

            double newOffset = _scrollViewer.VerticalOffset + scrollAmount;
            newOffset = Math.Max(0, Math.Min(newOffset, _scrollViewer.ScrollableHeight));
            _scrollViewer.ChangeView(null, newOffset, null, true);

            e.Handled = true;
        }

        private ListViewItem? FindParentListViewItem(IEnumerable<DependencyObject> elements)
        {
            foreach (var element in elements)
            {
                var parent = element;
                while (parent != null && parent != TerminalListView)
                {
                    if (parent is ListViewItem item)
                    {
                        return item;
                    }
                    parent = VisualTreeHelper.GetParent(parent);
                }
            }
            return null;
        }

        private int GetCharIndexAtPosition(ListViewItem item, TerminalLine line, Windows.Foundation.Point position)
        {
            double horizontalOffset = position.X - 4.0;
            if (horizontalOffset < 0) horizontalOffset = 0;

            int rawIndex = (int)(horizontalOffset / _charWidth);
            if (rawIndex < 0) rawIndex = 0;

            int totalLength = GetLineTextLength(line);
            if (rawIndex > totalLength) rawIndex = totalLength;
            
            return rawIndex;
        }

        private int GetLineTextLength(TerminalLine line)
        {
            return GetLineText(line).Length;
        }

        private string GetLineText(TerminalLine line)
        {
            string prefix = (line.ShowTimestamp && !string.IsNullOrEmpty(line.Timestamp)) ? line.Timestamp : "";
            if (line.Segments != null)
            {
                var sb = new StringBuilder();
                sb.Append(prefix);
                foreach (var seg in line.Segments) sb.Append(seg.Text);
                return sb.ToString();
            }
            return prefix + line.RawText;
        }

        // Recomputes each line's per-line render range (SelectionRange) from the single
        // document-wide _selection (start line:char -> end line:char). This is the only
        // place that reads _selection to drive visuals; everything else just mutates it.
        private void ApplySelectionToLines()
        {
            foreach (var line in _linesCollection)
            {
                line.Selection = null;
            }

            if (!_selection.HasValue || _selection.Value.IsEmpty) return;

            var start = _selection.Value.Start;
            var end = _selection.Value.End;

            int minLine = Math.Max(0, start.Line);
            int maxLine = Math.Min(_linesCollection.Count - 1, end.Line);

            for (int i = minLine; i <= maxLine; i++)
            {
                var line = _linesCollection[i];
                int totalLength = GetLineTextLength(line);

                int s, len;
                if (start.Line == end.Line)
                {
                    s = Math.Min(start.Char, totalLength);
                    int e = Math.Min(end.Char, totalLength);
                    len = e - s;
                }
                else if (i == start.Line)
                {
                    s = Math.Min(start.Char, totalLength);
                    len = totalLength - s;
                }
                else if (i == end.Line)
                {
                    s = 0;
                    len = Math.Min(end.Char, totalLength);
                }
                else
                {
                    s = 0;
                    len = totalLength;
                }

                if (len > 0)
                {
                    line.Selection = new SelectionRange { Start = s, Length = len };
                }
            }
        }

        private void ClearSelection()
        {
            _selection = null;
            foreach (var line in _linesCollection)
            {
                line.Selection = null;
            }
        }

        private void CopyMenu_Click(object sender, RoutedEventArgs e)
        {
            CopySelectedLinesToClipboard();
        }

        private void ClearMenu_Click(object sender, RoutedEventArgs e)
        {
            Clear();
        }

        private void TerminalListView_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
        {
            var hostPoint = e.GetPosition(null);
            var elements = VisualTreeHelper.FindElementsInHostCoordinates(hostPoint, TerminalListView);
            var item = FindParentListViewItem(elements);
            if (item != null)
            {
                int lineIndex = TerminalListView.IndexFromContainer(item);
                if (lineIndex != -1)
                {
                    var lineControl = FindVisualChild<TerminalLineControl>(item);
                    UIElement relativeTo = lineControl != null ? (UIElement)lineControl : (UIElement)item;
                    var relativePoint = e.GetPosition(relativeTo);
                    var line = _linesCollection[lineIndex];

                    int charIndex = GetCharIndexAtPosition(item, line, relativePoint);

                    string lineText = GetLineText(line);
                    var word = FindWordAtCharIndex(lineText, charIndex);

                    // Anchor at the word's start, focus at its end — so a later shift-click
                    // extends from the start of the double-clicked word, matching Windows.
                    _selection = new DocumentSelection
                    {
                        Anchor = new DocumentPosition(lineIndex, word.Start),
                        Focus = new DocumentPosition(lineIndex, word.Start + word.Length)
                    };
                    ApplySelectionToLines();
                }
            }
        }

        private SelectionRange FindWordAtCharIndex(string text, int index)
        {
            if (string.IsNullOrEmpty(text) || index < 0 || index >= text.Length)
            {
                return new SelectionRange { Start = 0, Length = 0 };
            }

            char startChar = text[index];
            bool isWordChar = char.IsLetterOrDigit(startChar) || startChar == '_';

            int start = index;
            while (start > 0)
            {
                char c = text[start - 1];
                bool isPrevWordChar = char.IsLetterOrDigit(c) || c == '_';
                if (isPrevWordChar != isWordChar) break;
                start--;
            }

            int end = index;
            while (end < text.Length - 1)
            {
                char c = text[end + 1];
                bool isNextWordChar = char.IsLetterOrDigit(c) || c == '_';
                if (isNextWordChar != isWordChar) break;
                end++;
            }

            return new SelectionRange { Start = start, Length = (end - start) + 1 };
        }

        private void TerminalListView_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            bool ctrl = IsCtrlPressed();
            bool shift = IsShiftPressed();

            if (e.Key == Windows.System.VirtualKey.C && ctrl)
            {
                CopySelectedLinesToClipboard();
                e.Handled = true;
                return;
            }

            if (e.Key == Windows.System.VirtualKey.A && ctrl)
            {
                SelectAll();
                e.Handled = true;
                return;
            }

            switch (e.Key)
            {
                case Windows.System.VirtualKey.Left:
                    MoveCaretHorizontal(-1, extend: shift, wordJump: ctrl);
                    e.Handled = true;
                    break;
                case Windows.System.VirtualKey.Right:
                    MoveCaretHorizontal(1, extend: shift, wordJump: ctrl);
                    e.Handled = true;
                    break;
                case Windows.System.VirtualKey.Up:
                    MoveCaretVertical(-1, extend: shift);
                    e.Handled = true;
                    break;
                case Windows.System.VirtualKey.Down:
                    MoveCaretVertical(1, extend: shift);
                    e.Handled = true;
                    break;
                case Windows.System.VirtualKey.Home:
                    MoveCaretToLineEdge(toStart: true, extend: shift, wholeDocument: ctrl);
                    e.Handled = true;
                    break;
                case Windows.System.VirtualKey.End:
                    MoveCaretToLineEdge(toStart: false, extend: shift, wholeDocument: ctrl);
                    e.Handled = true;
                    break;
                case Windows.System.VirtualKey.PageUp:
                    MoveCaretPage(-1, extend: shift);
                    e.Handled = true;
                    break;
                case Windows.System.VirtualKey.PageDown:
                    MoveCaretPage(1, extend: shift);
                    e.Handled = true;
                    break;
            }
        }

        // ================= KEYBOARD SELECTION =================
        //
        // There's no rendered caret in this read-only view, so we track a virtual one: when a
        // selection exists, the caret is its Focus end; otherwise it defaults to the very end of
        // the buffer (where a freshly-opened terminal's attention naturally is). All of the
        // Notepad/Visual-Studio-style rules below build on that: extend (Shift held) moves Focus
        // and keeps Anchor fixed; a bare arrow key collapses an existing selection to whichever
        // edge is in the direction of travel (standard Windows text-field behavior) rather than
        // moving further from wherever Focus happened to be.

        private DocumentPosition GetCaret()
        {
            if (_selection.HasValue) return _selection.Value.Focus;
            if (_linesCollection.Count == 0) return new DocumentPosition(0, 0);
            int lastLine = _linesCollection.Count - 1;
            return new DocumentPosition(lastLine, GetLineTextLength(_linesCollection[lastLine]));
        }

        private DocumentPosition GetAnchor() => _selection.HasValue ? _selection.Value.Anchor : GetCaret();

        private void SetSelection(DocumentPosition anchor, DocumentPosition focus)
        {
            _selection = new DocumentSelection { Anchor = anchor, Focus = focus };
            ApplySelectionToLines();
        }

        private void ScrollLineIntoView(int lineIndex)
        {
            if (lineIndex < 0 || lineIndex >= _linesCollection.Count) return;
            TerminalListView.ScrollIntoView(_linesCollection[lineIndex]);
        }

        private void SelectAll()
        {
            if (_linesCollection.Count == 0) return;
            int lastLine = _linesCollection.Count - 1;
            var start = new DocumentPosition(0, 0);
            var end = new DocumentPosition(lastLine, GetLineTextLength(_linesCollection[lastLine]));
            SetSelection(start, end);
            ScrollLineIntoView(end.Line);
        }

        private void MoveCaretHorizontal(int direction, bool extend, bool wordJump)
        {
            if (_linesCollection.Count == 0) return;
            _desiredColumn = null;

            if (!extend && _selection.HasValue && !_selection.Value.IsEmpty)
            {
                var sel = _selection.Value;
                var collapseTo = direction < 0 ? sel.Start : sel.End;
                SetSelection(collapseTo, collapseTo);
                ScrollLineIntoView(collapseTo.Line);
                return;
            }

            var caret = GetCaret();
            var anchor = extend ? GetAnchor() : caret;
            var newCaret = wordJump ? MoveWordPosition(caret, direction) : MoveCharacterPosition(caret, direction);

            SetSelection(extend ? anchor : newCaret, newCaret);
            ScrollLineIntoView(newCaret.Line);
        }

        private void MoveCaretVertical(int lineDelta, bool extend)
        {
            if (_linesCollection.Count == 0) return;

            var caret = GetCaret();
            if (!extend && _selection.HasValue && !_selection.Value.IsEmpty)
            {
                var sel = _selection.Value;
                caret = lineDelta < 0 ? sel.Start : sel.End;
            }
            var anchor = extend ? GetAnchor() : caret;

            int col = _desiredColumn ?? caret.Char;
            int newLine = Math.Clamp(caret.Line + lineDelta, 0, _linesCollection.Count - 1);
            int newLineLength = GetLineTextLength(_linesCollection[newLine]);
            var newCaret = new DocumentPosition(newLine, Math.Min(col, newLineLength));
            _desiredColumn = col;

            SetSelection(extend ? anchor : newCaret, newCaret);
            ScrollLineIntoView(newCaret.Line);
        }

        private void MoveCaretToLineEdge(bool toStart, bool extend, bool wholeDocument)
        {
            if (_linesCollection.Count == 0) return;
            _desiredColumn = null;

            var caret = GetCaret();
            var anchor = extend ? GetAnchor() : caret;

            DocumentPosition newCaret;
            if (wholeDocument)
            {
                int lastLine = _linesCollection.Count - 1;
                newCaret = toStart
                    ? new DocumentPosition(0, 0)
                    : new DocumentPosition(lastLine, GetLineTextLength(_linesCollection[lastLine]));
            }
            else
            {
                newCaret = toStart
                    ? new DocumentPosition(caret.Line, 0)
                    : new DocumentPosition(caret.Line, GetLineTextLength(_linesCollection[caret.Line]));
            }

            SetSelection(extend ? anchor : newCaret, newCaret);
            ScrollLineIntoView(newCaret.Line);
        }

        private void MoveCaretPage(int direction, bool extend)
        {
            if (_scrollViewer == null) { MoveCaretVertical(direction * 10, extend); return; }

            // Lines wrap to variable visual heights, so this can't be pixel-exact — approximate
            // using the ListViewItem's MinHeight (18px, see TerminalView.xaml) plus a small margin,
            // which is the same approximation every "page" concept in this view already accepts.
            const double approxLineHeight = 20.0;
            int pageLines = Math.Max(1, (int)(_scrollViewer.ViewportHeight / approxLineHeight));
            MoveCaretVertical(direction * pageLines, extend);
        }

        private DocumentPosition MoveCharacterPosition(DocumentPosition pos, int direction)
        {
            if (direction < 0)
            {
                if (pos.Char > 0) return new DocumentPosition(pos.Line, pos.Char - 1);
                if (pos.Line > 0)
                {
                    int prevLine = pos.Line - 1;
                    return new DocumentPosition(prevLine, GetLineTextLength(_linesCollection[prevLine]));
                }
                return pos;
            }
            else
            {
                int lineLength = GetLineTextLength(_linesCollection[pos.Line]);
                if (pos.Char < lineLength) return new DocumentPosition(pos.Line, pos.Char + 1);
                if (pos.Line < _linesCollection.Count - 1) return new DocumentPosition(pos.Line + 1, 0);
                return pos;
            }
        }

        // Ctrl+Left/Right word jump. Only jumps within the current line — at a line boundary it
        // falls back to a single-character move onto the adjacent line, which is a reasonable
        // "at least go somewhere sensible" default rather than trying to guess a word across lines.
        private DocumentPosition MoveWordPosition(DocumentPosition pos, int direction)
        {
            string text = GetLineText(_linesCollection[pos.Line]);

            if (direction < 0)
            {
                int i = pos.Char;
                if (i == 0) return MoveCharacterPosition(pos, -1);

                while (i > 0 && char.IsWhiteSpace(text[i - 1])) i--;
                if (i == 0) return new DocumentPosition(pos.Line, 0);

                bool isWord = char.IsLetterOrDigit(text[i - 1]) || text[i - 1] == '_';
                while (i > 0)
                {
                    char c = text[i - 1];
                    bool w = char.IsLetterOrDigit(c) || c == '_';
                    if (w != isWord) break;
                    i--;
                }
                return new DocumentPosition(pos.Line, i);
            }
            else
            {
                int len = text.Length;
                int i = pos.Char;
                if (i >= len) return MoveCharacterPosition(pos, 1);

                bool isWord = char.IsLetterOrDigit(text[i]) || text[i] == '_';
                while (i < len)
                {
                    char c = text[i];
                    bool w = char.IsLetterOrDigit(c) || c == '_';
                    if (w != isWord) break;
                    i++;
                }
                while (i < len && char.IsWhiteSpace(text[i])) i++;
                return new DocumentPosition(pos.Line, i);
            }
        }

        private bool IsCtrlPressed()
        {
            var ctrlState = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(Windows.System.VirtualKey.Control);
            return (ctrlState & Windows.UI.Core.CoreVirtualKeyStates.Down) == Windows.UI.Core.CoreVirtualKeyStates.Down;
        }

		private bool IsShiftPressed()
		{
			var shiftState = Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(Windows.System.VirtualKey.Shift);
			return (shiftState & Windows.UI.Core.CoreVirtualKeyStates.Down) == Windows.UI.Core.CoreVirtualKeyStates.Down;
		}

		private void CopySelectedLinesToClipboard()
        {
            var sb = new StringBuilder();
            bool hasSelection = false;

            foreach (var line in _linesCollection)
            {
                if (line.Selection.HasValue)
                {
                    hasSelection = true;
                    string lineText = GetLineText(line);
                    int start = line.Selection.Value.Start;
                    int length = line.Selection.Value.Length;

                    if (start < 0) start = 0;
                    if (start + length > lineText.Length) length = lineText.Length - start;

                    if (length > 0)
                    {
                        sb.AppendLine(lineText.Substring(start, length));
                    }
                    else if (lineText.Length == 0)
                    {
                        sb.AppendLine();
                    }
                }
            }

            if (hasSelection && sb.Length > 0)
            {
                var dataPackage = new Windows.ApplicationModel.DataTransfer.DataPackage();
                dataPackage.SetText(sb.ToString().TrimEnd('\r', '\n'));
                Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(dataPackage);
            }
        }
    }
}
