# Firmware Patch Templates

These two files are the actual patch applied on top of your own stock `app_extracted/`
dump to produce the de-Tuyafied `app_patched/` tree. Used automatically by
`src/ak_rtsp/build_firmware.sh` — see `docs/building_your_own_firmware.md` for the
full walkthrough. Listed here individually so the diff against stock is easy to read.

## `_ht_process.ini` → `app_patched/local/_ht_process.ini`

Stock's version has three process entries:

```ini
[process_0]
name = anyka_ipc
...
[process_1]
name = rtspserver
...
[process_2]
name = nvtservice
...
```

This template reduces it to a single entry that launches `ak_rtsp_wrapper.sh` instead
of `anyka_ipc`. `rtspserver`/`nvtservice` are dropped entirely — they're already inert
on stock firmware (gated behind `onvif_switch = 0` in `_ht_sw_settings.ini`, which is
never flipped on), so removing their process entries changes nothing at runtime.

**Do NOT** touch `cmdserver`/`msgserver` — they are not process-list entries (they're
started directly from `service.sh`) and are pure local IPC, not Tuya cloud clients.
Removing them causes a boot stall. See `docs/flash_partition_map_and_rootfs.md`.

## `ak_rtsp_wrapper.sh` → `app_patched/sbin/ak_rtsp_wrapper.sh`

`process_0`'s actual entry point. Brings up WiFi manually (DHCP) before exec'ing
`/usr/bin/ak_rtsp`, since `ethernet.sh` skips WiFi bring-up whenever
`/tmp/_ak39_factory.ini` is present (its own factory/production-test gate) — relevant
if you keep an `_ak39_factory.ini` on the SD card as a fallback path back to stock
factory-demo RTSP mode.
