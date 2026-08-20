# Flash Partition Map & De-Tuyafication Architecture

This document outlines the internal storage layout, boot sequence, and how the stock Tuya cloud software was permanently removed.

---

## 1. SPI NOR Flash Layout (8 MiB Total)

The camera uses an 8 MiB SPI NOR flash (`XM25QH64D` or Winbond equivalent) mapped across 8 MTD partitions:

| Partition | Offset | Size | MTD Device | Filesystem | Description |
|---|---|---|---|---|---|
| `UBOOT` | `0x000000` | 200 KB | `/dev/mtdblock0` | Raw | U-Boot bootloader |
| `ENV` | `0x032000` | 4 KB | `/dev/mtdblock1` | Raw | Primary U-Boot environment |
| `ENVBK` | `0x033000` | 4 KB | `/dev/mtdblock2` | Raw | Backup U-Boot environment |
| `DTB` | `0x034000` | 64 KB | `/dev/mtdblock3` | Raw | Device Tree Blob |
| `KERNEL` | `0x044000` | 1536 KB | `/dev/mtdblock4` | uImage | Linux Kernel (ARM926EJ-S) |
| `ROOTFS` | `0x1c4000` | 1008 KB | `/dev/mtdblock5` | SquashFS (gzip) | Read-only base root filesystem |
| `CONFIG` | `0x2c0000` | 256 KB | `/dev/mtdblock6` | JFFS2 (rw) | Persistent writable user config |
| `APP` | `0x300000` | 5120 KB | `/dev/mtdblock7` | SquashFS (XZ) | Application layer (mounted to `/usr`) |

---

## 2. Stock Boot Sequence & Traps

On boot, the kernel mounts `/` from `ROOTFS`, then `/etc/init.d/rc.local` mounts:
1. `/dev/mtdblock7` (APP) → `/usr`
2. `/dev/mtdblock6` (CONFIG) → `/etc/config`
3. Runs `/usr/sbin/main.sh` → `/usr/sbin/service.sh`

### The Shadow Overwrite Trap
In the stock firmware, `service.sh` explicitly copies `/usr/local/shadow` over `/etc/config/shadow` on every boot. This constantly reverts the root password to the unknown factory hash.

**The Fix:**
BusyBox `login` checks `/etc/passwd` directly. If the password field contains an MD5crypt hash instead of `x`, it verifies against that hash directly and **never consults `/etc/shadow`**. Since `service.sh` never touches `/etc/config/passwd`, writing a password hash to `/etc/config/passwd` provides a permanent Telnet backdoor on stock firmware!

```sh
# Example: Setting root password to 'admin' permanently via /etc/config/passwd
printf 'root:$1$anyka001$qk6nsIZ/vuhZcaN7A1ZIr/:0:0:root:/:/bin/sh\ndaemon:x:1:1:daemon:/usr/sbin:/bin/sh\nbin:x:2:2:bin:/bin:/bin/sh\nnobody:x:99:99:nobody:/home:/bin/sh\n' > /etc/config/passwd
```

---

## 3. De-Tuyafication Strategy

The Tuya cloud client (`anyka_ipc`) is a monolithic 5.9 MB binary that controls:
- Hardware ISP and video encoding
- Cloud pairing and MQTT connection
- RTSP serving (gated strictly behind cloud activation)

### Clean Exec-Takeover
Rather than rebuilding the entire Linux kernel and rootfs from scratch, we perform an **exec-takeover** at the application layer (`APP` partition / `/dev/mtdblock7`):

1. **Keep stock hardware initialization:** All driver modules (`ak_isp.ko`, `sensor_gc20c3.ko`, `atbm6x3x_wifi_usb.ko`, `ak_pcm.ko`), networking scripts (`ethernet.sh`), and standard daemons (`telnetd`, `ftpd`) remain intact.
2. **Preserve `cmdserver` and `msgserver`:** These binaries handle local IPC (UNIX domain sockets and SysV message queues). They are **not** cloud daemons and are required for system stability.
3. **Replace `anyka_ipc`:** `_ht_process.ini` is modified so the watchdog launches `ak_rtsp_wrapper.sh` instead of `anyka_ipc`.
4. **Standalone RTSP Server (`ak_rtsp`):** Our lightweight C program takes direct ownership of the camera ISP, VENC hardware, and PCM microphone capture without needing any external cloud dependencies.
