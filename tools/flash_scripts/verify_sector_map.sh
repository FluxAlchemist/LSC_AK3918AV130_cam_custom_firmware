#!/bin/sh
# verify_sector_map.sh — after flashing sector_map_test.bin and rebooting,
# check every 4096-byte sector of the APP partition against what should be
# there and report which specific sector indices reverted/failed. Each
# sector of the source image is filled with its own little-endian sector
# index repeated throughout, so a mismatch is trivial to detect per-sector
# without needing hexdump/od (neither is available in this rootfs busybox).
#
# Run from the SAME boot the image was flashed and rebooted into (does not
# need /usr mounted — reads the raw device directly).

IMG=/mnt/custom_firmware/sector_map_test.bin
DEV=/dev/mtdblock7
SECTOR=4096
NSECT=1280

mount | grep -q "on /tmp type tmpfs" || mount -t tmpfs tmpfs /tmp

if [ ! -f "$IMG" ]; then
    echo "ERROR: $IMG not found (need the SD card mounted with the original test image for comparison)"
    exit 1
fi

BAD=0
i=0
while [ "$i" -lt "$NSECT" ]; do
    dd if="$IMG" of=/tmp/a bs=$SECTOR skip=$i count=1 2>/dev/null
    dd if="$DEV" of=/tmp/b bs=$SECTOR skip=$i count=1 2>/dev/null
    if ! cmp -s /tmp/a /tmp/b; then
        echo "BAD sector $i (byte offset $((i*SECTOR)))"
        BAD=$((BAD+1))
    fi
    i=$((i+1))
    if [ $((i % 200)) -eq 0 ]; then
        echo "  ... checked $i/$NSECT ($BAD bad so far)"
    fi
done

echo "Done. $BAD/$NSECT sectors bad."
rm -f /tmp/a /tmp/b
