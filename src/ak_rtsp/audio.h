#pragma once
#include <stdint.h>
#include <stddef.h>

/* Mic capture via /dev/pcmC0D0c, built from ground-truth ioctl captures
 * of the stock firmware's own audio session (see the AUDIO_PERIOD_SAMPLES
 * comment below for the period-size correction that came out of that).
 * Capture only: no playback, no AEC/loopback.
 *
 * Runs its own background thread (matches ae.c's pattern) because
 * /dev/pcmC0D0c's read() blocks for a full period — calling it inline from
 * the RTSP client loop would stall video frame delivery by that much on
 * every audio read. The capture thread pushes completed periods into a
 * small ring buffer; audio_get_frame() drains it non-blockingly from the
 * client loop, same shape as how VENC's GET_STREAM is polled for video. */

#define AUDIO_SAMPLE_RATE    8000
#define AUDIO_CHANNELS       1
#define AUDIO_BITS           16
/* 256, NOT 1024: §11's original GET_PARS capture mislabeled which fd was
 * capture vs playback (cross-referenced by thread name, not path) and
 * reported captures's period_size as 1024. §23's path-tagged capture (fd
 * tagged directly from its open() path, authoritative) shows capture's real
 * GET_PARS period_size is 512 BYTES = 256 samples at 16-bit — confirmed by
 * matching real read() call sizes exactly (every real capture read() is
 * exactly 512 bytes, never any other size, across 1442 consecutive calls).
 * Requesting the wrong read() size (2048 bytes, from the old 1024-sample
 * constant) is a strong candidate for part of why our own read() got EPERM
 */
#define AUDIO_PERIOD_SAMPLES 256
#define AUDIO_PERIOD_BYTES   (AUDIO_PERIOD_SAMPLES * (AUDIO_BITS / 8) * AUDIO_CHANNELS)

/* Opens the device, replays the confirmed config ioctl sequence, and starts
 * the background capture thread. Returns 0 on success, -1 on failure
 * (non-fatal to the caller — video should keep working even if audio init
 * fails). */
int audio_start(void);

void audio_stop(void);

/* Non-blocking. Copies the oldest not-yet-consumed period into buf (up to
 * max_samples samples) and writes the sample count to *out_samples.
 * Returns 1 if a frame was copied, 0 if none is available yet, -1 if audio
 * isn't running. */
int audio_get_frame(int16_t *buf, size_t max_samples, size_t *out_samples);
