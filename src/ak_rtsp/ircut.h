#pragma once

/* Initialize the IR cut filter and IR LED GPIOs.
 *
 * Mirrors ht_night_mode_init → FUN_000d57f4 → FUN_000d4878 from anyka_ipc.
 * Exports GPIO 65/66 (ircut solenoid) and GPIO 70 (IR LED), sets them to
 * day mode (visible-light pass), and fires the reset pulse once per boot
 * to home the solenoid.  Produces the characteristic two-click sound. */
int ircut_init(void);

/* Switch the IR-cut filter + IR LED to day (night=0) or night (night=1)
 * position at runtime. Called by night.c's auto day/night monitor after
 * ircut_init() has already run. */
void ircut_switch(int night);
