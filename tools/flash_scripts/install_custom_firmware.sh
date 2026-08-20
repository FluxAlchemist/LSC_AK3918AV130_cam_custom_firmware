#!/bin/sh
# install_custom_firmware.sh — flash the de-Tuyafied APP partition
# Expects: /mnt/custom_firmware/ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs — picks the
# lexicographically newest match (this camera has no working RTC, so file mtimes are
# meaningless; the build timestamp is baked into the filename itself instead).
#
# *** ONLY run this from a bare UART `init=/bin/sh` recovery shell. ***
# NEVER run it over Telnet or from a fully-booted normal session. Flashing
# while WiFi/camera/ISP drivers are active corrupts the write (DMA
# contention with the SPI-NOR controller) — confirmed on hardware after an
# entire investigation session chasing phantom squashfs/kernel bugs that
# were really just this. Same content, same script, flashed from a bare
# recovery shell instead of Telnet: works every time.

DEV=/dev/mtdblock7
MAXSIZE=5242880

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

# busybox dd has no status=progress, and its default bs=512 is why a 5MB
# write took 25s of dead silence last time (207KB/s) — a larger bs below
# fixes the slowness; this heartbeat covers whatever time is still needed
# so a flash in progress never looks indistinguishable from a hung shell.
#
# bs=64k turned out to have a real bug (short, unaligned 16384-byte final
# block for a size like 2768896 — fixed by moving to bs=4k, an exact
# divisor of every image we produce), but bs=4k alone did NOT fix the real
# problem: a same-session, cache-dropped read-back after a bs=4k write
# still verified clean, yet the id table was back to the original firmware's
# stale bytes after a real reboot. A manual, standalone bs=1 write to that
# exact same offset DID persist correctly across reboot. So the address
# itself is fine — the difference is writing ~2.7MB in one continuous
# sequential burst vs. one small isolated write. This project already hit
# an analogous timing issue elsewhere (the upload feature in
# serial_console_dotnet deliberately paces 512-byte blocks with 150ms
# delays to protect this device's UART RX buffers) — the working theory is
# that back-to-back sector erase/program cycles with no gap between them
# don't give one specific sector's program cycle time to physically settle
# before the next sector's operation starts, corrupting just that sector
# while the rest of a fast sequential write is unaffected. Fix: write one
# 4096-byte sector at a time via dd's seek/skip, with a short delay between
# each, instead of one continuous dd call.
#
# Uses a real newline per update, not \r-in-place: a \r overwrite only
# renders correctly in an interactive terminal. Over a logged serial/telnet
# session the log viewer timestamps by line, so a run of \r-separated
# updates with no \n between them gets buffered and flushed as one clump
# on the next real newline — looked like the counter was stuck at "0s"
# and the write finished a second later, when it had actually been running
# the whole time. One line per second sidesteps that entirely.
SECTOR=4096
DELAY_MS=30
dd_with_progress() {
    SRC="$1"
    DST="$2"
    FSIZE=$(get_size "$SRC")
    NSECT=$((FSIZE / SECTOR))
    i=0
    LASTPRINT=$(date +%s 2>/dev/null || echo 0)
    while [ "$i" -lt "$NSECT" ]; do
        dd if="$SRC" of="$DST" bs=$SECTOR skip=$i seek=$i count=1 conv=notrunc 2>/tmp/dd_err
        if [ $? -ne 0 ]; then
            echo "ERROR: sector $i/$NSECT failed:"
            cat /tmp/dd_err
            return 1
        fi
        usleep $((DELAY_MS * 1000)) 2>/dev/null
        i=$((i+1))
        if [ $((i % 100)) -eq 0 ]; then
            echo "  ... wrote sector $i/$NSECT"
        fi
    done
    echo "  ... wrote sector $NSECT/$NSECT"
    return 0
}

# `wc` is not symlinked in this device's rootfs busybox (only present via
# the APP partition's own coreutils) — using it here silently broke this
# exact size check the one time it mattered (APP already down, recovering
# via a bare init=/bin/sh shell). `stat` IS a rootfs busybox symlink, and
# the case pattern below makes a stat failure a hard abort instead of
# quietly comparing against an empty string.
get_size() {
    S=$(stat -c%s "$1" 2>/dev/null)
    case "$S" in
        ''|*[!0-9]*) echo "ERROR: could not determine size of $1" >&2; exit 1 ;;
    esac
    echo "$S"
}

SIZE=$(get_size "$IMG")
echo "Image: $IMG ($SIZE bytes)"
if [ "$((SIZE % 4096))" -ne 0 ]; then
    echo "ERROR: image size is not a multiple of 4096 — the bs=4k dd write below"
    echo "would end on a short, unaligned final block, the exact condition that"
    echo "silently failed to commit to real flash last time. Refusing to flash."
    exit 1
fi
if [ "$SIZE" -gt "$MAXSIZE" ]; then
    echo "ERROR: image is larger than the APP partition ($MAXSIZE bytes) — refusing to flash"
    exit 1
fi

echo "md5sum of $IMG (record this before flashing):"
md5sum "$IMG"

echo "About to overwrite $DEV (APP partition) with $IMG."
printf "Type YES to continue: "
read CONFIRM
if [ "$CONFIRM" != "YES" ]; then
    echo "Aborted."
    exit 1
fi

dd_with_progress "$IMG" "$DEV" || { echo "ERROR: dd failed, DO NOT reboot yet — investigate first"; exit 1; }
sync
echo "Flash write done."

# No same-session read-back/cmp check here on purpose: `echo 3 >
# /proc/sys/vm/drop_caches` is a confirmed no-op on this kernel (verified via
# /proc/meminfo Cached: before/after, unchanged), so any post-write cmp against
# $DEV in this same session is just reading back the page cache the write
# itself populated, not physical flash — it can only ever report a false-positive
# match and proves nothing. A real reboot is the only trustworthy check on this
# device.

# Tried a post-flash test-mount to a throwaway path here (not /usr) as a
# reboot-free verification step — turns out that DOESN'T work: a block
# device can only be mounted once at a time regardless of target path, so
# if $DEV is already mounted live (e.g. /usr on a normal boot you ran this
# from over Telnet), any second mount attempt fails with "Device or
# resource busy" no matter where you point it. This only works from a
# fresh boot where $DEV was never mounted yet (UART init=/bin/sh) — and at
# that point you may as well just reboot for real and read dmesg, since
# there's no live system in that state to protect anyway. So: no
# reboot-free verification is possible from a normal Telnet session.
# Reboot now and check dmesg for "mount: mounting /dev/mtdblock7 on /usr
# failed" — if that's absent and boot continues normally, it worked.
echo "Reboot to load the custom firmware: reboot"
