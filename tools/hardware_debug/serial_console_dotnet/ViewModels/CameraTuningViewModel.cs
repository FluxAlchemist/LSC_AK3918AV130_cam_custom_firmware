using System;
using System.ComponentModel;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Media;
using serial_console_dotnet.Services;

namespace serial_console_dotnet.ViewModels
{
    /// <summary>
    /// Backs the "Camera Tuning" tab: talks to ak_rtsp's TCP control server
    /// (see ak_rtsp/control.h) to read/write live
    /// AE and day/night parameters, and surfaces the tee'd debug log.
    ///
    /// Deliberately independent of <see cref="MainViewModel"/>/<see cref="ConnectionService"/> —
    /// this tab's control connection and the main terminal connection are
    /// expected to be open at the same time.
    ///
    /// Debug log rendering is NOT owned here (no string collection) — this
    /// ViewModel only raises <see cref="LogLineReceived"/> / <see cref="SpecialLineReceived"/>;
    /// MainWindow.xaml.cs renders them into a second <c>TerminalView</c>
    /// instance (the same control the main terminal tab uses), so the
    /// tuning tab's log gets the same scrollback pruning, timestamps,
    /// temp-file logging, and special-line styling for free instead of a
    /// bare list with none of that.
    /// </summary>
    public class CameraTuningViewModel : INotifyPropertyChanged
    {
        private readonly CameraControlService _controlService = new();
        private readonly DispatcherQueue _dispatcherQueue;

        public event PropertyChangedEventHandler? PropertyChanged;

        /// <summary>A raw device/log text line (already stripped of the wire
        /// protocol's "LOG " prefix) — feed through an AnsiParserService and
        /// into a TerminalView's AppendText(), same as the main terminal.</summary>
        public event Action<string>? LogLineReceived;

        /// <summary>A client-side status/error annotation (message, level) —
        /// feed into a TerminalView's AppendSpecialLine(message, level).
        /// Level matches TerminalView's convention: "info" or "error".</summary>
        public event Action<string, string>? SpecialLineReceived;

        public CameraTuningViewModel()
        {
            _dispatcherQueue = DispatcherQueue.GetForCurrentThread();
            _controlService.ConnectionStateChanged += OnConnectionStateChanged;
            _controlService.LineReceived += OnLineReceived;
            _controlService.ErrorOccurred += OnErrorOccurred;
        }

        public string[] NightModeOptions { get; } = { "auto", "day", "night" };

        // ---- Connection -----------------------------------------------------

        private string _host = "192.168.50.4";
        public string Host { get => _host; set => SetProperty(ref _host, value); }

        // String-typed for direct TextBox.Text x:Bind (no int<->string TwoWay
        // converter needed); parsed on Connect().
        private string _port = "8091";
        public string Port { get => _port; set => SetProperty(ref _port, value); }

        private bool _isConnected;
        public bool IsConnected { get => _isConnected; set => SetProperty(ref _isConnected, value); }

        private string _statusText = "Disconnected";
        public string StatusText { get => _statusText; set => SetProperty(ref _statusText, value); }

        private SolidColorBrush _statusColor = new(Colors.Red);
        public SolidColorBrush StatusColor { get => _statusColor; set => SetProperty(ref _statusColor, value); }

        /// <summary>RTSP URL for the embedded LibVLCSharp preview — same host,
        /// standard RTSP port 554, no credentials (matches ak_rtsp's server).</summary>
        public string RtspUrl => $"rtsp://{Host}:554/";

        // ---- AE tuning --------------------------------------------------------

        private double _aeStableRange = 10;
        public double AeStableRange { get => _aeStableRange; set => SetProperty(ref _aeStableRange, value); }

        private double _aeHoldRange = 10;
        public double AeHoldRange { get => _aeHoldRange; set => SetProperty(ref _aeHoldRange, value); }

        private double _aeSpeed = 10;
        public double AeSpeed { get => _aeSpeed; set => SetProperty(ref _aeSpeed, value); }

        private double _aeExpMax = 32767;
        public double AeExpMax { get => _aeExpMax; set => SetProperty(ref _aeExpMax, value); }

        private bool _aeEnabled = true;
        public bool AeEnabled { get => _aeEnabled; set => SetProperty(ref _aeEnabled, value); }

        // ---- Picture tuning (see ak_rtsp/isp.c for the Ghidra-verified
        // mechanism behind each of these) ------------------------------------

        private double _ispSaturation = 0;
        public double IspSaturation { get => _ispSaturation; set => SetProperty(ref _ispSaturation, value); }

        private double _ispContrast = 0;
        public double IspContrast { get => _ispContrast; set => SetProperty(ref _ispContrast, value); }

        private double _ispBrightness = 0;
        public double IspBrightness { get => _ispBrightness; set => SetProperty(ref _ispBrightness, value); }

        private double _ispSharpness = 0;
        public double IspSharpness { get => _ispSharpness; set => SetProperty(ref _ispSharpness, value); }

        // ---- Night mode tuning --------------------------------------------------

        private string _nightMode = "auto";
        public string NightMode { get => _nightMode; set => SetProperty(ref _nightMode, value); }

        private string _nightState = "day";
        public string NightState { get => _nightState; set => SetProperty(ref _nightState, value); }

        private double _nightTriggerHwExp = 1800;
        public double NightTriggerHwExp { get => _nightTriggerHwExp; set => SetProperty(ref _nightTriggerHwExp, value); }

        private double _nightDayHwExp = 800;
        public double NightDayHwExp { get => _nightDayHwExp; set => SetProperty(ref _nightDayHwExp, value); }

        private double _nightConfirmSamples = 10;
        public double NightConfirmSamples { get => _nightConfirmSamples; set => SetProperty(ref _nightConfirmSamples, value); }

        private double _nightLockMs = 60000;
        public double NightLockMs { get => _nightLockMs; set => SetProperty(ref _nightLockMs, value); }

        // ---- Encoder (read-only in v1) ----

        private string _vencMinQp = "28 (read-only)";
        public string VencMinQp { get => _vencMinQp; set => SetProperty(ref _vencMinQp, value); }

        private string _vencMaxQp = "43 (read-only)";
        public string VencMaxQp { get => _vencMaxQp; set => SetProperty(ref _vencMaxQp, value); }

        // ---- Actions ------------------------------------------------------------

        public void Connect()
        {
            if (!int.TryParse(Port, out var port))
            {
                SpecialLineReceived?.Invoke($"Invalid control port '{Port}'", "error");
                return;
            }

            try
            {
                _controlService.Connect(Host, port);
            }
            catch (Exception ex)
            {
                SpecialLineReceived?.Invoke($"Connect failed: {ex.Message}", "error");
            }
        }

        public void Disconnect() => _controlService.Disconnect();

        /// <summary>Sends "SET &lt;param&gt; &lt;value&gt;" — called from the
        /// tab's code-behind on slider PointerReleased / toggle changes so a
        /// drag doesn't flood one SET per pixel.</summary>
        public void SendSet(string param, string value)
        {
            if (!IsConnected) return;
            try
            {
                _controlService.Send($"SET {param} {value}");
            }
            catch (Exception ex)
            {
                SpecialLineReceived?.Invoke($"SET {param} failed: {ex.Message}", "error");
            }
        }

        /// <summary>Sends "LIST" to refresh every bound property from the
        /// camera's current state.</summary>
        public void Refresh()
        {
            if (!IsConnected) return;
            try
            {
                _controlService.Send("LIST");
            }
            catch (Exception ex)
            {
                SpecialLineReceived?.Invoke($"LIST failed: {ex.Message}", "error");
            }
        }

        // ---- Wire-format handling ------------------------------------------------

        private void OnConnectionStateChanged(bool connected)
        {
            _dispatcherQueue.TryEnqueue(() =>
            {
                IsConnected = connected;
                StatusText = connected ? $"Connected to {Host}:{Port}" : "Disconnected";
                StatusColor = new SolidColorBrush(connected ? Colors.LimeGreen : Colors.Red);
                SpecialLineReceived?.Invoke(
                    connected ? $"Connected to control server {Host}:{Port}" : "Control server disconnected",
                    "info");
                if (connected) Refresh();
            });
        }

        private void OnErrorOccurred(string message)
        {
            _dispatcherQueue.TryEnqueue(() => SpecialLineReceived?.Invoke(message, "error"));
        }

        private void OnLineReceived(string line)
        {
            _dispatcherQueue.TryEnqueue(() => HandleLine(line));
        }

        private void HandleLine(string line)
        {
            if (string.IsNullOrEmpty(line) || line == ".") return;

            if (line.StartsWith("LOG "))
            {
                LogLineReceived?.Invoke(line.Substring(4));
                return;
            }

            if (line.StartsWith("ERR"))
            {
                SpecialLineReceived?.Invoke(line, "error");
                return;
            }

            // "OK ..." confirms a SET actually took effect — log it, but do
            // NOT re-apply the value to the bound property. The property is
            // already at this value (that's what we just sent); re-applying
            // our own echo back onto the exact TwoWay-bound slider/toggle
            // that triggered the SET is a feedback loop waiting to happen —
            // any tiny mismatch (e.g. a slider allowing fractional values
            // getting truncated to an int before sending, so the server's
            // exact-integer echo differs from the slider's current
            // fractional position) causes a real value change, which fires
            // ValueChanged again, which sends another SET, indefinitely.
            // Only GET/LIST responses (bare "name=value", no "OK " prefix)
            // should ever populate the UI from the server.
            if (line.StartsWith("OK "))
            {
                SpecialLineReceived?.Invoke(line, "info");
                return;
            }

            int eq = line.IndexOf('=');
            if (eq <= 0) return;

            string name  = line.Substring(0, eq);
            string value = line.Substring(eq + 1);
            ApplyKeyValue(name, value);
        }

        private void ApplyKeyValue(string name, string value)
        {
            switch (name)
            {
                case "ae.stable_range":       if (double.TryParse(value, out var v1)) AeStableRange = v1; break;
                case "ae.hold_range":         if (double.TryParse(value, out var v2)) AeHoldRange = v2; break;
                case "ae.speed":              if (double.TryParse(value, out var v3)) AeSpeed = v3; break;
                case "ae.exp_max":            if (double.TryParse(value, out var v4)) AeExpMax = v4; break;
                case "ae.enabled":            AeEnabled = value == "1"; break;
                case "isp.saturation":        if (double.TryParse(value, out var vs)) IspSaturation = vs; break;
                case "isp.contrast":          if (double.TryParse(value, out var vc)) IspContrast = vc; break;
                case "isp.brightness":        if (double.TryParse(value, out var vbr)) IspBrightness = vbr; break;
                case "isp.sharpness":         if (double.TryParse(value, out var vsh)) IspSharpness = vsh; break;
                case "night.mode":            NightMode = value; break;
                case "night.state":           NightState = value; break;
                case "night.trigger_hw_exp":  if (double.TryParse(value, out var v5)) NightTriggerHwExp = v5; break;
                case "night.day_hw_exp":      if (double.TryParse(value, out var v6)) NightDayHwExp = v6; break;
                case "night.confirm_samples": if (double.TryParse(value, out var v7)) NightConfirmSamples = v7; break;
                case "night.lock_ms":         if (double.TryParse(value, out var v8)) NightLockMs = v8; break;
                case "venc.minqp":            VencMinQp = value; break;
                case "venc.maxqp":            VencMaxQp = value; break;
            }
        }

        // ---- INotifyPropertyChanged boilerplate (mirrors MainViewModel's pattern) --

        private bool SetProperty<T>(ref T storage, T value, [CallerMemberName] string? propertyName = null)
        {
            if (EqualityComparer<T>.Default.Equals(storage, value)) return false;
            storage = value;
            OnPropertyChanged(propertyName);
            return true;
        }

        private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
}
