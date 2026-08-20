# Serial Console App (`serial_console_dotnet/serial_console_dotnet/`)

Native C# WinUI 3 desktop app, runs **unpackaged** (bypasses appx sandbox) — replaces
`plink`/`minicom` for the camera exploit workflow and hosts the live "Camera Tuning" tab for
`ak_rtsp`. See the main [README.md](../../../README.md) for the overall project this app supports, and
**[`docs/serial_console_and_camera_tuning.md`](../../../docs/serial_console_and_camera_tuning.md)** (repo root) for the full architecture writeup — tab layout, control
protocol, LibVLC preview wiring, and every gotcha discovered so far. Read that doc before touching
`MainWindow.xaml(.cs)`, `CameraTuningViewModel`, or the LibVLC preview — this file is intentionally
just the fast-orientation summary + local conventions.

## Build & run

```powershell
cd tools\hardware_debug\serial_console_dotnet
dotnet build
dotnet run --launch-profile "serial_console_dotnet (Unpackaged)"
```

Always use the `(Unpackaged)` launch profile — the packaged profile runs inside an appx sandbox
that blocks the raw COM-port and file-transfer access this app depends on.

## Project shape

| Path | Purpose |
|---|---|
| `MainWindow.xaml(.cs)` | Owns the `TabView` (`RootTabView`) only — no connection state of its own. Hosts an arbitrary number of `TerminalTabView`-backed tabs. Replaces standard operating system window borders with a custom draggable dark Mica titlebar and overlays system caption controls. |
| `Controls/TerminalTabView.xaml(.cs)` | **One fully independent terminal workspace** — connection sidebar, console terminal, and utility scripts. Spawns the separate, monitor-centered `DeviceExplorerWindow` when the file explorer is opened over telnet, routing transfer callbacks back to the active window context. |
| `Controls/DeviceExplorerWindow.xaml(.cs)` | Hosts `DeviceExplorerView` inside a dedicated resizable window. Instantiated with the parent's `WindowId`, querying the `DisplayArea` monitor info to center itself exactly on the active screen. Customized with custom titlebars matching the main window. |
| `Controls/DeviceExplorerView.xaml(.cs)` | The core file explorer UI. Features a left-hand favorites sidebar, directory list/grid contents view, collapsible FTP credentials pane, transfer progress bars, and file/folder upload and download actions. |
| `Controls/AddressBar.xaml(.cs)` | Intercepts navigation actions. Standardized with a `PointerPressed` interceptor on the parent border: walks the visual tree to allow standard breadcrumb button clicks to pass through, but opens direct path text editing if any empty space is clicked. |
| `ViewModels/MainViewModel.cs` | Backs one `TerminalTabView` instance (constructed per-tab, not a singleton). Hand-rolled `INotifyPropertyChanged`/`SetProperty<T>` — no shared ViewModel base class in this codebase. |
| `ViewModels/CameraTuningViewModel.cs` | Tuning tab. No UI dependency — raises `LogLineReceived`/`SpecialLineReceived` events; `MainWindow` renders them. |
| `Controls/TerminalView.xaml(.cs)` | ANSI-aware scrollback terminal. Instantiated once per `TerminalTabView` plus once for the tuning debug log — each instance owns its own temp log file and scroll state independently. |
| `Services/ConnectionService.cs` | One connection — Serial *and* Telnet behind one class (`ConnectionMode` enum), not an interface split. One instance per `TerminalTabView`. |
| `Services/CameraControlService.cs` | Separate TCP client for `ak_rtsp`'s control server (port 8091). Deliberately independent from `ConnectionService` — a tuning connection and any number of terminal connections are expected to be open concurrently. |
| `Services/*` (rest) | `BootExploitService` (U-Boot root-shell automation), `FileTransferService`/`SdCardService` (upload/download, mount/unmount), `AnsiParserService` (mutable per-stream parse state — **never share one instance across two streams**), `HistoryService`, `SettingsService`, `ShellExecutionService`, `TerminalSessionManager`, `AnsiInputEncoder`, `DialogService`, `ConnectionOrchestrator`. All are constructed fresh per `TerminalTabView` (except `SettingsService`, which is `static`). |

No shared `IConnection` interface — services with a similar shape (background read thread,
`event Action<...>`, `ConnectionStateChanged`) follow that shape by convention, not inheritance.

## Local conventions / gotchas

- **Construction order in `MainWindow`:** everything is constructed after
  `InitializeComponent()` *except* `CameraTuningViewModel`, which must come **before** — some
  XAML-declared slider bounds fire `ValueChanged` synchronously during `InitializeComponent()`,
  and the handler dereferences the tuning ViewModel.
- **Never read an `x:Bind`-bound ViewModel property back inside that same control's own event
  handler** (e.g. inside `Slider.ValueChanged`, reading `TuningViewModel.SomeProp` to decide what
  to send). There's no guaranteed ordering between `x:Bind`'s push-to-source and a named handler on
  the same event — read the value straight off `e`/`sender` instead (`e.NewValue`,
  `((ToggleSwitch)sender).IsOn`, `e.AddedItems[0]`).
- **Don't write a command's own success echo back onto the bound property that triggered it** —
  that's a feedback loop (`docs/serial_console_and_camera_tuning.md` has the full slider/`SET`/`"OK ..."` postmortem).
- `TerminalView`'s auto-scroll-to-bottom judges "has the user scrolled away" against the offset
  *we* last scrolled to (`_lastProgrammaticOffset`), not the live `ScrollableHeight` — a fast
  burst of incoming lines grows the extent faster than our own scroll calls can keep up, and
  comparing against a still-growing `ScrollableHeight` produced false "detached" states. Judge
  detachment as "offset moved backward from our last target," not "offset isn't yet at the bottom."
- `TerminalView.Clear()` always forces `_autoScroll = true` and clears the scroll-lock indicator —
  clearing the screen is a deliberate reset and should never leave a stale scroll-lock behind.
- Pointer capture during click-drag text selection (`TerminalListView.CapturePointer`) reroutes
  wheel events' source to the captured element, bypassing the ListView's internal `ScrollViewer` —
  `TerminalListView_PointerWheelChanged` manually drives `ChangeView` while `_isSelecting` is true
  so wheel-scroll-while-selecting keeps working.
- **`SerialPort.Open()`/`.Close()` can hang forever after a surprise USB-serial removal** (a
  long-standing `System.IO.Ports` bug — the internal read thread never unblocks against a handle
  that no longer refers to a real device). This used to make the Connect/Disconnect button
  permanently unresponsive after unplugging the adapter. Fix in `ConnectionService`: `Open()` runs
  on a background `Task` bounded by a 3s `Task.Wait` timeout; `Close()` in `Disconnect()` flips
  `_isConnected`/fires `ConnectionStateChanged` immediately and does the actual OS-level close on a
  detached fire-and-forget `Task` — a leaked close thread is preferable to a frozen UI. Also,
  `SerialPort_DataReceived`'s catch block now calls `Disconnect()` on any read error (previously it
  only logged), since a read failure on an already-open handle almost always means the device just
  disappeared.
- `LibVLCSharp.WinUI` bundles `LibVLCSharp` core source into its own assembly — **never also
  reference the plain `LibVLCSharp` package**, it produces a colliding second `LibVLCSharp.dll` and
  a runtime `TypeLoadException` for `VideoView` even though the build succeeds. Its `VideoView`
  class lives in namespace `LibVLCSharp.Platforms.Windows`, not `LibVLCSharp.WinUI`. Construct
  `LibVLC`/`MediaPlayer` only inside `VideoView.Initialized` using `e.SwapChainOptions`, never
  eagerly — otherwise libvlc's DirectX plugin opens its own top-level window instead of rendering
  into the embedded control.
