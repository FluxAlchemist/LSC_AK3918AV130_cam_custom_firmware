#!/bin/bash
# Assembles the patched APP squashfs tree and builds the flashable, padded
# firmware image. Invoked by `make firmware` (via WSL, for mksquashfs/
# unsquashfs) after ak_rtsp itself has been rebuilt natively on Windows.
#
# This script does NOT ship a stock APP partition dump -- that's your
# camera's proprietary Tuya firmware and can't be redistributed. You must
# supply your own unsquashed dump as app_extracted/ (see
# docs/building_your_own_firmware.md for how to get one). This script:
#   1. copies app_extracted/ to app_patched/ (skip if app_patched/ already
#      exists -- so re-runs don't clobber manual edits you've made to it)
#   2. applies the de-Tuyafication patch (deletes anyka_ipc, installs
#      ak_rtsp_wrapper.sh + ak_rtsp, points _ht_process.ini's process_0 at
#      the wrapper) using the templates in
#      ../../tools/firmware_patch_templates/
#   3. rebuilds the squashfs, round-trip verifies it, and pads it to the
#      exact partition size via pad_build.sh
#
# Env vars (all optional):
#   APP_EXTRACTED  - path to your unsquashed stock APP dump (default: ./app_extracted)
#   FW_DIR         - output directory (default: this script's directory)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEMPLATES="$SCRIPT_DIR/../../tools/firmware_patch_templates"

APP_EXTRACTED="${APP_EXTRACTED:-$SCRIPT_DIR/app_extracted}"
FW_DIR="${FW_DIR:-$SCRIPT_DIR}"
PATCHED="$FW_DIR/app_patched"
BINARY="$SCRIPT_DIR/ak_rtsp"

if [ ! -d "$APP_EXTRACTED" ]; then
    echo "FATAL: $APP_EXTRACTED not found." >&2
    echo "This must be your own unsquashed dump of the camera's stock APP" >&2
    echo "partition (mtdblock7) -- it is NOT included in this repo since it's" >&2
    echo "the vendor's proprietary firmware. See docs/building_your_own_firmware.md" >&2
    echo "for how to dump and unsquash your own, or set APP_EXTRACTED to point" >&2
    echo "at an existing extraction." >&2
    exit 1
fi

mkdir -p "$FW_DIR"

if [ ! -d "$PATCHED" ]; then
    echo "Creating app_patched/ from $APP_EXTRACTED..."
    cp -a "$APP_EXTRACTED" "$PATCHED"

    echo "Applying de-Tuyafication patch..."
    rm -f "$PATCHED/bin/anyka_ipc"
    cp "$TEMPLATES/ak_rtsp_wrapper.sh" "$PATCHED/sbin/ak_rtsp_wrapper.sh"
    chmod +x "$PATCHED/sbin/ak_rtsp_wrapper.sh"
    cp "$TEMPLATES/_ht_process.ini" "$PATCHED/local/_ht_process.ini"
else
    echo "app_patched/ already exists, re-using it (edit it directly if you need" \
         "to change anything besides the ak_rtsp binary)."
fi

echo "Copying freshly built ak_rtsp binary into app_patched/bin/..."
cp "$BINARY" "$PATCHED/bin/ak_rtsp"

cd "$FW_DIR"
rm -f new_app_gzip.squashfs
mksquashfs app_patched new_app_gzip.squashfs -comp gzip -b 131072 -all-root -noappend

echo "Round-trip verifying..."
rm -rf /tmp/rt_check
unsquashfs -d /tmp/rt_check new_app_gzip.squashfs >/dev/null
if ! diff <(cd app_patched && find . -type f -exec md5sum {} \; | sort) \
          <(cd /tmp/rt_check && find . -type f -exec md5sum {} \; | sort); then
    echo "FATAL: round-trip verification mismatch, aborting." >&2
    exit 1
fi
rm -rf /tmp/rt_check
echo "Round-trip verify OK."

bash "$SCRIPT_DIR/../../tools/flash_scripts/pad_build.sh" "$FW_DIR/new_app_gzip.squashfs"
