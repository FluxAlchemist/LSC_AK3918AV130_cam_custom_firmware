# Serial Console & Live Camera Tuning Application

The repository includes a dedicated Windows desktop application located in `tools/hardware_debug/serial_console_dotnet/`. Built with **C# / WinUI 3 and .NET 9** (running unpackaged), this utility serves as both a high-speed serial/Telnet hacking console and a live graphical tuning suite for the `ak_rtsp` firmware.

---

## 1. Features Overview

### 1.1 High-Performance Serial & Telnet Terminal
- **Buffered Output Flusher:** Queues incoming high-baud kernel boot floods and flushes in 50 ms batches, preventing UI lag during heavy boot output.
- **ANSI Color Highlighting:** Full real-time decoding of ANSI color codes onto styled text blocks.
- **One-Click U-Boot Root Shell:** Automatically sends the 4-command U-Boot kernel argument override sequence with precise timings, waits for the Linux shell prompt, and mounts `/proc`, `/sys`, `/usr`, and `/etc/config`.
- **Direct Keystroke Mode:** Intercepts arrows, Tab, Backspace, and Escape for seamless use of `vi` and interactive BusyBox shell commands over serial.

### 1.2 Device File Explorer
- Dedicated native window for exploring the camera's filesystem over Telnet or UART.
- **Base64 File Transfers:** Uploads and downloads files seamlessly over raw text-only serial links without requiring external network tools.
- **Drag & Drop:** Drop files directly onto the window to trigger base64 streaming into the current directory.
- **Integrated FTP Support:** Collapsible panel for full FTP transfers when networking is active.

### 1.3 Live Camera Tuning & Embedded RTSP Preview
- Connects directly to `ak_rtsp`'s TCP control server on port **8091**.
- **Live Parameter Controls:** Real-time sliders and toggles for:
  - `ae.stable_range` / `ae.hold_range` (AE dead-band widths)
  - `ae.speed` (Convergence aggressiveness)
  - `ae.exp_max` (Maximum exposure time ceiling)
  - `ae.enabled` (Freeze/unfreeze exposure for A/B testing)
  - `night.mode` (`auto`, `day`, `night`)
  - `night.trigger_hw_exp` / `night.day_hw_exp` (Day ↔ Night switching thresholds)
  - `night.lock_ms` (Anti-flap hysteresis lock duration)
- **Live Video Preview:** Embedded low-latency RTSP video stream powered by LibVLC, allowing you to see the immediate effect of ISP parameter adjustments.
- **Live Kernel & ISP Log Feed:** Streams real-time diagnostic output and AE window calculations from the camera.

---

## 2. Building & Running the Application

### Requirements
- Windows 10/11 (x64, ARM64, or x86)
- [.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0)
- Visual Studio 2022 (with *.NET Desktop Development* and *Windows App SDK C# templates*)

### Running via Visual Studio
1. Open `tools/hardware_debug/serial_console_dotnet/serial_console_dotnet.slnx` (or `.csproj`) in Visual Studio 2022.
2. Select the configuration (e.g. `Debug` / `x64` or `Release` / `x64`).
3. Press **F5** to build and run unpackaged.

### Running via Command Line
```powershell
cd tools/hardware_debug/serial_console_dotnet
dotnet run -c Release
```

---

## 3. Architecture & Control Protocol

```
[WinUI 3 Tuning Tab] ──(TCP :8091)──> [ak_rtsp control.c server]
       │                                     │
       ├─ SET ae.speed 7 ──────────────────> ├─ Re-applies to ISP via isp_tunnel()
       ├─ GET ae.exp_max ──────────────────> ├─ Reads live hardware register state
       └─ LOG stream ──────────────────────> └─ Redirects stdout to control socket
```

The control server uses a simple ASCII text protocol:
- `LIST` — Lists all current parameters and values (`name=value`).
- `GET <param>` — Queries a single parameter value.
- `SET <param> <value>` — Updates a parameter live without requiring a camera restart.
- `LOG <message>` — Real-time asynchronous stdout logs pushed from the firmware to the desktop app.
