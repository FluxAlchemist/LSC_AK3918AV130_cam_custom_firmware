#!/bin/sh
# start_rtsp.sh — kill whatever currently holds the ISP/VI/VENC/audio
# devices (anyka_ipc on stock firmware, OR our own ak_rtsp if this is
# already the flashed standalone firmware running ak_rtsp as process_0),
# reload venc modules, run the ak_rtsp binary from the SD card.
# Upload to SD card and run from telnet: sh /mnt/start_rtsp.sh

# Kill the Anyka userspace watchdog SUPERVISOR (/usr/bin/watchdog,
# CWatchdog.cpp) FIRST, before touching anything it supervises. On the
# permanently-flashed standalone firmware, this process's entire job is to
# notice when process_0 (ak_rtsp_wrapper.sh -> ak_rtsp) dies and immediately
# respawn it. Killing ak_rtsp before this (the old order) let the supervisor
# respawn a fresh ak_rtsp_wrapper.sh/ak_rtsp in the gap before the later
# `killall watchdog` line ran -- confirmed via `ps aux` 2026-07-15 showing
# TWO ak_rtsp_wrapper processes (the original boot one plus a respawned one)
# and a fresh ak_rtsp still holding /dev/watchdog/VI/VENC, which is exactly
# why our own watchdog feeder below got "Device or resource busy" and the
# board rebooted out from under the test run. Order now: supervisor dies
# first, so nothing auto-restarts anyka_ipc/ak_rtsp_wrapper.sh/ak_rtsp when
# we kill them next. Both the wrapper's plain name and its busybox
# `{comm}` form are covered; all calls are harmless no-ops on whichever
# firmware this ISN'T running on.
killall -9 watchdog          2>/dev/null
killall -9 ak_rtsp_wrapper.sh 2>/dev/null
killall -9 ak_rtsp_wrapper    2>/dev/null
killall -9 anyka_ipc          2>/dev/null
killall -9 ak_rtsp            2>/dev/null
sleep 2

# Kill any watchdog feeder loop left running from a PREVIOUS invocation of
# this script (found the hard way 2026-07-06: re-running start_rtsp.sh
# without a full reboot in between can leave the old feeder's background
# job alive — nothing in this script ever killed it by name, since it's an
# anonymous `sh` subshell, not matchable via killall. If that old loop then
# stops for any reason (or if BOTH old and new somehow contend for the
# same fd), NOTHING pets /dev/watchdog and the hardware timer resets the
# board on its own schedule — this looked exactly like a crash but wasn't
# one (root-caused after a div0 investigation). PID-file based so this is robust across
# repeated runs in the same boot.
WD_PIDFILE=/tmp/watchdog_feeder.pid
if [ -f "$WD_PIDFILE" ]; then
    OLD_WD_PID=$(cat "$WD_PIDFILE" 2>/dev/null)
    if [ -n "$OLD_WD_PID" ] && kill -0 "$OLD_WD_PID" 2>/dev/null; then
        echo "killing stale watchdog feeder from a previous run (pid $OLD_WD_PID)"
        kill -9 "$OLD_WD_PID" 2>/dev/null
    fi
    rm -f "$WD_PIDFILE"
fi

# Feed the hardware watchdog every 5s in the background.
# Opening /dev/watchdog once for the whole pipeline avoids the "Device or
# resource busy" race that per-iteration redirects cause.
while true; do printf V; sleep 5; done > /dev/watchdog &
WD_PID=$!
echo "$WD_PID" > "$WD_PIDFILE"
echo "watchdog feeder started (pid $WD_PID)"

# Clear dmesg buffer first
dmesg -c > /dev/null

# Reload venc modules to reset AL encoder scheduler and all chn_dev state.
# When anyka_ipc exits it calls AKV_Encoder_Destroy (via venc_adapter_close),
# which nukes the AL scheduler and zeroes the bridge-ops pointer. Simple
# rmmod + insmod is the only reliable way to get a fully clean slate.
rmmod ak_venc_bridge  2>/dev/null
rmmod ak_venc_adapter 2>/dev/null
insmod /usr/modules/ak_venc_adapter.ko || { echo "ERROR: insmod ak_venc_adapter failed"; exit 1; }
insmod /usr/modules/ak_venc_bridge.ko  || { echo "ERROR: insmod ak_venc_bridge failed"; exit 1; }
echo "venc modules reloaded"

# Reload ak_pcm too (2026-07-04) — same stale-kernel-module-state class of bug
# as VENC above, confirmed for audio: a
# killed+relaunched process gets EPERM opening /dev/pcmC0D0c even though a
# clean boot's first open works fine. Needed for real now that ak_rtsp's own
# mic capture (audio.c) depends on this device, not just kept for hygiene.
rmmod ak_pcm 2>/dev/null
insmod /usr/modules/ak_pcm.ko 2>/dev/null && echo "ak_pcm reloaded" || echo "WARNING: insmod ak_pcm failed"

# IR cut GPIO init is now handled inside ak_rtsp (ircut_init() in ircut.c).
# It fires the proper reset pulse (night→day) with the correct two-phase solenoid
# drive, matching anyka_ipc's ht_night_mode_init sequence.
# Clear the per-boot flag so ak_rtsp always fires the reset pulse on this run.
rm -f /tmp/ircut_has_reset_flag

# Set up dynamic debug if debugfs is compiled in
mount -t debugfs none /sys/kernel/debug 2>/dev/null
if [ -f /sys/kernel/debug/dynamic_debug/control ]; then
    echo "module ak_venc_adapter +p" > /sys/kernel/debug/dynamic_debug/control
    echo "module ak_venc_bridge +p" > /sys/kernel/debug/dynamic_debug/control
    echo "venc drivers dynamic debug enabled"
fi

# Continuously flush dmesg to the SD card WHILE ak_rtsp runs, not just at
# exit (added 2026-07-04 after a real hard reboot during audio testing lost
# the in-RAM dmesg buffer entirely — the exit-time `dmesg > ...` line below
# never got a chance to run because the board reset before ak_rtsp's process
# ever "exited" in the normal sense). `dmesg -c` clears the ring buffer after
# reading, so each 2s tick only appends what's new — cheap, and survives a
# hard reset since it's already been fsync'd to the SD card by the time any
# given crash happens (worst case losing <2s of kernel log, not the whole
# session). Killed automatically when the shell exits (it's this script's
# own background job).
> /mnt/ak_rtsp_dmesg_live.log
(while true; do dmesg -c >> /mnt/ak_rtsp_dmesg_live.log; sync; sleep 2; done) &
DMESG_TAIL_PID=$!
echo "Live dmesg tail started (pid $DMESG_TAIL_PID) -> /mnt/ak_rtsp_dmesg_live.log"

# Run RTSP server, tee its stdout/stderr live to the SD card, and capture
# dmesg on exit/crash. The tee (not just a plain redirect) matters: if
# something crashes the KERNEL hard enough to reboot the whole board (not
# just kill the ak_rtsp process), telnet drops immediately and everything
# below "RET=$?" never runs — dmesg is also gone, wiped by the reboot before
# it could be saved. main.c's control_start() already line-buffers stdout
# (see control.c), so every log line is written to /mnt/ak_rtsp_app.log via
# a plain write() as soon as it's printed, not just buffered in the telnet
# session — that's what survives on the SD card across the reboot. Cleared
# at the start of each run so it doesn't keep growing across repeated tests.
> /mnt/ak_rtsp_app.log
echo "Starting ak_rtsp (log: /mnt/ak_rtsp_app.log)..."
/mnt/ak_rtsp 2>&1 | tee -a /mnt/ak_rtsp_app.log
RET=$?

kill $DMESG_TAIL_PID 2>/dev/null
echo "ak_rtsp exited with status $RET"
dmesg > /mnt/ak_rtsp_dmesg.log
echo "dmesg saved to /mnt/ak_rtsp_dmesg.log (full final dump)"
echo "Live dmesg tail (survives a hard reset): /mnt/ak_rtsp_dmesg_live.log"

