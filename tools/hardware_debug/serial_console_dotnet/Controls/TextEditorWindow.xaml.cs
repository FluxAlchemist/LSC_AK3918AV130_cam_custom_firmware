using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;

namespace serial_console_dotnet.Controls
{
    public sealed partial class TextEditorWindow : Window
    {
        private readonly string _filename;
        private readonly Func<string, Task<bool>>? _onSaveCallback;
        private ScrollViewer? _editorScrollViewer;

        private int _lastNormalWidth = 800;
        private int _lastNormalHeight = 600;

        public TextEditorWindow(Microsoft.UI.WindowId parentWindowId, string filename, string content, Func<string, Task<bool>>? onSaveCallback)
        {
            this.InitializeComponent();

            _filename = filename;
            _onSaveCallback = onSaveCallback;

            // Native TitleBar customization
            this.ExtendsContentIntoTitleBar = true;
            this.SetTitleBar(AppTitleBar);

            var appWindow = this.AppWindow;
            appWindow.Title = $"Text Editor - {filename}";
            WindowTitleTextBlock.Text = $"Text Editor - {filename}";

            // Style buttons
            if (Microsoft.UI.Windowing.AppWindowTitleBar.IsCustomizationSupported())
            {
                var titleBar = appWindow.TitleBar;
                titleBar.ExtendsContentIntoTitleBar = true;
                titleBar.ButtonBackgroundColor = Microsoft.UI.Colors.Transparent;
                titleBar.ButtonInactiveBackgroundColor = Microsoft.UI.Colors.Transparent;
                titleBar.ButtonHoverBackgroundColor = Microsoft.UI.ColorHelper.FromArgb(255, 42, 55, 89);
                titleBar.ButtonPressedBackgroundColor = Microsoft.UI.ColorHelper.FromArgb(255, 62, 78, 122);
                titleBar.ButtonForegroundColor = Microsoft.UI.Colors.White;
                titleBar.ButtonHoverForegroundColor = Microsoft.UI.Colors.White;
                titleBar.ButtonPressedForegroundColor = Microsoft.UI.Colors.White;
            }

            // Populate content and title bar
            EditTextBox.Text = content;
            UpdateLineNumbers();
            UpdateStats();

            // Set file type label from extension
            string ext = Path.GetExtension(filename).TrimStart('.').ToUpper();
            FileTypeText.Text = string.IsNullOrEmpty(ext) ? "Plain text" : $"{ext} File";

            // Center window on active monitor
            var displayArea = Microsoft.UI.Windowing.DisplayArea.GetFromWindowId(parentWindowId, Microsoft.UI.Windowing.DisplayAreaFallback.Primary);
            var workArea = displayArea.WorkArea;

            // Load last layout settings or default
            try
            {
                var settings = Services.SettingsService.Load();
                var layout = settings.ExplorerWindowLayout; // Re-use explorer size as baseline
                _lastNormalWidth = Math.Min(layout.Width > 300 ? layout.Width : 800, workArea.Width);
                _lastNormalHeight = Math.Min(layout.Height > 300 ? layout.Height : 600, workArea.Height);
            }
            catch
            {
                _lastNormalWidth = Math.Min(800, workArea.Width);
                _lastNormalHeight = Math.Min(600, workArea.Height);
            }

            int x = workArea.X + (workArea.Width - _lastNormalWidth) / 2;
            int y = workArea.Y + (workArea.Height - _lastNormalHeight) / 2;
            appWindow.MoveAndResize(new Windows.Graphics.RectInt32(x, y, _lastNormalWidth, _lastNormalHeight));
        }

        private void EditTextBox_Loaded(object sender, RoutedEventArgs e)
        {
            _editorScrollViewer = FindScrollViewer(EditTextBox);
            if (_editorScrollViewer != null)
            {
                _editorScrollViewer.ViewChanged += EditorScrollViewer_ViewChanged;
            }
        }

        private ScrollViewer? FindScrollViewer(DependencyObject parent)
        {
            if (parent is ScrollViewer sv) return sv;
            int childCount = VisualTreeHelper.GetChildrenCount(parent);
            for (int i = 0; i < childCount; i++)
            {
                var result = FindScrollViewer(VisualTreeHelper.GetChild(parent, i));
                if (result != null) return result;
            }
            return null;
        }

        private void EditorScrollViewer_ViewChanged(object? sender, ScrollViewerViewChangedEventArgs e)
        {
            if (_editorScrollViewer != null)
            {
                LineNumbersScrollViewer.ChangeView(null, _editorScrollViewer.VerticalOffset, null, true);
            }
        }

        private void EditTextBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            UpdateLineNumbers();
            UpdateStats();
        }

        private void EditTextBox_SelectionChanged(object sender, RoutedEventArgs e)
        {
            UpdateStats();
        }

        private int CountLines(string text)
        {
            if (string.IsNullOrEmpty(text)) return 1;
            int count = 1;
            for (int i = 0; i < text.Length; i++)
            {
                if (text[i] == '\r')
                {
                    count++;
                    if (i + 1 < text.Length && text[i + 1] == '\n')
                    {
                        i++;
                    }
                }
                else if (text[i] == '\n')
                {
                    count++;
                }
            }
            return count;
        }

        private void UpdateLineNumbers()
        {
            string text = EditTextBox.Text;
            int lineCount = CountLines(text);
            LineNumbersTextBlock.Text = string.Join("\n", Enumerable.Range(1, lineCount));
        }

        private void UpdateStats()
        {
            string text = EditTextBox.Text;
            int selectionStart = EditTextBox.SelectionStart;

            int line = 1;
            int col = 1;
            for (int i = 0; i < selectionStart && i < text.Length; i++)
            {
                if (text[i] == '\r')
                {
                    line++;
                    col = 1;
                    if (i + 1 < selectionStart && text[i + 1] == '\n')
                    {
                        i++;
                    }
                }
                else if (text[i] == '\n')
                {
                    line++;
                    col = 1;
                }
                else
                {
                    col++;
                }
            }

            CursorPositionText.Text = $"Ln {line}, Col {col}";
            CharCountText.Text = $"{text.Length:N0} characters";
        }

        private async void SaveBtn_Click(object sender, RoutedEventArgs e)
        {
            if (_onSaveCallback == null) return;

            SaveBtn.IsEnabled = false;
            try
            {
                bool success = await _onSaveCallback(EditTextBox.Text);
                if (success)
                {
                    // Show a temporary visual feedback if desired
                }
            }
            catch {}
            finally
            {
                SaveBtn.IsEnabled = true;
            }
        }

        private async void SaveAsBtn_Click(object sender, RoutedEventArgs e)
        {
            // Standard SaveAs dialog on PC
            var savePicker = new Windows.Storage.Pickers.FileSavePicker();
            
            // Get hwnd for picker
            var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
            WinRT.Interop.InitializeWithWindow.Initialize(savePicker, hwnd);

            savePicker.SuggestedStartLocation = Windows.Storage.Pickers.PickerLocationId.DocumentsLibrary;
            string ext = Path.GetExtension(_filename);
            if (string.IsNullOrEmpty(ext)) ext = ".txt";
            savePicker.FileTypeChoices.Add("File", new System.Collections.Generic.List<string>() { ext });
            savePicker.SuggestedFileName = _filename;

            var file = await savePicker.PickSaveFileAsync();
            if (file != null)
            {
                try
                {
                    await Windows.Storage.FileIO.WriteTextAsync(file, EditTextBox.Text);
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine($"Error saving local file: {ex.Message}");
                }
            }
        }

        private void FindBtn_Click(object sender, RoutedEventArgs e)
        {
            if (FindPanel.Visibility == Visibility.Visible)
            {
                FindPanel.Visibility = Visibility.Collapsed;
            }
            else
            {
                FindPanel.Visibility = Visibility.Visible;
                FindTextBox.Focus(FocusState.Programmatic);
                FindTextBox.SelectAll();
            }
        }

        private void CloseFind_Click(object sender, RoutedEventArgs e)
        {
            FindPanel.Visibility = Visibility.Collapsed;
            EditTextBox.Focus(FocusState.Programmatic);
        }

        private void FindNext_Click(object sender, RoutedEventArgs e)
        {
            FindNext();
        }

        private void FindPrev_Click(object sender, RoutedEventArgs e)
        {
            FindPrev();
        }

        private void FindTextBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == Windows.System.VirtualKey.Enter)
            {
                FindNext();
                e.Handled = true;
            }
        }

        private void FindNext()
        {
            string search = FindTextBox.Text;
            if (string.IsNullOrEmpty(search)) return;

            string text = EditTextBox.Text;
            int startIndex = EditTextBox.SelectionStart + EditTextBox.SelectionLength;
            if (startIndex >= text.Length) startIndex = 0;

            int index = text.IndexOf(search, startIndex, StringComparison.OrdinalIgnoreCase);
            if (index == -1)
            {
                // Wrap around
                index = text.IndexOf(search, 0, StringComparison.OrdinalIgnoreCase);
            }

            if (index != -1)
            {
                EditTextBox.Focus(FocusState.Programmatic);
                EditTextBox.Select(index, search.Length);
            }
        }

        private void FindPrev()
        {
            string search = FindTextBox.Text;
            if (string.IsNullOrEmpty(search)) return;

            string text = EditTextBox.Text;
            int startIndex = EditTextBox.SelectionStart - 1;
            if (startIndex < 0) startIndex = text.Length - 1;

            int index = text.LastIndexOf(search, startIndex, StringComparison.OrdinalIgnoreCase);
            if (index == -1)
            {
                // Wrap around
                index = text.LastIndexOf(search, text.Length - 1, StringComparison.OrdinalIgnoreCase);
            }

            if (index != -1)
            {
                EditTextBox.Focus(FocusState.Programmatic);
                EditTextBox.Select(index, search.Length);
            }
        }
    }
}
