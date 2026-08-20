#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "ak_ioctls.h"
#include "isp.h"
#include "ae.h"

/* -------------------------------------------------------------------------
 * AE init — kernel-driven, ticked from userspace.
 *
 * Root cause of the oscillation we saw with a userspace P-controller:
 * ground-truth isp_hook captures of stock anyka_ipc (ExtractedData/ae_dump*.log)
 * show it calls SET_EXPOSURE exactly ONCE at startup (priv[exp=2 type=1]) and
 * NEVER calls AE_SUSPEND. It then just polls GET_EXPOSURE/GET_AE_RUN_INFO at
 * ~15Hz forever and lets the ISP kernel's own AE state machine converge —
 * smoothly and monotonically every time (e.g. lum 25->43->47->48->49,
 * hw_exp 648->1297->1442->1442->1442, gain flat 1.0x, no overshoot).
 *
 * Our previous approach called AE_SUSPEND(1) to kill that already-tuned
 * kernel algorithm and replaced it with a from-scratch 0.67x/1.5x ratio
 * P-controller with no delay compensation or filtering — a worse
 * reimplementation of something the hardware already does correctly. That
 * mismatch (not a second competing control loop) produced the strobe/
 * oscillation. Fix: don't suspend AE, don't write our own exposure values —
 * just tick GET_EXPOSURE like anyka_ipc does and let the on-chip AE run.
 *
 * AWB_EX struct size: 5124 bytes (0x1404), confirmed by Ghidra from
 * Isp_Struct_len table. isp_reapply_awb_ex() reads the correct payload from
 * sensor_isp.conf (module 0x13, offset +0x7e4) — see isp.c.
 *
 * WB struct: 0x7e0 bytes, layout confirmed by Ghidra (ak_vpss_set_mwb_attr).
 *   bytes  8-11: wb_type=1 (MANUAL)
 *   bytes 12-13: R_gain u16 (256=1x) = 460 (1.80x)
 *   bytes 14-15: G_gain u16 (256=1x) = 256 (reference)
 *   bytes 16-17: B_gain u16 (256=1x) = 504 (1.97x)
 * ------------------------------------------------------------------------- */

#define AE_TICK_US        66000 /* ~15Hz — matches anyka_ipc's GET_EXPOSURE tick rate */
#define AE_WINDOW_TICKS       15 /* ~1s window (15 ticks @ ~66ms) for min/max/peak stats */
#define AE_EXP_MAX        32767 /* exp_time_max ceiling (isp_pipeline_init default is a broken 2) */

static volatile int g_ae_running;
static pthread_t    g_ae_tid;

/* Latest AE sample, updated every tick — read by night.c's day/night monitor
 * so it doesn't need its own ISP polling loop. -1 lum = no sample yet. */
static volatile int      g_last_lum      = -1;
static volatile uint32_t g_last_hw_exp   = 0;
static volatile uint32_t g_last_hw_sgain = 0;
static volatile uint32_t g_last_hw_isp   = 0;

int ae_get_last_stats(int *lum, uint32_t *hw_exp, uint32_t *hw_sgain, uint32_t *hw_isp)
{
    if (g_last_lum < 0) return -1;
    *lum      = g_last_lum;
    *hw_exp   = g_last_hw_exp;
    *hw_sgain = g_last_hw_sgain;
    *hw_isp   = g_last_hw_isp;
    return 0;
}

void ae_report_stats(int lum, uint32_t hw_exp, uint32_t hw_sgain, uint32_t hw_isp)
{
    g_last_lum      = lum;
    g_last_hw_exp   = hw_exp;
    g_last_hw_sgain = hw_sgain;
    g_last_hw_isp   = hw_isp;
}

/* Live-tunable AE parameters, exposed for control.c. Plain (non-volatile)
 * struct: written wholesale by ae_set_tuning() (control thread) and read
 * wholesale by ae_loop()/ae_get_tuning() (AE thread). A torn read is possible
 * in theory but harmless here — this only affects a human-driven debug/tuning
 * path, not correctness-critical control flow, so a mutex isn't worth the
 * complexity (matches this codebase's existing volatile-globals-no-mutex
 * convention for AE/night state). */
/* Verified-working values from a live Camera Tuning tab session (2026-07-15)
 * -- confirmed stable on real hardware via the serial_console_dotnet app's
 * AE sliders, baked in as the new compile-time defaults for this deployment
 * (previously stable_range=hold_range=speed=10, exp_max=AE_EXP_MAX/32767). */
static ae_tuning_t g_tuning = { .stable_range = 4, .hold_range = 4, .speed = 7,
                                 .exp_max = 1039, .enabled = 1 };

void ae_get_tuning(ae_tuning_t *out) { *out = g_tuning; }

int ae_set_tuning(const ae_tuning_t *in)
{
    g_tuning = *in;

    static uint8_t expo[0x720];
    memset(expo, 0, sizeof(expo));
    if (isp_tunnel(ISP_INNER_GET_EXPOSURE, expo) != 0) return -1;
    uint32_t *pub = (uint32_t *)(expo + 0x1c);
    pub[1]  = g_tuning.exp_max;
    pub[9]  = g_tuning.stable_range;
    pub[10] = g_tuning.hold_range;
    pub[11] = g_tuning.speed;
    return isp_tunnel(ISP_INNER_SET_EXPOSURE, expo);
}

/* One-time ISP parameter setup (exposure, WB, frame-rate/sensor-fps, AWB_EX,
 * 3D NR ref). MUST be called before vi_set_channel_attr()/vi_start_capture()
 * (i.e. before STREAMON), not just "early" relative to venc bind.
 *
 * Verified via anyka_ipc's own strace capture (2026-07-14,
 * ExtractedData/strace_ioctl_anyka_ipc_combined_capture/): stock issues
 * every one of these ISP tunnel calls (including SET_FRAME_RATE/
 * SET_SENSOR_FPS) BEFORE it ever touches a VI channel (crop/S_FMT), let
 * alone STREAMON. ak_rtsp used to call this from ae_start() — after
 * vi_start_capture() — which left the ISP driver's per-channel cached-fps
 * field (camera_device+0x40 / the gate flag at +0x44 in ak_isp.ko) still at
 * its zeroed default when the first VI frame interrupt landed, hitting a
 * `1000/0` in the closed-source driver's _send_frame_or_slice_to_bridge and
 * ak_vb2_start_streaming ("Division by zero in kernel." dmesg spam).
 * Matching stock's ordering closes the race
 * at the source instead of patching around it. */
int ae_init_isp_params(void)
{
    static uint8_t expo[0x720];
    memset(expo, 0, sizeof(expo));
    if (isp_tunnel(ISP_INNER_GET_EXPOSURE, expo) != 0) {
        fprintf(stderr, "[ae] get_exposure_attr failed\n");
        return -1;
    }
    uint32_t *priv = (uint32_t *)expo;
    uint32_t *pub  = (uint32_t *)(expo + 0x1c);
    printf("[ae] init: priv[exp=%u type=%u sgain=%u] pub[emax=%u sgain_max=%u]\n",
           priv[1], priv[2], priv[3], pub[1], pub[3]);
    /* pub[8..12] = exp_step, exp_stable_range, exp_hold_range, exp_speed,
     * anti_flicker_target_lumi. Never touched before
     * now — log the raw sensor_isp.conf defaults so we know what we're
     * overriding below. */
    printf("[ae] init: pub[step=%u stable_range=%u hold_range=%u speed=%u "
           "anti_flicker_lumi=%u] (raw defaults from sensor_isp.conf)\n",
           pub[8], pub[9], pub[10], pub[11], pub[12]);

    /* Only fix exp_time_max (pub[1], was a broken 2 from isp_pipeline_init).
     * Leave sensor_gain_max/isp_gain_max/dgain_max (pub[3]/pub[5]/pub[7]) and
     * all of priv[] completely untouched — anyka_ipc's own ae_dump captures
     * show its single startup SET_EXPOSURE call writes these back COMPLETELY
     * UNCHANGED (pub[emax=2 smax=1.0x imax=1.5x] for the entire session) and
     * still reaches hw_exp=1442 with smooth convergence. An earlier attempt
     * widened sensor_gain_max to 4096 (16x) and isp_gain_max to 512 (2x)
     * thinking the defaults were a bug that capped indoor brightness — but
     * that extra gain headroom (never present in stock anyka_ipc) is the
     * likely cause of the "hand in front of lens" flicker: a sudden
     * brightness step lets the on-chip AE swing into high gain and hunt
     * before settling, an excursion the tighter stock ceiling never allows.
     * That was NOT sufficient — the on-chip AE still bistably flip-flops
     * between exactly two hw_exp values (e.g. 256 <-> 4815) at ~3-5Hz on a
     * high-contrast scene (bright window + dim room), even with the gain
     * ceiling back to stock. That's a dead-band/hysteresis problem in the
     * AE algorithm's own decision logic, not a gain-headroom problem: the
     * algorithm keeps re-deciding between two operating points instead of
     * settling once within tolerance. Widen exp_stable_range (dead-band)
     * and exp_hold_range, and reduce exp_speed (convergence aggressiveness)
     * so the on-chip algorithm settles instead of re-triggering every tick.
     * First attempt (stable_range=hold_range=25, speed=10) confirmed the
     * driver accepts these writes (readback matched) and killed the
     * sustained bistable hunting — but overshot: with a dead-band that wide,
     * lum=29 (near the exposure floor, hw_exp=256) already fell within
     * tolerance of the target and the algorithm never bothered climbing
     * further, so it stayed pinned at the floor the whole session — visibly
     * darker than anyka_ipc's ~47-50 lum target. Narrowing the dead-band
     * back down while keeping exp_speed slow (that seemed to be doing the
     * real work suppressing the limit-cycle, not the dead-band width alone)
     * to find a point that both reaches the real target AND doesn't hunt.
     *
     * These are now live-tunable via control.c (see ae_get_tuning()/
     * ae_set_tuning() above) instead of requiring a recompile to iterate —
     * g_tuning's compile-time defaults below are just the starting point. */
    if (ae_set_tuning(&g_tuning) != 0) {
        fprintf(stderr, "[ae] set_exposure_attr failed\n");
        return -1;
    }

    /* Read back to confirm the driver actually accepted these values —
     * some ISP fields silently clamp/reject out-of-range writes. */
    memset(expo, 0, sizeof(expo));
    if (isp_tunnel(ISP_INNER_GET_EXPOSURE, expo) == 0) {
        uint32_t *pub2 = (uint32_t *)(expo + 0x1c);
        printf("[ae] readback: pub[emax=%u step=%u stable_range=%u hold_range=%u speed=%u]\n",
               pub2[1], pub2[8], pub2[9], pub2[10], pub2[11]);
    }

    /* Manual WB — fixes blue cast. Day gains; night.c switches to neutral
     * gains when it engages night mode. */
    if (isp_set_wb_manual(460, 256, 504) == 0)
        printf("[ae] SET_WB OK\n");

    /* Frame timing — tells ISP AE the sensor's VTS (2284 lines per frame).
     * This is the DAY config; night.c reconfigures to {10fps, vts=3426}
     * when it engages night mode (Ghidra + isp_hook-confirmed values from
     * stock anyka_ipc). */
    uint32_t frame_rate[4] = {15, 2284, 8, 15};
    if (isp_tunnel(ISP_INNER_SET_FRAME_RATE, frame_rate) == 0)
        printf("[ae] SET_FRAME_RATE OK (15fps, vts=2284)\n");

    uint32_t fps[4] = {15, 0, 0, 15};
    if (isp_tunnel(ISP_INNER_SET_SENSOR_FPS, fps) == 0)
        printf("[ae] SET_SENSOR_FPS OK\n");

    /* AWB_EX — correct 5124-byte payload from sensor_isp.conf.
     * Previous 16/256-byte buffers caused EFAULT; kernel reads 0x1404 bytes. */
    isp_reapply_awb_ex();

    /* 3D NR reference frame dimensions */
    uint32_t nr_ref[2] = {1920, 1080};
    if (isp_tunnel(ISP_INNER_3D_NR_REF, nr_ref) == 0)
        printf("[ae] 3D_NR_REF OK\n");

    return 0;
}

/* Windowed AE diagnostics: sample lum/hw_exp every tick (~15Hz) instead of
 * once every ~1.3s, and summarize each ~1s window as min/max/last plus a
 * "reversal" count (how many times lum changed direction within the
 * window) so real high-frequency hunting is visible even though we only
 * print once per window. A window with reversals >= ~4-6 or a lum
 * min/max spread that swings between near-black and near-white is
 * oscillation; a window that's flat (spread small, reversals 0-1) is
 * genuinely stable. */
static void *ae_loop(void *arg)
{
    uint8_t  run_info[36];
    static uint8_t expo[0x720];
    int      tick = 0;

    int      win_n = 0;
    int      lum_min = 256, lum_max = -1, lum_last = -1, lum_prev = -1;
    uint32_t exp_min = 0xffffffffu, exp_max = 0;
    int      reversals = 0;
    int      dir = 0; /* -1 falling, 0 unknown/flat, +1 rising */

    (void)arg;
    printf("[ae] hardware AE tick started (~15Hz GET_EXPOSURE, no writes, no suspend — "
           "on-chip AE has exclusive control, matching anyka_ipc)\n");

    while (g_ae_running) {
        usleep(AE_TICK_US);

        /* control.c's "ae.enabled=0" freezes exposure at whatever it
         * currently is (skip the tick entirely) for A/B testing AE on vs
         * off without recompiling. Last known stats keep being returned by
         * ae_get_last_stats() unchanged. */
        if (!g_tuning.enabled) continue;

        /* GET_EXPOSURE is the tick that advances the ISP's on-chip AE state
         * machine — see the header comment. We never write it back. */
        memset(expo, 0, sizeof(expo));
        if (isp_tunnel(ISP_INNER_GET_EXPOSURE, expo) != 0) continue;

        memset(run_info, 0, sizeof(run_info));
        if (isp_tunnel(ISP_INNER_GET_AE_RUN_INFO, run_info) != 0) continue;

        int      lum      = run_info[0];
        uint32_t hw_exp   = *(uint32_t *)(run_info + 4);
        uint32_t hw_sgain = *(uint32_t *)(run_info + 8);
        uint32_t hw_isp   = *(uint32_t *)(run_info + 12);

        ae_report_stats(lum, hw_exp, hw_sgain, hw_isp);

        if (lum < lum_min) lum_min = lum;
        if (lum > lum_max) lum_max = lum;
        if (hw_exp < exp_min) exp_min = hw_exp;
        if (hw_exp > exp_max) exp_max = hw_exp;
        lum_last = lum;

        if (lum_prev >= 0 && lum != lum_prev) {
            int new_dir = (lum > lum_prev) ? 1 : -1;
            if (dir != 0 && new_dir != dir) reversals++;
            dir = new_dir;
        }
        lum_prev = lum;
        win_n++;

        if (++tick < AE_WINDOW_TICKS) continue;
        tick = 0;

        uint32_t priv_exp = ((uint32_t *)expo)[1];

        printf("[ae] window n=%2d  lum[min=%3d max=%3d last=%3d]  "
               "hw_exp[min=%5u max=%5u]  reversals=%d  sgain=%.1fx isp=%.1fx priv_exp=%u%s\n",
               win_n, lum_min, lum_max, lum_last, exp_min, exp_max, reversals,
               hw_sgain/256.0f, hw_isp/256.0f, priv_exp,
               (reversals >= 4 || lum_max - lum_min >= 100) ? "  <-- HUNTING" : "");

        win_n = 0; lum_min = 256; lum_max = -1; lum_prev = -1;
        exp_min = 0xffffffffu; exp_max = 0; reversals = 0; dir = 0;
    }

    printf("[ae] hardware AE tick stopped\n");
    return NULL;
}

int ae_start(void)
{
    /* ISP param setup already done by ae_init_isp_params() (called before
     * vi_start_capture() -- see that function's comment). This just spins
     * up the polling thread. */
    g_ae_running = 1;
    if (pthread_create(&g_ae_tid, NULL, ae_loop, NULL) != 0) {
        perror("[ae] pthread_create");
        g_ae_running = 0;
        return -1;
    }
    return 0;
}

void ae_stop(void)
{
    g_ae_running = 0;
    pthread_join(g_ae_tid, NULL);
}
