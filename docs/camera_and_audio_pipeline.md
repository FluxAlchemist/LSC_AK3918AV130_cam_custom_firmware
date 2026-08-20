# Camera, ISP & Audio Pipeline Architecture

This document details the reverse-engineered hardware control mechanisms for the GC20C3 image sensor, Anyka ISP pipeline, H.264 hardware encoder, and PCM microphone capture.

---

## 1. Video Subsystem (`ak_rtsp`)

```
[GC20C3 CMOS Sensor] (I2C)
        │
        ▼
[Anyka ISP Pipeline (ak_isp.ko)] ──> 24 Processing Modules (BLC, LSC, CCM, NR, Gamma, AE)
        │
        ▼
[VI Subsystem (ak_vi.ko)] ─────────> Ch0: 1920x1080 (Main) | Ch1: 640x360 (Sub)
        │
        ▼
[B2I Bridge & VENC (ak_venc.ko)] ──> Hardware H.264 Main Profile Encoder (VBR+)
        │
        ▼
[RTSP Server Engine] ──────────────> RTP / RTSP Video Stream (Port 554, path-agnostic)
```

### 1.1 Sensor Initialization (GC20C3)
The GalaxyCore GC20C3 1080p CMOS sensor is initialized over the I2C bus with a 134-register configuration sequence.

### 1.2 ISP 24-Module Processing Pipeline
The ISP hardware pipeline consists of 24 distinct hardware stages configured in sequence:
- **BLC** (Black Level Correction)
- **LSC** (Lens Shading Correction)
- **CCM** (Color Correction Matrix)
- **NR** (2D/3D Noise Reduction)
- **WDR** (Wide Dynamic Range & Tone Mapping)
- **Gamma Curve Correction**
- **White Balance:** Calibrated static gains (`R=460, G=256, B=504`) eliminate the harsh blue cast present on raw uncalibrated boots.

---

## 2. Important Discoveries & Kernel Fixes

### 2.1 The `ak_isp.ko` Divide-By-Zero Race Condition (Site A)
During early testing, the kernel log would continuously emit `Division by zero in kernel.` warnings during stream start.

- **Root Cause:** If stream capture (`vi_start_capture()` / `STREAMON`) begins before the AE module completes parameter initialization, the kernel ISP worker evaluates uninitialized frame timing denominators.
- **The Fix:** In `src/ak_rtsp/isp.c`, moving `ae_init_isp_params()` to execute strictly **before** `vi_set_channel_attr()` and `vi_start_capture()` completely closes the race condition.

### 2.2 VENC Rate Control Optimization
Initial builds suffered from macroblocking during rapid motion. Reverse-engineering `anyka_ipc` revealed that the stock firmware uses **VBR+** mode instead of standard AVBR. Enabling VBR+ with adjusted QP bounds provides crisp 1080p video with low bitrates.

---

## 3. Audio Subsystem (`audio.c`)

```
[Microphone Hardware]
        │
        ▼
[/dev/pcmC0D0c] (ak_pcm.ko)
        │
        ▼
[L16 / 8000 Hz / Mono PCM Capture Loop]
        │
        ▼
[RTP Audio Packets + RTCP Sender Reports] ──> VLC / Synology NAS Synced Audio
```

### 3.1 The PCM Ioctl Setter Discovery
The stock audio driver `/dev/pcmC0D0c` required a specific ioctl call before audio captures would succeed. 

Decompiling `ak_pcm.ko` in Ghidra revealed that ioctl `0x401c50e0` (which had been mistakenly presumed to be a parameter query "GET_PARS") is actually a **copy-from-user commit operation** that writes audio configuration into the kernel buffer. Replaying the structure correctly eliminated all audio read errors.

### 3.2 Audio-Video Synchronization (RTCP Sender Reports)
While `ffplay` tolerated simple RTP timestamps, VLC suffered from periodic audio dropouts. Implementing **RTCP Sender Reports** (SR packets sent every 5 seconds mapping RTP timestamps to NTP wall-clock time) resolved playback stutters, ensuring stable audio across all RTSP media players.

---

## 4. Day / Night & IR-Cut Solenoid Control (`ircut.c`, `night.c`)

The camera features an electromechanical IR-cut filter switched via dual GPIO solenoid pulses:
- **Day Mode:** IR-cut filter engaged (blocks infrared, accurate daytime colors).
- **Night Mode:** IR-cut filter disengaged + IR LED illuminator on.

At boot, a two-click polarity reset pulse initializes the physical solenoid to a known state.
