#!/bin/sh
# restore.sh — restore ONLY the stock APP partition
# (undoes install_custom_firmware.sh; leaves UBOOT/KERNEL/ROOTFS/CONFIG alone)
# Expects: /mnt/original_firmware/mtdblock7.bin
#
# *** ONLY run this from a bare UART `init=/bin/sh` recovery shell. ***
# This script's "100% reliable" track record is because it has only ever
# been run that way (often out of necessity, since a broken APP partition
# means Telnet can't come up anyway) — NOT because writing this partition
# is inherently safe from a normal Telnet session. Flashing while
# WiFi/camera/ISP drivers are active corrupts the write via DMA contention
# with the SPI-NOR controller.

IMG=/mnt/original_firmware/mtdblock7.bin
DEV=/dev/mtdblock7
EXPECTSIZE=5242880

# busybox dd has no status=progress, and its default bs=512 is dead slow
# (207KB/s observed) — bs=64k fixes that; this heartbeat covers whatever
# time is still needed so a flash in progress never looks like a hang.
dd_with_progress() {
    dd if="$1" of="$2" bs=64k 2>/tmp/dd_err &
    DDPID=$!
    SECS=0
    while kill -0 $DDPID 2>/dev/null; do
        printf "\r  ... still writing (%ds elapsed)" "$SECS"
        sleep 1
        SECS=$((SECS+1))
    done
    wait $DDPID
    RET=$?
    echo ""
    [ $RET -ne 0 ] && cat /tmp/dd_err
    return $RET
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

if [ ! -f "$IMG" ]; then
    echo "ERROR: $IMG not found"
    exit 1
fi

SIZE=$(get_size "$IMG")
echo "Image: $IMG ($SIZE bytes)"
if [ "$SIZE" -ne "$EXPECTSIZE" ]; then
    echo "ERROR: expected exactly $EXPECTSIZE bytes, got $SIZE — refusing to flash"
    exit 1
fi

echo "About to overwrite $DEV (APP partition) with the ORIGINAL stock dump."
printf "Type YES to continue: "
read CONFIRM
if [ "$CONFIRM" != "YES" ]; then
    echo "Aborted."
    exit 1
fi

dd_with_progress "$IMG" "$DEV" || { echo "ERROR: dd failed, DO NOT reboot yet — investigate first"; exit 1; }
sync
echo "Done. Disconnect and reconnect USB-C power to power cycle and return to stock behavior."
