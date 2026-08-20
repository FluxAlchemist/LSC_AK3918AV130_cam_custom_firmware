# Building Your Own Firmware From Your Own Dump

This repo ships the `ak_rtsp` **source code** and the flashing tools, but deliberately does
**not** ship a stock APP-partition dump or a pre-built firmware image — that partition is the
vendor's proprietary Tuya firmware and can't legally be redistributed. To produce a flashable
image you dump *your own* camera's APP partition, patch it locally, and rebuild it. This is a
few extra steps, but it means the exact firmware you flash is always something you built
yourself from your own hardware.

This guide covers the whole loop end-to-end: WSL setup → dump → unsquash → patch → rebuild →
verify → pad → flash.

---

## 1. Why WSL?

Two build steps — repacking the APP partition as a squashfs image and round-trip-verifying
it — need `mksquashfs`/`unsquashfs`, which aren't available on native Windows. More
importantly, the vendor's `app_extracted/` tree contains symlinks and specific executable-bit
permissions (e.g. `bin/ak_rtsp_wrapper.sh` must stay executable) that Windows tools like
`Copy-Item`/`robocopy`/git-bash `cp` **cannot reliably preserve**. `mksquashfs` needs those bits
correct in the source tree, so this step has to happen on a real Linux filesystem — WSL is the
simplest way to get one without a second machine.

```powershell
wsl --install -d Ubuntu
```

Reboot if prompted, then open the new Ubuntu shell (or `wsl -d Ubuntu` from PowerShell) and
install the squashfs tools:

```bash
sudo apt update
sudo apt install -y squashfs-tools
```

`ak_rtsp` itself still cross-compiles fine natively on Windows via Zig (see the main
[Building from Source](../README.md#building-from-source) section) — WSL is only needed for the
squashfs packaging step below.

> **Note:** this entire project — including this build pipeline — has only ever been developed
> and tested on Windows 11 + WSL/Ubuntu. It will very likely work fine on native Linux (you'd
> just skip WSL entirely and run `mksquashfs`/`unsquashfs` directly), but that path is untested.
> Same goes for other WSL distros or cross-compilers besides Zig — up to you to sort out any
> differences.

---

## 2. Dump Your Own APP Partition

You should already have a full backup from **Step 0** of the main README (all 8
`mtdblock*.bin` files on your SD card). The one this guide cares about is `mtdblock7.bin` —
the APP partition, 5,242,880 bytes (5120 KB) — copied to your PC, e.g. at:

```
original_firmware/mtdblock7.bin
```

If you skipped Step 0, go back and do it now — you can't safely rebuild firmware without a
known-good backup of what you're overwriting.

---

## 3. Unsquash It

From WSL, on your PC (not the camera):

```bash
cd /mnt/c/path/to/original_firmware
unsquashfs -d app_extracted mtdblock7.bin
```

This gives you `app_extracted/` — the real, complete stock filesystem tree that gets mounted
to `/usr` on the camera (`bin/`, `lib/`, `local/`, `modules/`, `sbin/`). This is the tree
`src/ak_rtsp/build_firmware.sh` expects to find (via the `APP_EXTRACTED` env var, or by default
at `src/ak_rtsp/app_extracted`).

---

## 4. Build `ak_rtsp`

From PowerShell (native Windows build, see main README for details):

```powershell
cd src/ak_rtsp
make CC="zig cc -target arm-linux-musleabi -mcpu=arm926ej_s -static"
```

---

## 5. Patch and Rebuild the Squashfs

`build_firmware.sh` automates the whole patch-and-rebuild step. Point it at your
`app_extracted/` tree (either by placing it at `src/ak_rtsp/app_extracted`, or via the
`APP_EXTRACTED` env var), then run it from WSL:

```bash
cd /mnt/c/path/to/repo/src/ak_rtsp
APP_EXTRACTED=/mnt/c/path/to/original_firmware/app_extracted bash build_firmware.sh
```

Or simply `make firmware` from PowerShell (it shells out to WSL for you — see the `firmware`
target in `src/ak_rtsp/Makefile`).

What this does, using the templates in `tools/firmware_patch_templates/` (see that folder's
README for the full diff-against-stock explanation):

1. Copies `app_extracted/` → `app_patched/` (only the first time — re-runs reuse the existing
   `app_patched/` so any manual edits you've made survive).
2. Deletes `app_patched/bin/anyka_ipc` (the 5.9 MB Tuya cloud binary).
3. Installs `ak_rtsp_wrapper.sh` and repoints `_ht_process.ini`'s `process_0` at it instead of
   `anyka_ipc`.
4. Copies your freshly built `ak_rtsp` binary into `app_patched/bin/ak_rtsp`.
5. Repacks the tree with `mksquashfs ... -comp gzip -b 131072 -all-root -noappend`.
6. **Round-trip verifies** — unsquashes the result back out and `md5sum`-diffs every file
   against `app_patched/`. This is cheap and catches build mistakes before they ever reach
   your camera; never skip it.
7. Pads the image to the exact partition size (5,242,880 bytes, 0xFF-filled — matching erased
   NOR convention) and names it with a build timestamp via `pad_build.sh`, producing
   `ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs`.

> **Why keep `cmdserver`/`msgserver` and not touch anything else?** They're pure local IPC
> (UNIX socket / SysV message queue), not Tuya cloud clients — removing them stalls boot. See
> `docs/flash_partition_map_and_rootfs.md` for the full de-Tuyafication rationale.

> **Why a fixed timestamp-based filename instead of relying on file mtime?** This camera has no
> working RTC (it boots to 2000-01-01, no NTP) and even a PC-side copy can reset a file's mtime.
> Baking the build time into the filename itself, and having every flashing script pick the
> lexicographically-last `ak_rtsp_firmware_*.squashfs` match, sidesteps this entirely.

---

## 6. Flash It

Follow the main [Quick Installation Guide](../README.md#quick-installation-guide) or
[Firmware Building & Flashing Guide](firmware_building_and_flashing.md) from here — copy the
new `ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs`, `flash_tool`, and `install_with_flash_tool.sh`
to your SD card and run the installer.

---

## Iterating Quickly Without Reflashing

If you're actively tuning `ak_rtsp` (ISP parameters, AE, etc.), you don't need to repeat this
whole flash cycle for every change. Keep your SD card permanently inserted and use
`tools/flash_scripts/start_rtsp.sh` to kill the running `ak_rtsp`/`anyka_ipc`, reload the venc
kernel modules, and launch a freshly-uploaded binary — no squashfs rebuild or reflash needed.
Only rebuild and reflash the APP partition once you're happy with a build and want it to boot
automatically without the SD card present.
