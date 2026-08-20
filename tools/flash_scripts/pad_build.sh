#!/bin/bash
# Builds the padded, ready-to-flash APP squashfs with the build timestamp baked directly
# into the filename: ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs. Deliberately NOT relying on
# file mtime for versioning — this camera has no working RTC (defaults to 2000-01-01 with
# no NTP configured), so any timestamp metadata that ever touches the device side is
# meaningless, and even on the PC side a copy/transfer step could reset it. Baking the
# timestamp into the name at creation time survives any of that. Flashing scripts pick the
# newest firmware by listing ak_rtsp_firmware_*.squashfs and taking the lexicographically
# last match — the YYYYMMDD_HHMMSS format sorts correctly as a plain string.
# Usage: pad_build.sh [path/to/new_app_gzip.squashfs]
# Defaults to ./new_app_gzip.squashfs in the current directory if no path is
# given. Output is written next to the input file.
set -euo pipefail
SRC="${1:-new_app_gzip.squashfs}"
cd "$(dirname "$SRC")"
SRC="$(basename "$SRC")"
TARGET=5242880

TS=$(date +%Y%m%d_%H%M%S)
OUT="ak_rtsp_firmware_${TS}.squashfs"

SIZE=$(stat -c%s "$SRC")
PAD=$((TARGET - SIZE))
echo "orig size=$SIZE pad=$PAD target=$TARGET"

head -c "$PAD" /dev/zero | tr '\0' '\377' > pad.bin
cat "$SRC" pad.bin > "$OUT"
rm -f pad.bin

ls -la "$OUT"
echo "--- tail bytes ---"
tail -c 32 "$OUT" | od -An -tx1
