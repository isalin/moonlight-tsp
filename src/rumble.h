/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_RUMBLE_H
#define MOONLIGHT_TSP_RUMBLE_H

/*
 * Rumble on the built-in motor. The motor is a GPIO output (227, PH3;
 * CrossMix exports it at boot) that is only on or off, so a thread makes
 * software PWM while the host asks for rumble and sleeps otherwise.
 */
#define RUMBLE_GPIO_VALUE "/sys/class/gpio/gpio227/value"

#define RUMBLE_PERIOD_NS 20000000u   /* PWM period */
#define RUMBLE_MIN_ON_NS 4000000u    /* shortest pulse the motor feels */

/*
 * Opens PATH (the GPIO value file) and starts the thread. PERCENT (1-100)
 * is the user's strength setting (see rumble_on_ns). Returns -1, and logs
 * once, if the file cannot be opened; rumble_set then does nothing.
 */
int rumble_start(const char *path, unsigned percent);

/* The host's motor strengths (0-65535); the stronger one is used. */
void rumble_set(unsigned short low, unsigned short high);

/* While paused the motor is off; on resume it gets its last value again. */
void rumble_pause(int paused);

/* Motor off, thread stopped, file closed. */
void rumble_stop(void);

/* Async-signal-safe: motor off with one write. The motor then stays off
 * until rumble_stop, also if the thread is in a pulse. */
void rumble_emergency_off(void);

/* Writes 0 to PATH (after a killed engine). Returns 0 if written. */
int rumble_reset(const char *path);

/*
 * Pulse length in ns in one RUMBLE_PERIOD_NS for the host's STRENGTH
 * (0-65535) at the setting PERCENT (0-100): 0 is off, and
 * RUMBLE_PERIOD_NS is constantly on. The host's full strength gives
 * RUMBLE_MIN_ON_NS at 5% (and below) and the full period at 100%, in
 * equal steps. A weak value gets at least RUMBLE_MIN_ON_NS.
 */
unsigned rumble_on_ns(unsigned strength, unsigned percent);

/*
 * One 0.5 s rumble on PATH at PERCENT, as the host's full strength feels
 * at that setting (the Settings test). SIGTERM, SIGINT, SIGHUP, SIGQUIT,
 * SIGUSR1, SIGUSR2, SIGALRM and SIGPIPE stop it early with the motor off.
 * Returns 0, 1 if PATH cannot be opened, or 128 + the signal.
 */
int rumble_test(const char *path, unsigned percent);

#endif
