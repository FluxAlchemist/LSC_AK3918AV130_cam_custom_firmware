using System;
using System.ComponentModel;
using System.Collections.Generic;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using LibVLCSharp.Shared;
using LibVLCSharp.Platforms.Windows;
using serial_console_dotnet.Controls;
using serial_console_dotnet.ViewModels;

namespace serial_console_dotnet
{
    public sealed partial class MainWindow : Window
    {
        public CameraTuningViewModel TuningViewModel { get; }

        private readonly IntPtr _hwnd;
        private int _terminalTabCounter = 0;
        private int _lastNormalWidth = 1280;
        private int _lastNormalHeight = 800;

        // Every open terminal tab, keyed by its TabViewItem, so TabCloseRequested/window-Closed
        // can find the matching TerminalTabView to clean up (disconnect, dispose background
        // threads) — each tab owns a fully independent connection (see TerminalTabView).
        private readonly Dictionary<TabViewItem, TerminalTabView> _terminalTabs = new();

        // Camera Tuning tab's embedded RTSP preview (LibVLCSharp.WinUI). One
        // LibVLC/MediaPlayer instance for the app's lifetime; Play()/Stop()
        // tracks TuningViewModel's connection state.
        private LibVLC? _libVLC;
        private MediaPlayer? _mediaPlayer;

        // Camera Tuning tab's debug log reuses TerminalView (same control as
        // the main terminal) fed through its own AnsiParserService instance —
        // AnsiParserService keeps mutable per-stream parse state, so it must
        // NOT be shared with any terminal tab's own parser instance.
        private readonly Services.AnsiParserService _tuningLogAnsiParser = new();

        public MainWindow()
        {
            // Constructed BEFORE InitializeComponent(): the Camera Tuning tab's
            // sliders/toggle are TwoWay-bound to TuningViewModel properties, and
            // WinUI applies each control's initial value (e.g. clamping
            // AeExpMaxSlider's Value to its Minimum="2") synchronously while
            // InitializeComponent() builds the tree. That fires ValueChanged/
            // Toggled immediately, and our handlers dereference TuningViewModel —
            // if it isn't constructed yet, that's a NullReferenceException.
            // DispatcherQueue.GetForCurrentThread() (used in CameraTuningViewModel's
            // constructor) is safe to call here since the UI thread's dispatcher
            // already exists before any Window is constructed.
            TuningViewModel = new CameraTuningViewModel();

            this.InitializeComponent();

            // Native TitleBar customization
            this.ExtendsContentIntoTitleBar = true;
            this.SetTitleBar(AppTitleBar);

            _hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);

            var appWindow = this.AppWindow;

            if (Microsoft.UI.Windowing.AppWindowTitleBar.IsCustomizationSupported())
            {
                var titleBar = appWindow.TitleBar;
                titleBar.ExtendsContentIntoTitleBar = true;
                
                // Style buttons
                titleBar.ButtonBackgroundColor = Microsoft.UI.Colors.Transparent;
                titleBar.ButtonInactiveBackgroundColor = Microsoft.UI.Colors.Transparent;
                titleBar.ButtonHoverBackgroundColor = Microsoft.UI.ColorHelper.FromArgb(255, 42, 55, 89);
                titleBar.ButtonPressedBackgroundColor = Microsoft.UI.ColorHelper.FromArgb(255, 62, 78, 122);
                titleBar.ButtonForegroundColor = Microsoft.UI.Colors.White;
                titleBar.ButtonHoverForegroundColor = Microsoft.UI.Colors.White;
                titleBar.ButtonPressedForegroundColor = Microsoft.UI.Colors.White;
            }

            // Restore last window layout settings, cropped to screen size
            try
            {
                var settings = Services.SettingsService.Load();
                var layout = settings.MainWindowLayout;

                var displayArea = Microsoft.UI.Windowing.DisplayArea.GetFromWindowId(appWindow.Id, Microsoft.UI.Windowing.DisplayAreaFallback.Primary);
                var workArea = displayArea.WorkArea;

                _lastNormalWidth = Math.Min(layout.Width > 400 ? layout.Width : 1280, workArea.Width);
                _lastNormalHeight = Math.Min(layout.Height > 300 ? layout.Height : 800, workArea.Height);

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

            this.SizeChanged += MainWindow_SizeChanged;
            this.Closed += MainWindow_Closed;

            // ---- Terminal tabs — start with one, [+] adds more, each fully independent ----
            AddTerminalTab();

            // ---- Camera Tuning tab (TuningViewModel constructed above, before
            // InitializeComponent()) -------------------------------------------
            TuningViewModel.PropertyChanged += TuningViewModel_PropertyChanged;
            TuningViewModel.LogLineReceived += TuningLog_LineReceived;
            TuningViewModel.SpecialLineReceived += (msg, lvl) => TuningLogTerminal.AppendSpecialLine(msg, lvl);
            TuningLogTerminal.SetTitle("Camera Debug Log");

            Core.Initialize();
            // LibVLC/MediaPlayer are NOT created here — see PreviewVideoView_Initialized.
            // Constructing LibVLC without the VideoView's SwapChainOptions is what caused
            // the video to pop out into its own top-level window instead of rendering
            // embedded: libvlc's DirectX plugin needs to be told at construction time
            // which swap chain to render into.
        }

        // ================= TERMINAL TABS =================

        private void AddTerminalTab()
        {
            _terminalTabCounter++;

            var tabView = new TerminalTabView(_hwnd)
            {
                VerticalAlignment = VerticalAlignment.Stretch,
                HorizontalAlignment = HorizontalAlignment.Stretch
            };
            var tabItem = new TabViewItem
            {
                Header = $"Terminal {_terminalTabCounter}",
                IsClosable = true,
                Content = tabView,
                IconSource = new Microsoft.UI.Xaml.Controls.FontIconSource { Glyph = "" },
                VerticalAlignment = VerticalAlignment.Stretch,
                HorizontalAlignment = HorizontalAlignment.Stretch
            };

            _terminalTabs[tabItem] = tabView;

            // Camera Tuning is always the last item — insert new terminal tabs right before it
            // so tab order matches creation order (oldest leftmost), same as the old static layout.
            int insertIndex = Math.Max(0, RootTabView.TabItems.Count - 1);
            RootTabView.TabItems.Insert(insertIndex, tabItem);
            RootTabView.SelectedItem = tabItem;
        }

        private void RootTabView_AddTabButtonClick(TabView sender, object args)
        {
            AddTerminalTab();
        }

        private void RootTabView_TabCloseRequested(TabView sender, TabViewTabCloseRequestedEventArgs args)
        {
            if (args.Tab is not TabViewItem tabItem) return;
            if (!_terminalTabs.TryGetValue(tabItem, out var tabView)) return; // Camera Tuning tab isn't closable

            _terminalTabs.Remove(tabItem);
            sender.TabItems.Remove(tabItem);
            tabView.Cleanup();
        }

        // ================= CAMERA TUNING TAB =================

        /// <summary>
        /// Renders one debug log line from ak_rtsp's tee'd stdout (already
        /// stripped of the "LOG " wire prefix by CameraTuningViewModel) into
        /// TuningLogTerminal the same way each terminal tab feeds its own
        /// ConsoleTerminal — through AnsiParserService so scrollback
        /// pruning/timestamps/temp-log-file behavior match exactly, not a
        /// bare list.
        /// </summary>
        private void TuningLog_LineReceived(string line)
        {
            var lines = _tuningLogAnsiParser.ProcessIncomingText(line + "\n", out string logsSegment);
            TuningLogTerminal.AppendText(lines, logsSegment);
        }

        /// <summary>
        /// LibVLC must be constructed with the VideoView's SwapChainOptions —
        /// creating it beforehand (e.g. in the window constructor) with no
        /// arguments is what caused the video to pop out into its own
        /// top-level window instead of rendering embedded: libvlc's DirectX
        /// plugin needs to be told at construction time which swap chain to
        /// render into. This mirrors LibVLCSharp's own UWP sample pattern
        /// (WinUI shares the same VideoView implementation).
        /// </summary>
        private void PreviewVideoView_Initialized(object sender, InitializedEventArgs e)
        {
            _libVLC = new LibVLC(e.SwapChainOptions);
            _mediaPlayer = new MediaPlayer(_libVLC);
            PreviewVideoView.MediaPlayer = _mediaPlayer;

            // Covers the edge case where the tab connected before the swap
            // chain finished initializing.
            if (TuningViewModel.IsConnected)
            {
                using var media = new Media(_libVLC, TuningViewModel.RtspUrl, FromType.FromLocation);
                _mediaPlayer.Play(media);
            }
        }

        private void TuningViewModel_PropertyChanged(object? sender, PropertyChangedEventArgs e)
        {
            if (e.PropertyName != nameof(CameraTuningViewModel.IsConnected) || _libVLC == null || _mediaPlayer == null)
                return;

            if (TuningViewModel.IsConnected)
            {
                using var media = new Media(_libVLC, TuningViewModel.RtspUrl, FromType.FromLocation);
                _mediaPlayer.Play(media);
            }
            else
            {
                _mediaPlayer.Stop();
            }
        }

        /// <summary>x:Bind helper — inverted Visibility (function bindings need
        /// an exact return type match; unlike direct property bindings, x:Bind
        /// won't auto-convert a bool return value to Visibility here).</summary>
        public Visibility Not(bool value) => value ? Visibility.Collapsed : Visibility.Visible;

        /// <summary>x:Bind helper — "label: value" display text for tuning
        /// card rows.</summary>
        public string FormatParam(string label, double value) => $"{label}: {value:0}";
        public string FormatParam(string label, string value) => $"{label}: {value}";

        private void TuningConnectButton_Click(object sender, RoutedEventArgs e) => TuningViewModel.Connect();
        private void TuningDisconnectButton_Click(object sender, RoutedEventArgs e) => TuningViewModel.Disconnect();
        private void TuningRefreshButton_Click(object sender, RoutedEventArgs e) => TuningViewModel.Refresh();

        // These all read the fired value from the sender/event args directly
        // rather than the x:Bind-TwoWay-bound ViewModel property. There's no
        // guaranteed ordering between x:Bind's auto-generated "push value to
        // source" code and an explicit named event handler on the same
        // control/event — reading the VM property here could see a stale
        // value from before the user's latest interaction (this is exactly
        // what caused the AE Enabled toggle to appear to "reset itself": it
        // was re-sending the OLD value, and the server's echoed reply then
        // overwrote the UI back to the old state). Also, Slider.PointerReleased
        // is unreliable in WinUI3 (the internal Thumb captures the pointer
        // and often doesn't bubble it to the Slider, and it never fires at
        // all for keyboard-driven adjustments) — ValueChanged is what
        // actually fires reliably for every kind of interaction.
        private void AeEnabledToggle_Toggled(object sender, RoutedEventArgs e) =>
            TuningViewModel.SendSet("ae.enabled", ((ToggleSwitch)sender).IsOn ? "1" : "0");

        private void AeStableRangeSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e) =>
            TuningViewModel.SendSet("ae.stable_range", ((int)e.NewValue).ToString());

        private void AeHoldRangeSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e) =>
            TuningViewModel.SendSet("ae.hold_range", ((int)e.NewValue).ToString());

        private void AeSpeedSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e) =>
            TuningViewModel.SendSet("ae.speed", ((int)e.NewValue).ToString());

        private void AeExpMaxSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e) =>
            TuningViewModel.SendSet("ae.exp_max", ((int)e.NewValue).ToString());

        private void IspSaturationSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e) =>
            TuningViewModel.SendSet("isp.saturation", ((int)e.NewValue).ToString());

        private void IspContrastSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e) =>
            TuningViewModel.SendSet("isp.contrast", ((int)e.NewValue).ToString());

        private void IspBrightnessSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e) =>
            TuningViewModel.SendSet("isp.brightness", ((int)e.NewValue).ToString());

        private void IspSharpnessSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e) =>
            TuningViewModel.SendSet("isp.sharpness", ((int)e.NewValue).ToString());

        private void NightModeCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (e.AddedItems.Count > 0 && e.AddedItems[0] is string mode)
                TuningViewModel.SendSet("night.mode", mode);
        }

        private void NightTriggerSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e) =>
            TuningViewModel.SendSet("night.trigger_hw_exp", ((int)e.NewValue).ToString());

        private void NightDaySlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e) =>
            TuningViewModel.SendSet("night.day_hw_exp", ((int)e.NewValue).ToString());

        private void NightConfirmSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e) =>
            TuningViewModel.SendSet("night.confirm_samples", ((int)e.NewValue).ToString());

        private void NightLockSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e) =>
            TuningViewModel.SendSet("night.lock_ms", ((int)e.NewValue).ToString());

        // ================= WINDOW LIFECYCLE =================

        private void MainWindow_SizeChanged(object sender, WindowSizeChangedEventArgs args)
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

        private void MainWindow_Closed(object sender, WindowEventArgs args)
        {
            // Save layout settings
            try
            {
                var settings = Services.SettingsService.Load();
                var layout = settings.MainWindowLayout;
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

            foreach (var tabView in _terminalTabs.Values)
            {
                tabView.Cleanup();
            }

            TuningViewModel.Disconnect();
            _mediaPlayer?.Stop();
            _mediaPlayer?.Dispose();
            _libVLC?.Dispose();
        }
    }
}
