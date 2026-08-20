using System;
using Microsoft.UI.Xaml;

namespace serial_console_dotnet.Controls
{
    public sealed partial class DeviceExplorerWindow : Window
    {
        public DeviceExplorerView Explorer => ExplorerView;

        public event Action<string>? ExecuteFileRequested;

        private int _lastNormalWidth = 960;
        private int _lastNormalHeight = 720;

        public DeviceExplorerWindow(Microsoft.UI.WindowId parentWindowId)
        {
            this.InitializeComponent();

            // Native TitleBar customization
            this.ExtendsContentIntoTitleBar = true;
            this.SetTitleBar(AppTitleBar);

            var appWindow = this.AppWindow;
            appWindow.Title = "Device File Explorer";

            // Style normal and close buttons
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

            // Retrieve display details for the monitor that hosts the parent window
            var displayArea = Microsoft.UI.Windowing.DisplayArea.GetFromWindowId(parentWindowId, Microsoft.UI.Windowing.DisplayAreaFallback.Primary);
            var workArea = displayArea.WorkArea;

            // Restore last window layout settings, cropped to screen size
            try
            {
                var settings = Services.SettingsService.Load();
                var layout = settings.ExplorerWindowLayout;

                _lastNormalWidth = Math.Min(layout.Width > 300 ? layout.Width : 960, workArea.Width);
                _lastNormalHeight = Math.Min(layout.Height > 300 ? layout.Height : 720, workArea.Height);

                int x = workArea.X + (workArea.Width - _lastNormalWidth) / 2;
                int y = workArea.Y + (workArea.Height - _lastNormalHeight) / 2;
                appWindow.MoveAndResize(new Windows.Graphics.RectInt32(x, y, _lastNormalWidth, _lastNormalHeight));

                if (layout.IsMaximized)
                {
                    if (appWindow.Presenter is Microsoft.UI.Windowing.OverlappedPresenter overlappedPresenter)
                    {
                        overlappedPresenter.Maximize();
                    }
                }
            }
            catch {}

            this.SizeChanged += DeviceExplorerWindow_SizeChanged;
            this.Closed += DeviceExplorerWindow_Closed;

            ExplorerView.ExplorerClosed += () =>
            {
                this.Close();
            };

            ExplorerView.ExecuteFileRequested += path =>
            {
                ExecuteFileRequested?.Invoke(path);
            };
        }

        private void DeviceExplorerWindow_SizeChanged(object sender, WindowSizeChangedEventArgs args)
        {
            var appWindow = this.AppWindow;
            if (appWindow.Presenter is Microsoft.UI.Windowing.OverlappedPresenter overlappedPresenter)
            {
                if (overlappedPresenter.State == Microsoft.UI.Windowing.OverlappedPresenterState.Maximized)
                {
                    return;
                }
            }
            _lastNormalWidth = (int)args.Size.Width;
            _lastNormalHeight = (int)args.Size.Height;
        }

        private void DeviceExplorerWindow_Closed(object sender, WindowEventArgs args)
        {
            // Save layout settings
            try
            {
                var settings = Services.SettingsService.Load();
                var layout = settings.ExplorerWindowLayout;
                layout.Width = _lastNormalWidth;
                layout.Height = _lastNormalHeight;

                if (this.AppWindow.Presenter is Microsoft.UI.Windowing.OverlappedPresenter overlappedPresenter)
                {
                    layout.IsMaximized = overlappedPresenter.State == Microsoft.UI.Windowing.OverlappedPresenterState.Maximized;
                }
                else
                {
                    layout.IsMaximized = false;
                }

                Services.SettingsService.Save(settings);
            }
            catch {}
        }
    }
}
