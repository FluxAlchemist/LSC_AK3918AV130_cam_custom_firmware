using System;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;

namespace serial_console_dotnet.Controls
{
    public sealed partial class AddressBar : UserControl
    {
        private string _currentPath = "/";

        public event Action? BackClicked;
        public event Action? ForwardClicked;
        public event Action? UpClicked;
        public event Action? RefreshClicked;
        public event Action<string>? PathSubmitted;
        public event Action<string>? ViewModeChanged;

        public AddressBar()
        {
            this.InitializeComponent();
        }

        public void SetCurrentPath(string path)
        {
            _currentPath = path;
            UpdateBreadcrumbs();
        }

        public void SetNavigationState(bool canGoBack, bool canGoForward)
        {
            BackButton.IsEnabled = canGoBack;
            ForwardButton.IsEnabled = canGoForward;
        }

        public void SetEnabled(bool isEnabled)
        {
            BackButton.IsEnabled = isEnabled;
            ForwardButton.IsEnabled = isEnabled;
            UpButton.IsEnabled = isEnabled;
            RefreshButton.IsEnabled = isEnabled;
            AddressBarGrid.IsHitTestVisible = isEnabled;
            ListViewBtn.IsEnabled = isEnabled;
            IconViewBtn.IsEnabled = isEnabled;
        }

        private void UpdateBreadcrumbs()
        {
            BreadcrumbPanel.Children.Clear();

            // Always add Root /
            var rootBtn = new Button
            {
                Content = "/",
                Style = (Style)Application.Current.Resources["OutlineButtonStyle"],
                Padding = new Thickness(6, 4, 6, 4)
            };
            rootBtn.Click += (s, e) => { PathSubmitted?.Invoke("/"); };
            BreadcrumbPanel.Children.Add(rootBtn);

            string[] segments = _currentPath.Split(new[] { '/' }, StringSplitOptions.RemoveEmptyEntries);
            string builtPath = "";
            foreach (var seg in segments)
            {
                var arrow = new TextBlock
                {
                    Text = ">",
                    Foreground = (SolidColorBrush)Application.Current.Resources["TextMutedBrush"],
                    VerticalAlignment = VerticalAlignment.Center,
                    Margin = new Thickness(2, 0, 2, 0)
                };
                BreadcrumbPanel.Children.Add(arrow);

                builtPath += "/" + seg;
                string destPath = builtPath;

                var segBtn = new Button
                {
                    Content = seg,
                    Style = (Style)Application.Current.Resources["OutlineButtonStyle"],
                    Padding = new Thickness(6, 4, 6, 4)
                };
                segBtn.Click += (s, e) => { PathSubmitted?.Invoke(destPath); };
                BreadcrumbPanel.Children.Add(segBtn);
            }
        }

        private void BackButton_Click(object sender, RoutedEventArgs e)
        {
            BackClicked?.Invoke();
        }

        private void ForwardButton_Click(object sender, RoutedEventArgs e)
        {
            ForwardClicked?.Invoke();
        }

        private void UpButton_Click(object sender, RoutedEventArgs e)
        {
            UpClicked?.Invoke();
        }

        private void RefreshButton_Click(object sender, RoutedEventArgs e)
        {
            RefreshClicked?.Invoke();
        }

        private void AddressBar_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            var source = e.OriginalSource as DependencyObject;
            while (source != null)
            {
                if (source is Button)
                {
                    return;
                }
                source = VisualTreeHelper.GetParent(source);
            }

            BreadcrumbsScrollViewer.Visibility = Visibility.Collapsed;
            AddressTextBox.Visibility = Visibility.Visible;
            AddressTextBox.Text = _currentPath;
            AddressTextBox.Focus(FocusState.Programmatic);
            AddressTextBox.SelectAll();
        }

        private void AddressTextBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == Windows.System.VirtualKey.Enter)
            {
                string target = AddressTextBox.Text.Trim();
                ExitAddressEditMode();
                if (!string.IsNullOrEmpty(target))
                {
                    PathSubmitted?.Invoke(target);
                }
                e.Handled = true;
            }
            else if (e.Key == Windows.System.VirtualKey.Escape)
            {
                ExitAddressEditMode();
                e.Handled = true;
            }
        }

        private void AddressTextBox_LostFocus(object sender, RoutedEventArgs e)
        {
            ExitAddressEditMode();
        }

        private void ExitAddressEditMode()
        {
            AddressTextBox.Visibility = Visibility.Collapsed;
            BreadcrumbsScrollViewer.Visibility = Visibility.Visible;
        }

        private void ViewModeBtn_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button btn && btn.Tag is string mode)
            {
                ViewModeChanged?.Invoke(mode);
            }
        }
    }
}
