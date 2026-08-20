#!/bin/sh
# ak_rtsp_wrapper.sh — process_0's actual entry point in the de-Tuyafied
# firmware (see _ht_process.ini in this same directory). Runs as a direct
# child of the stock /usr/bin/watchdog, exactly where anyka_ipc used to run,
# on a clean boot where sensor_driver.sh has already insmod'd ak_isp/
# sensor_gc20c3/ak_venc_adapter/ak_venc_bridge/ak_pcm and copied
# /tmp/sensor_isp.conf.
#
# See docs/building_your_own_firmware.md for how this fits into the patched
# APP squashfs tree.
PATH=$PATH:/bin:/sbin:/usr/bin:/usr/sbin

# --- Manual WiFi bring-up ---
# ethernet.sh's wifi_driver.sh install() deliberately SKIPS starting
# wpa_supplicant and assigning an IP whenever /tmp/_ak39_factory.ini exists
# (its own factory/production-test gate). If you keep _ak39_factory.ini on
# the SD card (e.g. as a quick fallback path to stock factory-demo RTSP),
# ethernet.sh will skip WiFi bring-up entirely — so we do it ourselves here
# instead. Plain DHCP only. Guarded by a running-check so a watchdog respawn
# of this script after a crash doesn't spawn a second wpa_supplicant/udhcpc.
WPA_CONF=/etc/config/wpa_supplicant.conf
if [ -f "$WPA_CONF" ] && ! ps | grep -v grep | grep -q wpa_supplicant; then
    echo "ak_rtsp_wrapper: starting wpa_supplicant + udhcpc"
    wpa_supplicant -Dnl80211 -i wlan0 -c "$WPA_CONF" -B
    udhcpc -i wlan0 -x hostname:IPCamera &
    touch /tmp/connect_type_wifi
fi

/usr/bin/ak_rtsp
RET=$?
exit $RET
