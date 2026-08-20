#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <pthread.h>

#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "audio.h"

static int g_audio_fd = -1;

/* Opened alongside capture. A prior debugging session
 * found the likely reason SET_MODE(0) crashed the kernel on this fd in
 * every earlier session: a fresh, path-tagged ground-truth capture (isp_hook.c
 * extended with open()/read()/write() hooks) shows real anyka_ipc opens this
 * exact device O_WRONLY (flags=0x1), not O_RDWR like this code used to —
 * and calls the identical SET_MODE(0) successfully there. Switched the open()
 * call below to O_WRONLY and re-added SET_MODE(0) to test that theory
 * directly. If it still crashes, that disproves the open-mode theory (still
 * useful — narrows the search further) rather than being a regression to
 * avoid. */
static int g_audio_playback_fd = -1;
static int g_audio_loopback_fd = -1;

static volatile int g_playback_running = 0;
static volatile int g_playback_thread_started = 0;
static pthread_t    g_playback_tid;

static volatile int g_audio_running = 0;
static pthread_t    g_audio_tid;

static void audio_close_aux(void); /* closes g_audio_playback_fd/g_audio_loopback_fd */

#define AUDIO_RING_SLOTS 8

struct audio_slot {
    int16_t samples[AUDIO_PERIOD_SAMPLES];
    size_t  n_samples;
};

static struct audio_slot g_ring[AUDIO_RING_SLOTS];
static volatile uint32_t g_ring_write = 0; /* total periods produced */
static volatile uint32_t g_ring_read  = 0; /* total periods consumed */

/* Reports exactly which step failed (added after a real hardware run came
 * back with a bare "audio_init: config ioctl: Not a tty" (ENOTTY) and no
 * way to tell which of the 9 calls it was — turned out to be GET_CAPS
 * specifically). */
#define AUDIO_IOC(step_name, expr) \
    do { \
        if ((expr) < 0) { \
            fprintf(stderr, "audio_init: step '%s' failed: %s (errno=%d)\n", \
                    step_name, strerror(errno), errno); \
            goto fail; \
        } \
    } while (0)

/* Same as AUDIO_IOC but non-fatal — for calls confirmed to be pure
 * informational reads in the real capture (their result was never fed into
 * any later SET call, only logged). GET_CAPS (nr=0x81) failed with ENOTTY
 * on a real hardware test of a capture-only client — best-supported theory
 * is it needs some cross-stream precondition only met when anyka_ipc's
 * playback+capture+loopback streams are all open together, which a
 * capture-only client like this one has no reason to replicate. Applied to
 * the other pure-GET informational
 * calls too on the same reasoning, since they were never load-bearing. */
#define AUDIO_IOC_WARN(step_name, expr) \
    do { \
        if ((expr) < 0) { \
            fprintf(stderr, "audio_init: step '%s' failed (non-fatal): %s (errno=%d)\n", \
                    step_name, strerror(errno), errno); \
        } \
    } while (0)

/* Hard-capped retry count for both the capture and playback streaming loops,
 * added 2026-07-04 after a real hardware test: with read() returning EPERM
 * immediately (not blocking), the previous unbounded "retry every 10ms
 * forever" loop hammered the driver at ~100Hz with no backoff, and the
 * camera hard-rebooted partway through that run. A capture/playback loop must never retry
 * a failing syscall indefinitely at that rate — give up after a handful of
 * consecutive failures and stop cleanly instead. */
#define AUDIO_MAX_CONSECUTIVE_FAILS 5

/* Configures the playback fd matching the real, path-tagged ground-truth
 * order: SET_STATE(0) -> SET_MODE(0) ->
 * SET_RATE(8000) -> SET_PERIODS(4) -> SET_STATE(4). SET_MODE(0) is skipped
 * below — confirmed to still crash the kernel even with the
 * O_WRONLY open fix, same signature every time. Root cause is
 * unresolved (unlike capture_ioctl's dispatch, playback_ioctl's real
 * dispatch hasn't been decompiled/read yet — that would be the next Ghidra
 * step if playback support is ever needed). Skipped for now since playback
 * isn't required for the actual goal (capture) and this crash was
 * repeatedly blocking us from observing capture's own read() result. All
 * steps here are non-fatal (AUDIO_IOC_WARN) — this is a best-effort peer
 * stream for capture's benefit, not something we depend on ourselves. */
static void audio_configure_playback(void)
{
    if (g_audio_playback_fd < 0) return;

    int32_t v;

    v = 0;
    AUDIO_IOC_WARN("playback SET_STATE(0)", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_STATE, &v));

    v = AUDIO_SAMPLE_RATE;
    AUDIO_IOC_WARN("playback SET_RATE", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_RATE, &v));

    v = 4;
    AUDIO_IOC_WARN("playback SET_PERIODS(4)", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_PERIODS, &v));

    v = 4;
    AUDIO_IOC_WARN("playback SET_STATE(4)", ioctl(g_audio_playback_fd, AKPCM_IOC_SET_STATE, &v));

    printf("[ak_rtsp] audio: %s configured+started (%dHz/%dch/%dbit)\n",
           DEV_PCM_PLAYBACK, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS);
}

/* Keeps the playback fd actively streaming for the duration of capture —
 * the real test of a theory:
 * whether capture's read() EPERM needs a sibling stream genuinely running,
 * not merely open/configured. Writes silence; audible output is a non-issue
 * since the speaker path is out of scope for this project. Same
 * retry-cap/backoff discipline as the capture loop. */
static void *audio_playback_loop(void *arg)
{
    (void)arg;

    int16_t silence[AUDIO_PERIOD_SAMPLES];
    memset(silence, 0, sizeof(silence));

    int consecutive_fails = 0;
    while (g_playback_running) {
        ssize_t n = write(g_audio_playback_fd, silence, sizeof(silence));
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[audio] playback write");
            if (++consecutive_fails >= AUDIO_MAX_CONSECUTIVE_FAILS) {
                fprintf(stderr,
                        "[audio] playback write failed %d times in a row — "
                        "stopping playback stream thread\n", consecutive_fails);
                g_playback_running = 0;
                break;
            }
            usleep(200000);
            continue;
        }
        consecutive_fails = 0;
    }
    return NULL;
}

/* Config sequence replayed byte-for-byte from a real, path-tagged anyka_ipc
 * capture session — superseding an earlier
 * sequence, which had the wrong SET_STATE/SET_PERIODS values and was
 * missing RESET_BUF entirely, due to a since-corrected fd mislabeling bug in
 * the original capture method). Several ioctl semantics are still
 * unconfirmed (see ak_ioctls.h comments) — this deliberately replays the
 * exact same calls/values in the exact same order rather than trying to
 * "clean up" a sequence we don't fully understand yet.
 *
 * Split from playback config: the real
 * capture shows playback's SET_MODE(0) succeeds while capture is ALREADY
 * ACTIVELY READING (interleaved reads visible in the log between playback's
 * config ioctls), not merely after capture's config ioctls returned success.
 * An earlier "configure capture fully, then playback" reorder crashed on
 * SET_MODE again — this function now only opens+configures the capture fd;
 * the caller (audio_start()) starts the capture read thread before calling
 * audio_init_playback(), so playback configures while capture is genuinely
 * mid-stream, matching the real timing as closely as this code can. */
static int audio_init_capture(void)
{
    g_audio_fd = open(DEV_PCM_CAPTURE, O_RDONLY);
    if (g_audio_fd < 0) {
        perror("audio_init: open " DEV_PCM_CAPTURE);
        return -1;
    }

    int32_t v;
    struct akpcm_pars pars;

    /* Real order (ground truth): SET_STATE(2) -> GET_PERIODS -> GET_PARS
     * -> RESET_BUF(-1) -> SET_MODE(0) -> SET_RATE(8000) -> SET_STATE(2) again
     * -> SET_PERIODS(2). Note SET_STATE's value here is 2, not 0/4 as an
     * earlier attempt incorrectly assumed — replay the exact value, its
     * meaning is still unconfirmed. */
    v = 2;
    AUDIO_IOC("SET_STATE(2)", ioctl(g_audio_fd, AKPCM_IOC_SET_STATE, &v));

    AUDIO_IOC_WARN("GET_PERIODS#1", ioctl(g_audio_fd, AKPCM_IOC_GET_PERIODS, &v));

    /* GET_PARS is misnamed — a Ghidra relocation-fix session
     * decompiled ak_pcm.ko's real capture_ioctl dispatch and found this exact
     * ioctl (req=0x401c50e0) does a copy_FROM_user of this 28-byte struct into the
     * kernel, validates rate!=0, period_size!=0 && period_size%64==0, and
     * period_count in [1,0x50], computes the hardware rate divisor, and ONLY ON
     * SUCCESS sets the ready bit at +0x304 that capture_read() requires (bit 0x2).
     * Every previous session called this with a zeroed struct (treating
     * it as a pure informational GET) — that fails validation every time
     * (rate=0), which is exactly the observed EINVAL, and is why the ready bit
     * was never set regardless of what SET_MODE/SET_RATE/SET_STATE/SET_PERIODS
     * did. Fill it with the real captured values instead. */
    memset(&pars, 0, sizeof(pars));
    pars.format       = 0;
    pars.rate         = AUDIO_SAMPLE_RATE;
    pars.channels     = AUDIO_CHANNELS;
    pars.bits         = AUDIO_BITS;
    pars.period_size  = AUDIO_PERIOD_BYTES;
    pars.period_count = 16;
    AUDIO_IOC("GET_PARS (commit params)", ioctl(g_audio_fd, AKPCM_IOC_GET_PARS, &pars));

    /* dir=NONE — value goes directly in the ioctl request, not via pointer.
     * Real capture calls this with arg=-1, not NULL/0 (see ak_ioctls.h) —
     * previously entirely missing from this sequence, a leading candidate
     * for (part of) the actual EPERM fix. */
    AUDIO_IOC("RESET_BUF(-1)", ioctl(g_audio_fd, AKPCM_IOC_RESET_BUF, (void *)(long)-1));

    /* dir=NONE — value goes directly in the ioctl request, not via pointer */
    AUDIO_IOC("SET_MODE(0)", ioctl(g_audio_fd, AKPCM_IOC_SET_MODE, 0));

    v = AUDIO_SAMPLE_RATE;
    AUDIO_IOC("SET_RATE(8000)", ioctl(g_audio_fd, AKPCM_IOC_SET_RATE, &v));

    v = 2;
    AUDIO_IOC("SET_STATE(2)#2", ioctl(g_audio_fd, AKPCM_IOC_SET_STATE, &v));

    v = 2;
    AUDIO_IOC("SET_PERIODS(2)", ioctl(g_audio_fd, AKPCM_IOC_SET_PERIODS, &v));

    printf("[ak_rtsp] audio: %s opened, capture configured (%dHz/%dch/%dbit, "
           "period=%d samples)\n",
           DEV_PCM_CAPTURE, AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS,
           AUDIO_PERIOD_SAMPLES);
    return 0;

fail:
    close(g_audio_fd);
    g_audio_fd = -1;
    return -1;
}

/* Opens+configures playback/loopback — called AFTER the capture read thread
 * is already running (see audio_start() and the comment above
 * audio_init_capture()). Best-effort: not fatal if these fail, capture is
 * already working by this point regardless. O_WRONLY for playback matches
 * real anyka_ipc's own open() flags — was O_RDWR in every earlier
 * session. */
static void audio_init_playback(void)
{
    g_audio_playback_fd = open(DEV_PCM_PLAYBACK, O_WRONLY);
    if (g_audio_playback_fd < 0)
        fprintf(stderr, "audio_init: open %s failed (non-fatal): %s\n",
                DEV_PCM_PLAYBACK, strerror(errno));
    else
        audio_configure_playback();
    g_audio_loopback_fd = open(DEV_PCM_LOOPBACK, O_RDWR);
    if (g_audio_loopback_fd < 0)
        fprintf(stderr, "audio_init: open %s failed (non-fatal): %s\n",
                DEV_PCM_LOOPBACK, strerror(errno));
    printf("[ak_rtsp] audio: playback_fd=%d (configured+started) loopback_fd=%d "
           "(held open, unconfigured)\n",
           g_audio_playback_fd, g_audio_loopback_fd);
}

static void audio_close_aux(void)
{
    if (g_audio_playback_fd >= 0) { close(g_audio_playback_fd); g_audio_playback_fd = -1; }
    if (g_audio_loopback_fd >= 0) { close(g_audio_loopback_fd); g_audio_loopback_fd = -1; }
}

static void audio_close(void)
{
    if (g_audio_fd >= 0) {
        close(g_audio_fd);
        g_audio_fd = -1;
    }
    audio_close_aux();
}

static void *audio_capture_loop(void *arg)
{
    (void)arg;

    int consecutive_fails = 0;
    while (g_audio_running) {
        struct audio_slot *slot = &g_ring[g_ring_write % AUDIO_RING_SLOTS];
        ssize_t n = read(g_audio_fd, slot->samples, sizeof(slot->samples));
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[audio] read");
            if (++consecutive_fails >= AUDIO_MAX_CONSECUTIVE_FAILS) {
                fprintf(stderr,
                        "[audio] read failed %d times in a row — stopping capture "
                        "thread (not retrying further)\n",
                        consecutive_fails);
                g_audio_running = 0; /* so audio_get_frame() reports "not running" immediately */
                break;
            }
            usleep(200000); /* 200ms backoff, not 10ms — this is a slow retry, not a poll */
            continue;
        }
        consecutive_fails = 0;
        if (n == 0) {
            usleep(10000);
            continue;
        }
        slot->n_samples = (size_t)n / sizeof(int16_t);
        g_ring_write++;
    }
    return NULL;
}

/* Tracks whether pthread_create succeeded, independent of g_audio_running
 * (which the capture thread itself may clear on a fatal read() streak — see
 * audio_capture_loop). Needed so audio_stop() always joins a thread that was
 * actually started, even if it already exited on its own. */
static volatile int g_audio_thread_started = 0;

int audio_start(void)
{
    if (audio_init_capture() < 0) return -1;

    g_ring_write = 0;
    g_ring_read  = 0;
    g_audio_running = 1;

    if (pthread_create(&g_audio_tid, NULL, audio_capture_loop, NULL) != 0) {
        perror("[audio] pthread_create");
        g_audio_running = 0;
        audio_close();
        return -1;
    }
    g_audio_thread_started = 1;

    /* Deliberately started only now — see the comment above
     * audio_init_capture(): real anyka_ipc's playback SET_MODE succeeds
     * while capture is already mid-read, not merely after capture's config
     * ioctls returned success. */
    audio_init_playback();

    if (g_audio_playback_fd >= 0) {
        g_playback_running = 1;
        if (pthread_create(&g_playback_tid, NULL, audio_playback_loop, NULL) != 0) {
            perror("[audio] playback pthread_create");
            g_playback_running = 0;
        } else {
            g_playback_thread_started = 1;
        }
    }

    return 0;
}

void audio_stop(void)
{
    g_playback_running = 0;
    if (g_playback_thread_started) {
        pthread_join(g_playback_tid, NULL);
        g_playback_thread_started = 0;
    }

    g_audio_running = 0;
    if (g_audio_thread_started) {
        pthread_join(g_audio_tid, NULL);
        g_audio_thread_started = 0;
    }
    audio_close();
}

int audio_get_frame(int16_t *buf, size_t max_samples, size_t *out_samples)
{
    if (!g_audio_running) return -1;
    if (g_ring_read >= g_ring_write) return 0;

    /* Producer lapped us — drop the oldest slots we can no longer trust. */
    if (g_ring_write - g_ring_read > AUDIO_RING_SLOTS)
        g_ring_read = g_ring_write - AUDIO_RING_SLOTS;

    struct audio_slot *slot = &g_ring[g_ring_read % AUDIO_RING_SLOTS];
    size_t n = slot->n_samples;
    if (n > max_samples) n = max_samples;
    memcpy(buf, slot->samples, n * sizeof(int16_t));
    *out_samples = n;
    g_ring_read++;
    return 1;
}
