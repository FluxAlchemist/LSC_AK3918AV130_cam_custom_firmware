#pragma once
#include <stdint.h>

/* Auto-exposure tick thread.
 *
 * Ticks the ISP's on-chip AE state machine by polling GET_EXPOSURE at ~15Hz
 * (matching stock anyka_ipc behavior confirmed via isp_hook capture — see
 * ae.c). Does not write exposure values back and does not suspend hardware
 * AE; the kernel's own already-tuned AE algorithm does the convergence.
 * Periodically reads GET_AE_RUN_INFO for logging only.
 *
 * Call ae_init_isp_params() BEFORE vi_set_channel_attr()/vi_start_capture()
 * (before STREAMON) -- matches anyka_ipc's own strace-confirmed ordering and
 * avoids a kernel div0 race, see ae_init_isp_params()'s comment in ae.c.
 * Call ae_start() after venc_activate() as before.  ae_stop() on shutdown. */

int  ae_init_isp_params(void);
int  ae_start(void);
void ae_stop(void);

/* Latest AE sample (updated every ~15Hz tick), for night.c's day/night
 * monitor to read without its own ISP polling loop. Returns -1 if no
 * sample has been taken yet (AE thread not started or hasn't ticked once). */
int ae_get_last_stats(int *lum, uint32_t *hw_exp, uint32_t *hw_sgain, uint32_t *hw_isp);

/* Updates the same last-known-stats globals ae_get_last_stats() reads. Called by ae_loop()
 * normally; also called by aec_custom.c's loop when built with AEC_CUSTOM, so night.c/control.c
 * keep working unchanged regardless of which tick loop is actually driving exposure. */
void ae_report_stats(int lum, uint32_t hw_exp, uint32_t hw_sgain, uint32_t hw_isp);

/* Live-tunable AE parameters, for control.c's TCP tuning server.
 * enabled=0 freezes exposure at its current value (AE tick skipped). */
typedef struct {
    int stable_range;  /* exp_stable_range dead-band, pub[9] */
    int hold_range;    /* exp_hold_range, pub[10] */
    int speed;         /* exp_speed convergence aggressiveness, pub[11] */
    int exp_max;       /* exp_time_max ceiling, pub[1] */
    int enabled;        /* 1=tick GET_EXPOSURE normally, 0=frozen */
} ae_tuning_t;

void ae_get_tuning(ae_tuning_t *out);

/* Applies the given tuning immediately via GET+modify+SET_EXPOSURE. Returns
 * 0 on success, -1 on ioctl failure (values are still stored in case the
 * ISP fd isn't ready yet — a later ae_setup()/manual retry will pick them
 * up). */
int ae_set_tuning(const ae_tuning_t *in);
