#!/bin/sh
# install_ak_rtsp_firmware.sh — flash the real de-Tuyafied APP partition:
# gzip-compressed, anyka_ipc removed, ak_rtsp in place as process_0.
# Padded to the exact full partition size (5,242,880 bytes) and written
# with the same fast single-shot dd mechanism as restore.sh.
# Expects: /mnt/custom_firmware/ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs — picks the
# lexicographically newest match (this camera has no working RTC, so file mtimes are
# meaningless; the build timestamp is baked into the filename itself instead).
#
# *** ONLY run this from a bare UART `init=/bin/sh` recovery shell. ***
# NEVER run it over Telnet or from a fully-booted normal session. Flashing
# while WiFi/camera/ISP drivers are active corrupts the write (DMA
# contention with the SPI-NOR controller) — confirmed on hardware after an
# entire investigation session chasing phantom squashfs/kernel bugs that
# were really just this.
#
# Run: sh /mnt/install_ak_rtsp_firmware.sh

DEV=/dev/mtdblock7
EXPECTSIZE=5242880

# Pick the newest ak_rtsp_firmware_*.squashfs by filename (lexicographic == chronological
# for YYYYMMDD_HHMMSS). Shell glob results are sorted, so the last match in the loop wins.
IMG=""
for f in /mnt/custom_firmware/ak_rtsp_firmware_[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]_[0-9][0-9][0-9][0-9][0-9][0-9].squashfs; do
    IMG="$f"
done
if [ -z "$IMG" ] || [ ! -f "$IMG" ]; then
    echo "ERROR: no ak_rtsp_firmware_*.squashfs found in /mnt/custom_firmware/"
    exit 1
fi

dd_with_progress() {
    dd if="$1" of="$2" bs=64k 2>/tmp/dd_err &
    DDPID=$!
    SECS=0
    while kill -0 $DDPID 2>/dev/null; do
        echo "  ... still writing (${SECS}s elapsed)"
        sleep 1
        SECS=$((SECS+1))
    done
    wait $DDPID
    RET=$?
    [ $RET -ne 0 ] && cat /tmp/dd_err
    return $RET
}

get_size() {
    S=$(stat -c%s "$1" 2>/dev/null)
    case "$S" in
        ''|*[!0-9]*) echo "ERROR: could not determine size of $1" >&2; exit 1 ;;
    esac
    echo "$S"
}

SIZE=$(get_size "$IMG")
echo "Image: $IMG ($SIZE bytes)"
if [ "$SIZE" -ne "$EXPECTSIZE" ]; then
    echo "ERROR: expected exactly $EXPECTSIZE bytes (full partition), got $SIZE — refusing to flash"
    exit 1
fi

echo "md5sum of $IMG (record this before flashing):"
md5sum "$IMG"

echo "About to overwrite $DEV (APP partition) with $IMG."
echo "This is the real ak_rtsp firmware — anyka_ipc removed, ak_rtsp as process_0."
printf "Type YES to continue: "
read CONFIRM
if [ "$CONFIRM" != "YES" ]; then
    echo "Aborted."
    exit 1
fi

dd_with_progress "$IMG" "$DEV" || { echo "ERROR: dd failed, DO NOT reboot yet — investigate first"; exit 1; }
sync
echo "Flash write done."

# No same-session read-back/cmp check here on purpose: drop_caches is a
# confirmed no-op on this kernel, so any post-write cmp against $DEV in this
# same session only reads back the page cache, never physical flash — it
# proves nothing. A real reboot is the only trustworthy check.
echo "Reboot to load the ak_rtsp firmware: reboot"
