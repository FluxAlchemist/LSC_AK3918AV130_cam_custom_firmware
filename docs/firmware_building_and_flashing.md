# Firmware Building & Flashing Guide

> ⚠️ **CAUTION: FLASHING IS STRICTLY AT YOUR OWN RISK**  
> This firmware and its tooling were synthesized and assembled with AI assistance from an internal research and exploration project. It has not been exhaustively tested by hand in all edge cases and scenarios.  
> Modifying embedded firmware or writing to the SPI NOR flash chip carries an inherent risk of bricking your device. In the worst case, an unrecoverable hard-brick may occur requiring physical chip desoldering and an external SPI programmer (such as CH341A).  
> **The creators and contributors of this project assume zero responsibility for damaged hardware, lost warranties, or bricked devices.** Proceed only if you understand these risks.

---

## 1. MANDATORY: Backup Your Stock Firmware First!

Before flashing any custom image, **always dump your camera's original stock firmware partitions to your MicroSD card and save a copy on your PC.**

### Step 1: Mount the SD Card
From your Telnet or UART shell:
```sh
mount -t vfat /dev/mmcblk0p1 /mnt
mkdir -p /mnt/original_firmware
```

### Step 2: Dump All Partitions
Run the following commands to create exact byte-for-byte dumps of all 8 flash partitions:
```sh
dd if=/dev/mtdblock0 of=/mnt/original_firmware/mtdblock0.bin bs=64k
dd if=/dev/mtdblock1 of=/mnt/original_firmware/mtdblock1.bin bs=64k
dd if=/dev/mtdblock2 of=/mnt/original_firmware/mtdblock2.bin bs=64k
dd if=/dev/mtdblock3 of=/mnt/original_firmware/mtdblock3.bin bs=64k
dd if=/dev/mtdblock4 of=/mnt/original_firmware/mtdblock4.bin bs=64k
dd if=/dev/mtdblock5 of=/mnt/original_firmware/mtdblock5.bin bs=64k
dd if=/dev/mtdblock6 of=/mnt/original_firmware/mtdblock6.bin bs=64k
dd if=/dev/mtdblock7 of=/mnt/original_firmware/mtdblock7.bin bs=64k
sync
```

> 🛑 **IMPORTANT:**
> - Copy `/mnt/original_firmware` to your computer for safe keeping.
> - Leave a copy in `/mnt/original_firmware/` on your SD card so you can restore stock at any time using `tools/flash_scripts/restore.sh` or `tools/flash_scripts/restore_full_firmware.sh`.

---

## 2. Restoring Stock Firmware (Emergency Fallback)

If anything goes wrong during testing or you wish to revert to stock Tuya:

- **Restore APP partition only (reverts RTSP to stock):**
  ```sh
  sh /mnt/restore.sh
  ```
- **Restore ALL partitions (full factory state):**
  ```sh
  sh /mnt/restore_full_firmware.sh
  ```

---

## 3. Building `ak_rtsp` & `flash_tool`

The `ak_rtsp` binary is a standalone C program with custom ARMv5TE assembly atomic stubs.

> **Prerequisite:** `ak_rtsp`'s `Makefile` compiles [OpenIPC's SmolRTSP](https://github.com/OpenIPC/smolrtsp)
> sources directly in, and expects them checked out as a sibling directory (`../smolrtsp`
> relative to `src/ak_rtsp/`) — this is not included in this repo. Clone it and run CMake's
> configure step (no full build needed) to populate its FetchContent dependencies:
> ```bash
> git clone https://github.com/OpenIPC/smolrtsp.git
> cd smolrtsp && cmake -S . -B build
> ```
> See [Building from Source](../README.md#building-from-source) in the main README for details.

### Method A: Zig Cross-Compiler (Recommended)
```bash
cd src/ak_rtsp
make CC="zig cc -target arm-linux-musleabi -mcpu=arm926ej_s -static"
zig cc -target arm-linux-musleabi -mcpu=arm926ej_s -static -O2 flash_tool.c -o flash_tool
```

### Method B: Anyka GCC Toolchain
```bash
cd src/ak_rtsp
make CC=arm-anykav200-linux-uclibcgnueabi-gcc
arm-anykav200-linux-uclibcgnueabi-gcc -O2 flash_tool.c -o flash_tool
```

---

## 4. Packaging the APP SquashFS

This step requires your **own** dump of the camera's stock APP partition (not included in this
repo — see below) plus WSL for `mksquashfs`/`unsquashfs`. Full walkthrough, including WSL setup
and how to dump/unsquash your own partition:
**[Building Your Own Firmware From Your Own Dump](building_your_own_firmware.md)**.

Short version, once you have `app_extracted/` (your own unsquashed stock APP dump):

```bash
cd src/ak_rtsp
APP_EXTRACTED=/path/to/app_extracted bash build_firmware.sh
```

This applies the patch templates in `tools/firmware_patch_templates/` (deletes `anyka_ipc`,
installs `ak_rtsp_wrapper.sh`, repoints `_ht_process.ini`), repacks with `mksquashfs -comp gzip
-b 131072 -all-root -noappend`, round-trip verifies, and pads/timestamps the result via
`pad_build.sh` — producing `ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs`.

---

## 5. The SPI NOR DMA Contention Gotcha

> ⚠️ **WARNING:** On the Anyka AK3918AV130 SoC, writing directly to `/dev/mtdblock7` using `dd` on a running system causes **silent flash corruption** due to DMA contention between the SPI-NOR controller, active ISP video capture, and WiFi drivers.
>
> Always use `flash_tool` (`src/ak_rtsp/flash_tool.c`) / `install_with_flash_tool.sh` which operates directly on `/dev/mtd7` (raw char MTD) in 4KB erase-blocks with immediate read-back verification and re-erase retries.

---

## 6. Flashing Step-by-Step

### Step 1: Copy Files to MicroSD Card
Format the SD card (FAT32) and copy:
- `custom_firmware/ak_rtsp_firmware_*.squashfs`
- `flash_tool` (compiled ARM binary)
- `tools/flash_scripts/install_with_flash_tool.sh`
- `tools/flash_scripts/restore.sh` *(for safety)*
- `original_firmware/mtdblock*.bin` *(your stock backup)*

### Step 2: Connect via Telnet or UART
Connect to the camera shell:
```bash
telnet 192.168.x.x
```

### Step 3: Run the Flash Tool
```sh
mount -t vfat /dev/mmcblk0p1 /mnt
sh /mnt/install_with_flash_tool.sh
```

### Step 4: Verify the Flash Logs!
> 🛑 **IMPORTANT: Carefully check the final output of `install_with_flash_tool.sh`:**
> ```text
> FAULT SUMMARY: 0 chunks needed retry, 0 total failed verify attempts.
> Flashing and verification completed successfully!
> ```
> **DO NOT power off if verification failed!** If errors are reported, run `sh /mnt/restore.sh` to restore your stock backup before powering off.

### Step 5: Power Cycle the Camera
> ℹ️ **NOTE:** The software `reboot` command **does not work reliably** on this hardware platform. You must **manually disconnect and reconnect the USB-C power cable** to perform a clean power cycle.
