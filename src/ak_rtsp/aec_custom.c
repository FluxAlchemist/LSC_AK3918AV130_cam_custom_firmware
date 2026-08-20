#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include "ak_ioctls.h"
#include "isp.h"
#include "ae.h"
#include "aec_custom.h"

/* -------------------------------------------------------------------------
 * Custom software AEC. Summary of what makes this different from the
 * earlier, failed userspace P-controller ("AE rewrite" attempt):
 *
 *   1. Cooldown after every write (COOLDOWN_TICKS, with an early-stabilization
 *      check) -- the old attempt reacted to readings before the sensor/ISP
 *      pipeline had settled from its own previous write, which is exactly
 *      the textbook cause of the 2-value limit cycle it produced.
 *   2. EMA-filtered luma, not raw samples, before the controller ever sees it.
 *   3. PI (not pure P) with anti-windup, rate-clamped step -- avoids both the
 *      "stalls just outside the dead-band" failure of pure-P on integer
 *      hardware, and the "climbs to blown-out white" overshoot of unclamped
 *      ratio scaling.
 *   4. Warm-up burst before the very first actuation, not a single-sample seed.
 *
 * Single control variable: exp_time (priv[0x04] in the 0x720-byte exposure
 * attr struct). Gain (sensor_gain/isp_gain) is read back and written
 * unchanged every cycle -- confirmed that stock's real gain never engages
 * even during hard transients, so matching that is matching observed
 * reality, not cutting a corner.
 * ------------------------------------------------------------------------- */

#define AEC_TICK_US            66000  /* ~15Hz, matches ae.c's tick rate */
#define AEC_WINDOW_TICKS          15  /* ~1s window for logging, matches ae.c */

#define AEC_WARMUP_TICKS           8  /* ~530ms before first actuation is allowed */
#define AEC_COOLDOWN_TICKS         5  /* ~330ms after a write before reacting again */
#define AEC_COOLDOWN_MIN_TICKS     2  /* hard floor even if "settled early" fires */
#define AEC_STABILIZE_WINDOW       3  /* ticks of lum_filt history for early-exit check */
#define AEC_STABILIZE_SPREAD       2  /* lum units -- spread below this = "settled early" */

#define AEC_EMA_ALPHA_Q8          51  /* ~0.2 in Q8 */
#define AEC_DEADBAND               6  /* lum units either side of target */

#define AEC_KP_Q8                256  /* 1.0 in Q8 -- full-scale error saturates the rate limiter */
#define AEC_KI_Q8                 26  /* ~0.1 in Q8 -- small, just nudges past the last-mile stall */
#define AEC_MAX_STEP_FRAC_Q8      38  /* ~15% of current exp_time, in Q8 */
#define AEC_MAX_I_Q8              38  /* anti-windup clamp, same bound as one max step */

/* Empirical floor, not a confirmed hardware min-field (none found in the
 * exposure attr struct's pub[] documented so far) -- matches the value
 * (256) seen repeatedly as the observed hw_exp floor across many logged
 * sessions. Revisit if a real exp_time_min field is ever confirmed. */
#define AEC_EXP_TIME_MIN         256
#define AEC_FALLBACK_TARGET_LUMI  35  /* used only if pub[12] reads back as 0 */

static volatile int g_aec_running;
static pthread_t    g_aec_tid;

static int aec_suspend_onchip_ae(int suspend)
{
#ifdef AEC_SKIP_SUSPEND
    /* Diagnostic-only build (make aec-custom-nosuspend): skip AE_SUSPEND
     * entirely to isolate whether IT (not our own control loop) is what's
     * driving the much heavier div0 flood seen with AEC_CUSTOM. The on-chip
     * algorithm stays technically live in the background this way -- NOT
     * something to ship, reintroduces the "two controllers" risk from the
     * original failed P-controller attempt. */
    (void)suspend;
    return 0;
#else
    /* Payload format for ISP_INNER_AE_SUSPEND is not byte-confirmed (never
     * captured from stock -- anyka_ipc never calls it).
     * Use the same uint32_t[4]-with-first-word-set
     * shape as the other simple SET-style tunnel calls (SET_FRAME_RATE,
     * SET_SENSOR_FPS) rather than a bare uint32_t, since this codebase has
     * been bitten before by too-small buffers causing EFAULT (AWB_EX). */
    uint32_t buf[4] = { (uint32_t)suspend, 0, 0, 0 };
    return isp_tunnel(ISP_INNER_AE_SUSPEND, buf);
#endif
}

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void *aec_loop(void *arg)
{
    uint8_t run_info[36];
    static uint8_t expo[0x720];
    (void)arg;

    printf("[aec] custom software AEC starting -- suspending on-chip AE\n");
    if (aec_suspend_onchip_ae(1) != 0)
        fprintf(stderr, "[aec] AE_SUSPEND(1) failed (continuing anyway): %s\n", strerror(errno));

    /* Seed exp_time_max/target_lumi from the live struct (ae_init_isp_params()
     * already fixed pub[1]=exp_time_max to a sane ceiling before this ever
     * runs -- see main.c's call order). */
    uint32_t exp_time_max   = 32767;
    uint32_t target_lumi    = AEC_FALLBACK_TARGET_LUMI;
    uint32_t current_exp    = AEC_EXP_TIME_MIN;

    memset(expo, 0, sizeof(expo));
    if (isp_tunnel(ISP_INNER_GET_EXPOSURE, expo) == 0) {
        uint32_t *priv = (uint32_t *)expo;
        uint32_t *pub  = (uint32_t *)(expo + 0x1c);
        current_exp = priv[1];
        if (pub[1] > 0)  exp_time_max = pub[1];
        if (pub[12] > 0) target_lumi  = pub[12];
    }
    printf("[aec] target_lumi=%u exp_time_max=%u exp_time_min=%u starting_exp=%u\n",
           target_lumi, exp_time_max, (unsigned)AEC_EXP_TIME_MIN, current_exp);

    int32_t lum_filt_q8       = -1;   /* -1 = not yet seeded */
    int32_t i_accum_q8        = 0;
    int     warmup_remaining  = AEC_WARMUP_TICKS;
    int     cooldown_remaining = 0;

    int recent_lum_filt[AEC_STABILIZE_WINDOW];
    int recent_n = 0;

    int      tick = 0;
    int      win_n = 0;
    int      lum_min = 256, lum_max = -1, lum_last = -1, lum_prev = -1;
    uint32_t exp_min = 0xffffffffu, exp_max = 0;
    int      reversals = 0;
    int      dir = 0;
    int      lum_filt_disp = -1;
    const char *state = "warmup";

    while (g_aec_running) {
        usleep(AEC_TICK_US);

        ae_tuning_t t;
        ae_get_tuning(&t);
        if (!t.enabled) continue;  /* same freeze switch ae.c's loop honors */

        memset(run_info, 0, sizeof(run_info));
        if (isp_tunnel(ISP_INNER_GET_AE_RUN_INFO, run_info) != 0) continue;

        int      lum_raw  = run_info[0];
        uint32_t hw_exp   = *(uint32_t *)(run_info + 4);
        uint32_t hw_sgain = *(uint32_t *)(run_info + 8);
        uint32_t hw_isp   = *(uint32_t *)(run_info + 12);
        ae_report_stats(lum_raw, hw_exp, hw_sgain, hw_isp);

        /* Refresh the authoritative exp_time from the live struct -- this is
         * also the read half of the read-modify-write cycle used below when
         * we actually write. */
        memset(expo, 0, sizeof(expo));
        if (isp_tunnel(ISP_INNER_GET_EXPOSURE, expo) == 0)
            current_exp = ((uint32_t *)expo)[1];

        /* --- EMA filter (Q8 fixed-point, round-to-nearest on display) --- */
        int32_t lum_raw_q8 = (int32_t)lum_raw << 8;
        if (lum_filt_q8 < 0) lum_filt_q8 = lum_raw_q8;
        else lum_filt_q8 += (AEC_EMA_ALPHA_Q8 * (lum_raw_q8 - lum_filt_q8)) >> 8;
        lum_filt_disp = (lum_filt_q8 + 128) >> 8;

        /* --- windowed diagnostics bookkeeping, same shape as ae.c --- */
        if (lum_raw < lum_min) lum_min = lum_raw;
        if (lum_raw > lum_max) lum_max = lum_raw;
        if (hw_exp < exp_min) exp_min = hw_exp;
        if (hw_exp > exp_max) exp_max = hw_exp;
        lum_last = lum_raw;
        if (lum_prev >= 0 && lum_raw != lum_prev) {
            int new_dir = (lum_raw > lum_prev) ? 1 : -1;
            if (dir != 0 && new_dir != dir) reversals++;
            dir = new_dir;
        }
        lum_prev = lum_raw;
        win_n++;

        /* --- control logic --- */
        if (warmup_remaining > 0) {
            warmup_remaining--;
            state = "warmup";
        } else if (cooldown_remaining > 0) {
            cooldown_remaining--;
            state = "cooldown";
            if (cooldown_remaining <= (AEC_COOLDOWN_TICKS - AEC_COOLDOWN_MIN_TICKS)) {
                if (recent_n < AEC_STABILIZE_WINDOW) {
                    recent_lum_filt[recent_n++] = lum_filt_disp;
                } else {
                    memmove(recent_lum_filt, recent_lum_filt + 1,
                            (AEC_STABILIZE_WINDOW - 1) * sizeof(int));
                    recent_lum_filt[AEC_STABILIZE_WINDOW - 1] = lum_filt_disp;
                }
                if (recent_n >= AEC_STABILIZE_WINDOW) {
                    int mn = recent_lum_filt[0], mx = recent_lum_filt[0];
                    for (int i = 1; i < AEC_STABILIZE_WINDOW; i++) {
                        if (recent_lum_filt[i] < mn) mn = recent_lum_filt[i];
                        if (recent_lum_filt[i] > mx) mx = recent_lum_filt[i];
                    }
                    if (mx - mn <= AEC_STABILIZE_SPREAD) cooldown_remaining = 0;
                }
            }
        } else {
            int32_t err = (int32_t)target_lumi - lum_filt_disp;
            if (err > -AEC_DEADBAND && err < AEC_DEADBAND) {
                state = "settled";
                recent_n = 0;
            } else {
                int32_t err_frac_q8 = (target_lumi > 0)
                    ? (int32_t)(((int64_t)err * 256) / (int32_t)target_lumi) : 0;
                int32_t p_q8 = (AEC_KP_Q8 * err_frac_q8) >> 8;
                int32_t new_i = i_accum_q8 + ((AEC_KI_Q8 * err_frac_q8) >> 8);
                new_i = clamp_i32(new_i, -AEC_MAX_I_Q8, AEC_MAX_I_Q8);
                i_accum_q8 = new_i;

                int32_t corr_q8 = clamp_i32(p_q8 + i_accum_q8,
                                            -AEC_MAX_STEP_FRAC_Q8, AEC_MAX_STEP_FRAC_Q8);
                int64_t step = ((int64_t)current_exp * corr_q8) >> 8;
                int64_t new_exp = (int64_t)current_exp + step;

                if (new_exp >= (int64_t)exp_time_max) {
                    new_exp = exp_time_max;
                    i_accum_q8 = 0;  /* anti-windup: pinned, stop accumulating */
                    state = "at_ceiling";
                } else if (new_exp <= (int64_t)AEC_EXP_TIME_MIN) {
                    new_exp = AEC_EXP_TIME_MIN;
                    i_accum_q8 = 0;
                    state = "at_floor";
                } else {
                    state = "correcting";
                }

                if ((uint32_t)new_exp != current_exp) {
                    uint32_t *priv = (uint32_t *)expo;
                    priv[1] = (uint32_t)new_exp;  /* exp_time */
                    priv[2] = 1;                  /* exp_type -- keep MANUAL, matches stock */
                    if (isp_tunnel(ISP_INNER_SET_EXPOSURE, expo) == 0) {
                        current_exp = (uint32_t)new_exp;
                        cooldown_remaining = AEC_COOLDOWN_TICKS;
                        recent_n = 0;
                    }
                }
            }
        }

        if (++tick < AEC_WINDOW_TICKS) continue;
        tick = 0;

        printf("[aec] window n=%2d lum_raw[min=%3d max=%3d last=%3d] lum_filt=%3d target=%3u "
               "exp[min=%5u max=%5u cur=%5u] reversals=%d sgain=%.1fx isp=%.1fx state=%-10s%s\n",
               win_n, lum_min, lum_max, lum_last, lum_filt_disp, target_lumi,
               exp_min, exp_max, current_exp, reversals,
               hw_sgain / 256.0f, hw_isp / 256.0f, state,
               (reversals >= 4 || lum_max - lum_min >= 100) ? "  <-- HUNTING" : "");

        win_n = 0; lum_min = 256; lum_max = -1; lum_prev = -1;
        exp_min = 0xffffffffu; exp_max = 0; reversals = 0; dir = 0;
    }

    printf("[aec] custom software AEC stopped\n");
    return NULL;
}

int aec_custom_start(void)
{
    g_aec_running = 1;
    if (pthread_create(&g_aec_tid, NULL, aec_loop, NULL) != 0) {
        perror("[aec] pthread_create");
        g_aec_running = 0;
        return -1;
    }
    return 0;
}

void aec_custom_stop(void)
{
    g_aec_running = 0;
    pthread_join(g_aec_tid, NULL);
    aec_suspend_onchip_ae(0);
}
