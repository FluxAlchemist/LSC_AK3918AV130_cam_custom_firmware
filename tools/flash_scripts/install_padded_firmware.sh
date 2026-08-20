#!/bin/sh
# install_padded_firmware.sh — flash a custom APP image padded to the full
# partition size, using the exact same fast single-shot dd mechanism as
# restore.sh (bs=64k, one dd call, no chunking/pacing).
#
# This exists to isolate one specific variable: restore.sh
# ALWAYS writes the full 5,242,880-byte partition and always succeeds;
# every custom image flashed so far has been smaller than the partition
# and always reverts its tail content after reboot, regardless of content,
# compressor, block size, or write-chunking/pacing. Padding the image out
# to the exact same 5,242,880-byte size and writing it the exact same way
# restore does tests whether "write spans the whole device" is the actual
# deciding factor. Expects: /mnt/custom_firmware/roundtrip_v43_padded.squashfs
# (or whatever you rename it to below).
#
# Run from a UART or Telnet root shell: sh /mnt/install_padded_firmware.sh

IMG=/mnt/custom_firmware/roundtrip_v43_padded.squashfs
DEV=/dev/mtdblock7
EXPECTSIZE=5242880

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

if [ ! -f "$IMG" ]; then
    echo "ERROR: $IMG not found"
    exit 1
fi

SIZE=$(get_size "$IMG")
echo "Image: $IMG ($SIZE bytes)"
if [ "$SIZE" -ne "$EXPECTSIZE" ]; then
    echo "ERROR: expected exactly $EXPECTSIZE bytes (full partition), got $SIZE — refusing to flash"
    exit 1
fi

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

echo "Reading back $DEV and comparing against $IMG (same-session check only —"
echo "drop_caches is a confirmed no-op on this kernel, so this proves nothing"
echo "about physical media; reboot is the only trustworthy check)..."
CMP_OUT=$(cmp "$IMG" "$DEV" 2>&1)
echo "  $CMP_OUT"

echo "Reboot to load the padded custom firmware: reboot"
