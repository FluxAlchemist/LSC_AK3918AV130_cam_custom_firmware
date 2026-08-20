#!/bin/sh
# restore_full_firmware.sh — restore ALL partitions to the original stock dump
# (full recovery: UBOOT/ENV/ENVBK/DTB/KERNEL/ROOTFS/CONFIG/APP, not just APP)
# Expects: /mnt/original_firmware/mtdblock0.bin .. mtdblock7.bin
# Run from a UART root shell: sh /mnt/restore_full_firmware.sh
#
# Writes partition-by-partition (not a single dd of full_flash.bin to a raw
# /dev/mtd0 device) since each mtdblockN.bin is exactly what was originally
# dumped from and matches /dev/mtdblockN 1:1 — same safe method already used
# throughout this project.

SRC=/mnt/original_firmware

# index:device:expected_size:name
PARTS="
0:/dev/mtdblock0:204800:UBOOT
1:/dev/mtdblock1:4096:ENV
2:/dev/mtdblock2:4096:ENVBK
3:/dev/mtdblock3:65536:DTB
4:/dev/mtdblock4:1572864:KERNEL
5:/dev/mtdblock5:1032192:ROOTFS
6:/dev/mtdblock6:262144:CONFIG
7:/dev/mtdblock7:5242880:APP
"

# busybox dd has no status=progress, and its default bs=512 is dead slow
# (207KB/s observed flashing APP alone) — bs=64k fixes that; this heartbeat
# covers whatever time is still needed so a flash in progress never looks
# like a hang, which matters even more here across 8 partitions in a row.
dd_with_progress() {
    dd if="$1" of="$2" bs=64k 2>/tmp/dd_err &
    DDPID=$!
    SECS=0
    while kill -0 $DDPID 2>/dev/null; do
        printf "\r    ... still writing (%ds elapsed)" "$SECS"
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
# the APP partition's own coreutils) — using it here would silently break
# this exact size check in precisely the scenario this script exists for
# (APP already down, recovering via a bare init=/bin/sh shell). `stat` IS
# a rootfs busybox symlink, and the case pattern below makes a stat
# failure a hard abort instead of quietly comparing against an empty string.
get_size() {
    S=$(stat -c%s "$1" 2>/dev/null)
    case "$S" in
        ''|*[!0-9]*) echo "ERROR: could not determine size of $1" >&2; exit 1 ;;
    esac
    echo "$S"
}

echo "Checking all partition images before touching anything..."
for P in $PARTS; do
    IDX=$(echo $P | cut -d: -f1)
    DEV=$(echo $P | cut -d: -f2)
    EXPECT=$(echo $P | cut -d: -f3)
    NAME=$(echo $P | cut -d: -f4)
    IMG="$SRC/mtdblock${IDX}.bin"
    if [ ! -f "$IMG" ]; then
        echo "ERROR: $IMG not found — aborting, nothing has been written"
        exit 1
    fi
    SIZE=$(get_size "$IMG")
    if [ "$SIZE" -ne "$EXPECT" ]; then
        echo "ERROR: $IMG is $SIZE bytes, expected $EXPECT ($NAME) — aborting, nothing has been written"
        exit 1
    fi
    echo "  OK: $NAME -> $DEV ($SIZE bytes)"
done

echo ""
echo "This will overwrite EVERY partition (UBOOT/ENV/ENVBK/DTB/KERNEL/ROOTFS/CONFIG/APP)"
echo "with the original stock dump, undoing ALL changes made by this project."
printf "Type YES to continue: "
read CONFIRM
if [ "$CONFIRM" != "YES" ]; then
    echo "Aborted."
    exit 1
fi

for P in $PARTS; do
    IDX=$(echo $P | cut -d: -f1)
    DEV=$(echo $P | cut -d: -f2)
    NAME=$(echo $P | cut -d: -f4)
    IMG="$SRC/mtdblock${IDX}.bin"
    echo "Flashing $NAME -> $DEV ..."
    dd_with_progress "$IMG" "$DEV" || { echo "ERROR: dd failed on $DEV, STOP and investigate before rebooting"; exit 1; }
done
sync

echo "Done. All partitions restored to stock. Disconnect and reconnect USB-C power to power cycle."
