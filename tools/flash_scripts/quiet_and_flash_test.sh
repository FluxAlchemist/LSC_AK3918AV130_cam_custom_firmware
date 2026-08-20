#!/bin/sh
# quiet_and_flash_test.sh — kill anyka_ipc/watchdog, unload video/venc/audio kernel
# modules (leaving WiFi/SD/flash alone), then run flash_tool. Exists to test whether
# quieting the DMA-heavy camera/audio subsystems from a live Telnet session gets
# anywhere close to the bare-UART-recovery-shell flashing reliability, without ever
# touching UART. This tests the DMA-contention
# theory against a live Telnet session instead of a bare UART recovery shell.
#
# This is a TEST harness, not the proven-safe method — the bare UART recovery shell
# (install_ak_rtsp_firmware.sh / install_custom_firmware.sh) remains the reliable
# fallback. Keep a full recovery plan ready (restore.sh, UART access)
# before running this over Telnet.
#
# Run from Telnet: sh /mnt/quiet_and_flash_test.sh

DEV=/dev/mtd7
TOOL=/mnt/flash_tool
EXPECTSIZE=5242880

if [ ! -f "$TOOL" ]; then
    echo "ERROR: Flash tool $TOOL not found. Please compile it and copy to SD card root."
    exit 1
fi

# Pick the newest ak_rtsp_firmware_*.squashfs by filename (lexicographic == chronological
# for YYYYMMDD_HHMMSS). This camera has no working RTC, so file mtimes are meaningless —
# the build timestamp is baked into the filename itself instead. Shell glob results are
# sorted, so the last match in the loop wins.
IMG=""
for f in /mnt/custom_firmware/ak_rtsp_firmware_[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]_[0-9][0-9][0-9][0-9][0-9][0-9].squashfs; do
    IMG="$f"
done
if [ -z "$IMG" ] || [ ! -f "$IMG" ]; then
    echo "ERROR: no ak_rtsp_firmware_*.squashfs found in /mnt/custom_firmware/"
    exit 1
fi

SIZE=$(stat -c%s "$IMG" 2>/dev/null)
if [ -z "$SIZE" ]; then
    echo "ERROR: Could not get size of $IMG"
    exit 1
fi

if [ "$SIZE" -ne "$EXPECTSIZE" ]; then
    echo "ERROR: expected exactly $EXPECTSIZE bytes (full partition), got $SIZE — refusing to flash"
    exit 1
fi

echo "md5sum of $IMG (record this before flashing):"
md5sum "$IMG"

echo "=== Quiet phase: killing anyka_ipc/watchdog, feeding hw watchdog ourselves ==="
killall -9 anyka_ipc 2>/dev/null
killall -9 watchdog  2>/dev/null
sleep 2

# Feed the hardware watchdog every 5s in the background — same pattern as
# start_rtsp.sh. Opening /dev/watchdog once for the whole script avoids the
# "Device or resource busy" race that per-iteration redirects cause. Without
# this, killing watchdog above leaves nothing petting the hardware timer and
# the board reboots mid-flash.
while true; do printf V; sleep 5; done > /dev/watchdog &
WDPID=$!
echo "hardware watchdog feeder started (pid $WDPID)"

echo "=== Unloading video/venc/audio kernel modules (WiFi/SD/flash left alone) ==="
# Order matches the vendor's own sensor_driver.sh uninstall() function exactly
# (app_extracted/local/sensor_driver.sh) — leaf-consumers before the modules
# they depend on.
rmmod ak_venc_bridge  2>/dev/null && echo "  rmmod ak_venc_bridge  OK" || echo "  rmmod ak_venc_bridge  skipped/failed"
rmmod ak_venc_adapter 2>/dev/null && echo "  rmmod ak_venc_adapter OK" || echo "  rmmod ak_venc_adapter skipped/failed"
rmmod ak_isp          2>/dev/null && echo "  rmmod ak_isp          OK" || echo "  rmmod ak_isp          skipped/failed"
rmmod sensor_gc20c3   2>/dev/null && echo "  rmmod sensor_gc20c3   OK" || echo "  rmmod sensor_gc20c3   skipped/failed"
rmmod ak_pcm          2>/dev/null && echo "  rmmod ak_pcm          OK" || echo "  rmmod ak_pcm          skipped/failed"
# ak_uio (ISP/VENC userspace register mmap) and ak_ion (camera/venc DMA buffer
# allocator) are generic support modules loaded by main.sh before the sensor
# driver — unload them last, after their consumers above are gone, in case
# anything above still held a reference.
rmmod ak_uio          2>/dev/null && echo "  rmmod ak_uio          OK" || echo "  rmmod ak_uio          skipped/failed"
rmmod ak_ion          2>/dev/null && echo "  rmmod ak_ion          OK" || echo "  rmmod ak_ion          skipped/failed"

echo "Modules NOT touched (kept alive on purpose): cfg80211, usb-common, usbcore,"
echo "ak_hcd, atbm6x3x_wifi_usb (WiFi/Telnet), mmc_core, mmc_block, ak_mci, exfat (SD)."
echo ""
echo "lsmod after quieting:"
lsmod

echo ""
echo "=== Flash phase ==="
# Ensure device node exists (major 90, minor 14 for partition 7 RW char device)
if [ ! -c "$DEV" ]; then
    echo "Device node $DEV not found, creating it..."
    mknod "$DEV" c 90 14 || { echo "ERROR: failed to create device node $DEV"; kill $WDPID 2>/dev/null; exit 1; }
fi

echo "About to overwrite $DEV (APP partition) with $IMG using $TOOL, live over Telnet"
echo "with camera/audio quieted (WiFi/SD/flash still active). This is a TEST of whether"
echo "quieting is enough to avoid the known DMA-contention corruption — it may still fail."
printf "Type YES to continue: "
read CONFIRM
if [ "$CONFIRM" != "YES" ]; then
    echo "Aborted."
    kill $WDPID 2>/dev/null
    exit 1
fi

"$TOOL" "$IMG" "$DEV"
RET=$?

if [ $RET -eq 0 ]; then
    echo "Flashing and verification completed successfully!"
    echo "You can safely reboot now: reboot"
else
    echo "ERROR: Flashing/Verification failed with code $RET!"
    echo "DO NOT reboot yet — check the error messages above, and the fault summary"
    echo "line from flash_tool for how many chunk retries/faults it hit before failing."
fi

# Leave the watchdog feeder running until an explicit reboot — killing it now would
# let the hardware timer reboot the board on its own schedule instead of on our terms.
echo "Hardware watchdog feeder (pid $WDPID) left running — reboot when ready."
exit $RET
