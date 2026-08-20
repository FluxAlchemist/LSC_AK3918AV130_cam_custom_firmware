#pragma once

/* Custom software AEC (auto-exposure control) -- an alternative to ae.c's on-chip-tick-only
 * loop, carrying over lessons from an earlier (failed, removed) userspace
 * P-controller attempt.
 *
 * Compile-time alternative to ae.c's on-chip-tick-only loop, enabled only when built with
 * AEC_CUSTOM defined (`make aec-custom`). Never runs alongside ae.c's own tick loop -- main.c
 * calls exactly one of ae_start()/aec_custom_start() depending on the build flag, chosen once at
 * startup, never switched at runtime. ae.c itself (ae_init_isp_params(), ae_tuning_t,
 * ae_get_tuning()/ae_set_tuning(), ae_report_stats()) stays compiled in either way -- night.c and
 * control.c depend on those regardless of which loop is driving exposure.
 *
 * Suspends the on-chip AE algorithm (ISP_INNER_AE_SUSPEND) for the process lifetime once started;
 * resumes it in aec_custom_stop(). */

int  aec_custom_start(void);
void aec_custom_stop(void);
