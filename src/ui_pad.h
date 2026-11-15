/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_UI_PAD_H
#define MOONLIGHT_TSP_UI_PAD_H

/*
 * Menu input for the UI from the built-in pad's evdev device. Keys are
 * named by the printed labels: A (right face button) confirms, B (bottom)
 * goes back. The device is not grabbed; the engine reads it too while
 * streaming, and the UI stops polling it then.
 */
enum ui_key {
    UK_NONE = 0,
    UK_UP, UK_DOWN, UK_LEFT, UK_RIGHT,
    UK_A, UK_B, UK_X, UK_Y,
    UK_L1, UK_R1, UK_START, UK_SELECT, UK_MENU,
};

struct ui_pad {
    int fd;             /* evdev, -1 if not found */
    int repeat_fd;      /* timerfd for held directions */
    enum ui_key held;   /* direction being repeated */
    int stick_x, stick_y;           /* -1/0/1 from the left stick */
    int abs_min[2], abs_max[2];     /* ABS_X, ABS_Y ranges */
};

int ui_pad_open(struct ui_pad *p);
/* Looks for the device again if it is not open (0 when open). */
int ui_pad_reopen(struct ui_pad *p);
void ui_pad_close(struct ui_pad *p);

/* Discards queued events (after the engine had the pad). */
void ui_pad_drain(struct ui_pad *p);

/*
 * Reads available events and returns key presses into keys (at most max).
 * Call when fd is readable; call ui_pad_repeat when repeat_fd is.
 */
int ui_pad_read(struct ui_pad *p, enum ui_key *keys, int max);
enum ui_key ui_pad_repeat(struct ui_pad *p);

/* Stops any key repeat (e.g. when leaving the menu for the stream). */
void ui_pad_stop_repeat(struct ui_pad *p);

enum ui_key ui_key_from_name(const char *name);

#endif
