/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_INPUT_EVDEV_H
#define MOONLIGHT_TSP_INPUT_EVDEV_H

/*
 * Reads the built-in pad ("TRIMUI Player1", a virtual Xbox 360 pad made by
 * trimui_inputd) from evdev and forwards it as controller 0. The device is
 * not grabbed, so keymon's hotkeys keep working.
 *
 * MENU (BTN_MODE) is never forwarded: a short press raises INPUT_EV_MENU.
 * Holding START+SELECT+L1+R1 for one second raises INPUT_EV_EXIT.
 *
 * Mouse mode (input_set_mouse, or for as long as MENU is held) sends the
 * pad as a mouse and keyboard instead, by printed label: A, L1 and R1 are
 * the left button, B, L2 and R2 the right one, X the middle one, Y is
 * Enter, the d-pad the arrow keys, the right stick moves the pointer and
 * the left stick is the wheel. A MENU hold that drove the mouse does not
 * open the menu when released.
 */
enum input_ev {
    INPUT_EV_NONE = 0,
    INPUT_EV_MENU = 1,
    INPUT_EV_EXIT = 2,
};

#define INPUT_DEVICE_NAME "TRIMUI Player1"

struct input_options {
    int swap_abxy;      /* exchange A/B and X/Y (Nintendo-style labels) */
    int mouse;          /* start in mouse mode */
    int rumble;         /* tell the host the pad can rumble */
    int pointer_speed;  /* mouse pointer speed in percent (0: 100) */
    int scroll_reverse; /* the left stick scrolls the other way */
};

/*
 * Starts the reader thread. wake_fd (may be -1) gets one byte written for
 * every event; collect them with input_take_events.
 */
int input_start(const struct input_options *opt, int wake_fd);
void input_stop(void);

/* Returns and clears the pending INPUT_EV_* bits. */
unsigned input_take_events(void);

/* While disabled, the host sees a released pad (menu open). */
void input_set_forwarding(int enabled);

/* Turns mouse mode on or off. Held buttons stay unseen in the new mode
 * until they are released. */
void input_set_mouse(int on);

/* Sends a one-shot Guide (Xbox button) press. */
void input_send_guide(void);

/* Win32 modifier bits for input_send_key (the protocol's MODIFIER_*). */
#define INPUT_MOD_SHIFT 0x01
#define INPUT_MOD_CTRL 0x02
#define INPUT_MOD_ALT 0x04
#define INPUT_MOD_WIN 0x08

/*
 * Taps a key: presses the modifier keys in MODS (Ctrl, Alt, Shift, Win),
 * then the Win32 virtual key VK down, holds it 50 ms (without I.lock),
 * then releases VK and the modifiers. The call takes 50 ms.
 */
void input_send_key(int vk, int mods);

/* Prints raw events and the mapped state until SIGINT (button test). */
int input_dump(void);

#endif
