/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host test of the pad mapping and mouse mode. It includes the module
 * itself (its state machine is static), feeds it evdev events with chosen
 * timestamps and records what would be sent to the host.
 */
#include "../src/input_evdev.c"

#include <stdarg.h>
#include <stdlib.h>

#define MS 1000000ull
/* Test times are offsets from BASE: the module reads time 0 as "never". */
#define BASE (5000 * MS)

static char out[65536];
static size_t outlen;
static int failures, checks;
static long sum_dx, sum_dy, sum_v, sum_h;
static int n_moves;

static void rec(const char *fmt, ...)
{
    va_list ap;

    if (outlen >= sizeof(out) - 128)
        return;
    va_start(ap, fmt);
    outlen += (size_t)vsnprintf(out + outlen, sizeof(out) - outlen, fmt, ap);
    va_end(ap);
    out[outlen++] = '\n';
    out[outlen] = '\0';
}

int LiSendMultiControllerEvent(short n, short mask, int flags,
                               unsigned char lt, unsigned char rt, short lx,
                               short ly, short rx, short ry)
{
    rec("pad %x %u %u %d %d %d %d", flags, lt, rt, lx, ly, rx, ry);
    return 0;
}

int LiSendControllerArrivalEvent(uint8_t n, uint16_t mask, uint8_t type,
                                 uint32_t flags, uint16_t caps)
{
    return 0;
}

int LiSendMouseMoveEvent(short dx, short dy)
{
    sum_dx += dx;
    sum_dy += dy;
    n_moves++;
    return 0;
}

int LiSendMouseButtonEvent(char action, int button)
{
    rec("button %s %d", action == BUTTON_ACTION_PRESS ? "down" : "up",
        button);
    return 0;
}

int LiSendKeyboardEvent(short vk, char action, char modifiers)
{
    if (modifiers != 0)
        rec("key %s %04x m%x", action == KEY_ACTION_DOWN ? "down" : "up",
            vk & 0xffff, modifiers);
    else
        rec("key %s %04x", action == KEY_ACTION_DOWN ? "down" : "up",
            vk & 0xffff);
    return 0;
}

int LiSendHighResScrollEvent(short n)
{
    sum_v += n;
    return 0;
}

int LiSendHighResHScrollEvent(short n)
{
    sum_h += n;
    return 0;
}

static void clear(void)
{
    outlen = 0;
    out[0] = '\0';
    sum_dx = sum_dy = sum_v = sum_h = 0;
    n_moves = 0;
}

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s\n--- sent:\n%s---\n", what, out);
    }
}

/* The recorded output contains line (a whole line). */
static int sent(const char *line)
{
    char want[128];
    size_t n = strlen(line);
    const char *p = out;

    snprintf(want, sizeof(want), "%s\n", line);
    if (strncmp(out, want, n + 1) == 0)
        return 1;
    while ((p = strstr(p, want)) != NULL) {
        if (p == out || p[-1] == '\n')
            return 1;
        p++;
    }
    return 0;
}

static void reset(int swap, int mouse)
{
    static const int sticks[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
    unsigned i;

    memset(I.ax, 0, sizeof(I.ax));
    for (i = 0; i < NELEM(sticks); i++)
        I.ax[sticks[i]] = (struct axis){ -32767, 32767, 0, 1 };
    I.ax[ABS_Z] = (struct axis){ 0, 255, 0, 1 };
    I.ax[ABS_RZ] = (struct axis){ 0, 255, 0, 1 };
    I.fd = 99;          /* "pad open": poll_timeout only checks it */
    I.opt.swap_abxy = swap;
    I.opt.pointer_speed = 0;
    I.opt.scroll_reverse = 0;
    I.pad = I.masked = 0;
    I.hat_x = I.hat_y = 0;
    I.lx = I.ly = I.rx = I.ry = 0;
    I.lt = I.rt = 0;
    I.dirty = I.dropped = 0;
    I.combo_since = 0;
    I.combo_fired = 0;
    I.menu_down = 0;
    I.menu_used = 0;
    I.mono_times = 0;
    I.forward_since = 0;
    I.forwarding = 1;
    I.mouse_on = mouse;
    I.mode = MODE_PAD;
    I.neutral_sent = 0;
    I.sent_buttons = I.sent_keys = 0;
    stop_motion();
    atomic_store(&I.events, 0);
    sync_host(1);
    clear();
}

static void stamp(struct input_event *e, uint64_t t)
{
    e->input_event_sec = (time_t)((BASE + t) / 1000000000ull);
    e->input_event_usec = (suseconds_t)((BASE + t) % 1000000000ull / 1000);
}

/* One evdev report made at time made and read at time t, then what the
 * reader does after each wakeup. */
static void ev_late(int type, int code, int value, uint64_t made,
                    uint64_t t)
{
    struct input_event e[2];

    memset(e, 0, sizeof(e));
    e[0].type = (unsigned short)type;
    e[0].code = (unsigned short)code;
    e[0].value = value;
    e[1].type = EV_SYN;
    e[1].code = SYN_REPORT;
    stamp(&e[0], made);
    stamp(&e[1], made);
    handle(&e[0], BASE + t);
    handle(&e[1], BASE + t);
    check_combo(BASE + t);
    sync_host(BASE + t);
}

static void ev(int type, int code, int value, uint64_t t)
{
    ev_late(type, code, value, t, t);
}

static void key(int code, int down, uint64_t t)
{
    ev(EV_KEY, code, down, t);
}

/* A poll timeout: the reader syncs without new events. */
static void wake(uint64_t t)
{
    check_combo(BASE + t);
    sync_host(BASE + t);
}

static int pt(uint64_t t)
{
    return poll_timeout(BASE + t);
}

/* Runs the reader's poll timeouts from t0 until t1; returns the last
 * wakeup time. */
static uint64_t run(uint64_t t0, uint64_t t1)
{
    uint64_t t = t0;

    while (t < t1) {
        int ms = pt(t);

        if (ms < 0)
            break;
        t += (uint64_t)ms * MS;
        if (t >= t1)
            break;
        wake(t);
    }
    return t;
}

static void test_pad(void)
{
    reset(0, 0);
    key(BTN_B, 1, 10 * MS);     /* labelled A: Xbox B by position */
    check(sent("pad 2000 0 0 0 0 0 0"), "label A is Xbox B");
    key(BTN_B, 0, 20 * MS);
    key(BTN_Y, 1, 30 * MS);     /* labelled X, the top button: Xbox Y */
    check(sent("pad 8000 0 0 0 0 0 0"), "label X is Xbox Y");
    key(BTN_Y, 0, 40 * MS);

    reset(1, 0);
    key(BTN_B, 1, 10 * MS);
    check(sent("pad 1000 0 0 0 0 0 0"), "swapped: label A is Xbox A");
    key(BTN_B, 0, 20 * MS);

    reset(0, 0);
    ev(EV_ABS, ABS_HAT0Y, -1, 10 * MS);
    check(sent("pad 1 0 0 0 0 0 0"), "d-pad up");
    ev(EV_ABS, ABS_Z, 255, 20 * MS);
    check(sent("pad 1 255 0 0 0 0 0"), "analog left trigger");
    check(pt(20 * MS) == -1, "pad mode sleeps until events");
}

static void test_menu_tap(void)
{
    reset(0, 0);
    key(BTN_TR, 1, 0);
    clear();
    key(BTN_MODE, 1, 100 * MS);
    check(sent("pad 0 0 0 0 0 0 0"), "menu press releases the pad");
    key(BTN_MODE, 0, 250 * MS);
    check(input_take_events() == INPUT_EV_MENU, "a tap opens the menu");
    check(strstr(out, "button") == NULL, "a tap clicks nothing");
    check(!sent("pad 200 0 0 0 0 0 0"), "R1 held across the tap stays "
          "unseen");
    key(BTN_TR, 0, 300 * MS);
    key(BTN_TR, 1, 310 * MS);
    check(sent("pad 200 0 0 0 0 0 0"), "R1 pressed again is sent");

    /* A tap while a stick is held must still open the menu. */
    reset(0, 0);
    ev(EV_ABS, ABS_RX, 32767, 0);
    clear();
    key(BTN_MODE, 1, 100 * MS);
    check(pt(100 * MS) == -1, "a stick pushed before MENU is masked");
    run(100 * MS, 250 * MS);
    key(BTN_MODE, 0, 250 * MS);
    check(n_moves == 0, "a short tap does not move the pointer");
    check(input_take_events() == INPUT_EV_MENU, "tap with a stick held");

    /* A stick pushed during a tap waits for the 200 ms hold. */
    reset(0, 0);
    key(BTN_MODE, 1, 100 * MS);
    ev(EV_ABS, ABS_RX, 32767, 110 * MS);
    check(pt(110 * MS) == 190, "stick waits for the hold");
    run(110 * MS, 250 * MS);
    key(BTN_MODE, 0, 250 * MS);
    check(n_moves == 0, "a stick pushed in a short tap moves nothing");
    check(input_take_events() == INPUT_EV_MENU, "and the tap opens the menu");

    /* A long press is not a tap. */
    reset(0, 0);
    key(BTN_MODE, 1, 0);
    key(BTN_MODE, 0, 1200 * MS);
    check(input_take_events() == 0, "a 1.2 s hold opens no menu");
}

static void test_menu_hold(void)
{
    uint64_t t;

    reset(0, 0);
    key(BTN_MODE, 1, 0);
    key(BTN_B, 1, 50 * MS);     /* labelled A */
    check(sent("button down 1"), "hold MENU + A: left button");
    key(BTN_B, 0, 80 * MS);
    check(sent("button up 1"), "A released: left button up");
    key(BTN_MODE, 0, 150 * MS);
    check(input_take_events() == 0, "a hold that clicked opens no menu");

    reset(0, 0);
    key(BTN_MODE, 1, 0);
    ev(EV_ABS, ABS_RX, 32767, 20 * MS);
    t = run(20 * MS, 700 * MS);
    check(n_moves > 0 && sum_dx > 0 && sum_dy == 0, "hold MENU + right "
          "stick moves right");
    check(sum_dx > 1800 * 45 / 100 && sum_dx < 1800 * 55 / 100,
          "about 0.5 s at full speed");
    key(BTN_MODE, 0, t + MS);
    check(input_take_events() == 0, "a hold that moved opens no menu");
    check(sent("pad 0 0 0 0 0 0 0") && !sent("pad 0 0 0 0 0 32767 0"),
          "MENU up: the stick still pushed is not given to the game");
    clear();
    run(t + MS, t + 500 * MS);
    check(n_moves == 0, "the pointer stops with MENU up");
    ev(EV_ABS, ABS_RX, 0, t + 510 * MS);
    ev(EV_ABS, ABS_RX, 32767, t + 520 * MS);
    check(sent("pad 0 0 0 0 0 32767 0"), "a recentred stick goes to the "
          "game again");

    /* A button held while MENU goes down does not click. */
    reset(0, 0);
    key(BTN_B, 1, 0);
    key(BTN_MODE, 1, 10 * MS);
    key(BTN_MODE, 0, 900 * MS);
    check(strstr(out, "button") == NULL, "A held across MENU: no click");
    check(input_take_events() == INPUT_EV_MENU, "an idle hold is a tap");
}

static void test_mouse_map(void)
{
    reset(0, 1);
    check(I.mode == MODE_MOUSE, "mouse mode on");
    key(BTN_A, 1, 10 * MS);     /* labelled B */
    check(sent("button down 3"), "B: right button");
    key(BTN_A, 0, 20 * MS);
    check(sent("button up 3"), "B up");
    key(BTN_Y, 1, 30 * MS);     /* labelled X */
    check(sent("button down 2"), "X: middle button");
    key(BTN_Y, 0, 40 * MS);
    key(BTN_X, 1, 50 * MS);     /* labelled Y */
    check(sent("key down 800d"), "Y: Enter");
    key(BTN_X, 0, 60 * MS);
    check(sent("key up 800d"), "Y up");
    key(BTN_TL, 1, 70 * MS);
    key(BTN_TR, 1, 80 * MS);
    key(BTN_TL, 0, 90 * MS);
    check(!sent("button up 1"), "L1 up while R1 holds the left button");
    key(BTN_TR, 0, 100 * MS);
    check(sent("button up 1"), "left button up with both released");
    clear();
    ev(EV_ABS, ABS_HAT0X, 1, 110 * MS);
    check(sent("key down 8027"), "d-pad right: arrow key");
    ev(EV_ABS, ABS_HAT0X, -1, 120 * MS);
    check(sent("key up 8027") && sent("key down 8025"), "right to left");
    ev(EV_ABS, ABS_HAT0X, 0, 130 * MS);
    check(sent("key up 8025"), "d-pad released");
    clear();
    ev(EV_ABS, ABS_RZ, 200, 140 * MS);
    check(sent("button down 3"), "R2: right button");
    ev(EV_ABS, ABS_RZ, 80, 150 * MS);
    check(!sent("button up 3"), "R2 half released keeps it down");
    ev(EV_ABS, ABS_RZ, 30, 160 * MS);
    check(sent("button up 3"), "R2 released");
    check(strstr(out, "pad ") == NULL, "no controller reports in mouse mode");
    key(BTN_START, 1, 170 * MS);
    key(BTN_START, 0, 180 * MS);
    check(strstr(out, "pad ") == NULL, "START does nothing in mouse mode");
}

static void test_motion(void)
{
    reset(0, 1);
    ev(EV_ABS, ABS_RY, -32767, 0);      /* up */
    run(0, 1000 * MS);
    check(sum_dy < -1700 && sum_dy > -1900 && sum_dx == 0,
          "right stick up: 1800 px/s up");
    check(n_moves >= 90, "the pointer updates every 10 ms");
    ev(EV_ABS, ABS_RY, 0, 1000 * MS);
    check(pt(1000 * MS) == -1, "centred sticks stop the ticks");

    reset(0, 1);
    ev(EV_ABS, ABS_RX, 32767 / 10, 0);  /* inside the dead zone */
    check(pt(0) == -1, "dead zone: no ticks");
    run(0, 500 * MS);
    check(n_moves == 0, "dead zone: no motion");

    reset(0, 1);
    ev(EV_ABS, ABS_RX, 32767 / 2, 0);
    run(0, 1000 * MS);
    check(sum_dx > 100 && sum_dx < 400, "half deflection is slower");

    reset(0, 1);
    ev(EV_ABS, ABS_Y, -32767, 0);       /* left stick up */
    run(0, 1000 * MS);
    check(sum_v > 2300 && sum_v < 2500 && n_moves == 0,
          "left stick up scrolls up");
    ev(EV_ABS, ABS_Y, 0, 1000 * MS);
    ev(EV_ABS, ABS_X, 32767, 1000 * MS);
    run(1000 * MS, 1500 * MS);
    check(sum_h > 1100 && sum_h < 1300, "left stick right scrolls right");
}

static void test_mode_changes(void)
{
    reset(0, 1);
    key(BTN_TL, 1, 0);
    check(sent("button down 1"), "L1 down");
    clear();
    pthread_mutex_lock(&I.lock);
    I.mouse_on = 0;
    sync_host(BASE + 10 * MS);
    pthread_mutex_unlock(&I.lock);
    check(sent("button up 1"), "mouse off releases the left button");
    check(sent("pad 0 0 0 0 0 0 0"), "L1 held into pad mode is unseen");
    key(BTN_TL, 0, 20 * MS);
    key(BTN_TL, 1, 30 * MS);
    check(sent("pad 100 0 0 0 0 0 0"), "L1 pressed again is LB");

    /* Paused (menu open) releases everything; resume masks held keys. */
    reset(0, 1);
    ev(EV_ABS, ABS_HAT0Y, 1, 0);
    check(sent("key down 8028"), "down arrow");
    clear();
    I.forwarding = 0;
    sync_host(BASE + 10 * MS);
    check(sent("key up 8028"), "pause releases the arrow key");
    ev(EV_ABS, ABS_HAT0Y, 0, 20 * MS);
    key(BTN_B, 1, 30 * MS);
    I.forwarding = 1;
    sync_host(BASE + 40 * MS);
    check(strstr(out, "button") == NULL, "the resuming A press is unseen");
    key(BTN_B, 0, 50 * MS);
    key(BTN_B, 1, 60 * MS);
    check(sent("button down 1"), "A after resume clicks");

    /* The exit combo works in mouse mode and releases the buttons. */
    reset(0, 1);
    key(BTN_START, 1, MS);
    key(BTN_SELECT, 1, MS);
    key(BTN_TL, 1, MS);
    key(BTN_TR, 1, MS);
    check(pt(MS) == 50, "combo polls");
    run(MS, 1100 * MS);
    check(input_take_events() == INPUT_EV_EXIT, "combo exits");
    check(sent("button up 1"), "combo releases the left button");
}

static void test_review_fixes(void)
{
    struct input_event e;

    /* With mouse mode on, MENU is no shift: a tap while the pointer
     * moves still opens the menu. */
    reset(0, 1);
    ev(EV_ABS, ABS_RX, 32767, 0);
    run(0, 100 * MS);
    key(BTN_MODE, 1, 100 * MS);
    run(100 * MS, 250 * MS);
    key(BTN_MODE, 0, 250 * MS);
    check(n_moves > 0, "mouse mode on: the pointer keeps moving");
    check(input_take_events() == INPUT_EV_MENU, "mouse mode on: a tap "
          "while moving opens the menu");

    /* Events lost while MENU is held, its release among them: mouse mode
     * ends and no menu opens. Lost trigger state reads back as released
     * (the ioctls fail on the test's fake fd). */
    reset(0, 0);
    ev(EV_ABS, ABS_Z, 80, 0);
    key(BTN_MODE, 1, 10 * MS);
    check(I.mode == MODE_MOUSE, "MENU held: mouse mode");
    clear();
    memset(&e, 0, sizeof(e));
    e.type = EV_SYN;
    e.code = SYN_DROPPED;
    handle(&e, BASE + 20 * MS);
    e.type = EV_KEY;
    e.code = BTN_MODE;
    e.value = 0;
    handle(&e, BASE + 20 * MS);
    e.type = EV_SYN;
    e.code = SYN_REPORT;
    e.value = 0;
    handle(&e, BASE + 20 * MS);
    wake(20 * MS);
    check(I.mode == MODE_PAD && I.menu_down == 0, "SYN_DROPPED ends a lost "
          "MENU hold");
    check(input_take_events() == 0, "and opens no menu");
    check(I.lt == 0 && !(I.pad & P_L2), "trigger state reread");

    /* The exit combo ends a MENU hold: its release opens no menu. */
    reset(0, 0);
    key(BTN_START, 1, 0);
    key(BTN_SELECT, 1, 0);
    key(BTN_TL, 1, 0);
    key(BTN_TR, 1, 0);
    run(0, 900 * MS);
    key(BTN_MODE, 1, 900 * MS);
    run(900 * MS, 1100 * MS);
    key(BTN_MODE, 0, 1100 * MS);
    check(input_take_events() == INPUT_EV_EXIT, "combo then MENU up: only "
          "the exit");

    /* Resume from the UI menu: a partly pulled trigger stays unseen. */
    reset(0, 0);
    I.forwarding = 0;
    sync_host(BASE);
    ev(EV_ABS, ABS_Z, 50, 10 * MS);
    clear();
    I.forwarding = 1;
    sync_host(BASE + 20 * MS);
    check(sent("pad 0 0 0 0 0 0 0"), "trigger at 50 masked on resume");
    ev(EV_ABS, ABS_Z, 5, 30 * MS);
    ev(EV_ABS, ABS_Z, 50, 40 * MS);
    check(sent("pad 0 50 0 0 0 0 0"), "trigger seen after it rested");

    /* The reader handles the UI's resuming press after the engine has
     * already resumed: event times keep it unseen. */
    reset(0, 0);
    I.forwarding = 0;
    sync_host(BASE);
    I.mono_times = 1;
    I.forwarding = 1;
    I.forward_since = BASE + 50 * MS;
    sync_host(BASE + 50 * MS);
    clear();
    ev_late(EV_KEY, BTN_MODE, 1, 40 * MS, 60 * MS);
    check(I.mode == MODE_PAD, "a late resuming MENU press is no shift");
    key(BTN_MODE, 0, 100 * MS);
    check(input_take_events() == 0, "its release does not reopen the menu");
    ev_late(EV_KEY, BTN_B, 1, 45 * MS, 60 * MS);
    check(!sent("pad 2000 0 0 0 0 0 0"), "a late resuming A press is unseen");
    key(BTN_B, 0, 110 * MS);
    key(BTN_B, 1, 120 * MS);
    check(sent("pad 2000 0 0 0 0 0 0"), "A pressed after the resume is sent");

    /* The pad disappearing releases what the host holds. */
    reset(0, 1);
    key(BTN_B, 1, 0);
    check(sent("button down 1"), "left button down");
    pad_lost();
    check(sent("button up 1") && I.fd == -1, "pad lost: button released");
    reset(0, 0);
    key(BTN_B, 1, 0);
    clear();
    pad_lost();
    check(sent("pad 0 0 0 0 0 0 0"), "pad lost: controller released");
}

static void test_settings(void)
{
    /* Pointer speed scales the full-deflection speed. */
    reset(0, 1);
    I.opt.pointer_speed = 50;
    ev(EV_ABS, ABS_RY, -32767, 0);
    run(0, 1000 * MS);
    check(sum_dy < -850 && sum_dy > -950, "pointer 50%: 900 px/s");
    reset(0, 1);
    I.opt.pointer_speed = 200;
    ev(EV_ABS, ABS_RY, -32767, 0);
    run(0, 1000 * MS);
    check(sum_dy < -3500 && sum_dy > -3700, "pointer 200%: 3600 px/s");

    /* Reverse scroll turns both wheel directions round. */
    reset(0, 1);
    I.opt.scroll_reverse = 1;
    ev(EV_ABS, ABS_Y, -32767, 0);
    run(0, 1000 * MS);
    check(sum_v < -2300 && sum_v > -2500, "reverse: stick up scrolls down");
    ev(EV_ABS, ABS_Y, 0, 1000 * MS);
    ev(EV_ABS, ABS_X, 32767, 1000 * MS);
    run(1000 * MS, 1500 * MS);
    check(sum_h < -1100 && sum_h > -1300, "reverse: stick right scrolls "
          "left");

    /* Key taps: modifiers down first and up last, each event with the
     * modifiers held after it. */
    reset(0, 0);
    input_send_key(0x73, INPUT_MOD_ALT);
    check(strcmp(out, "key down 80a4 m4\nkey down 8073 m4\n"
                      "key up 8073 m4\nkey up 80a4\n") == 0, "Alt+F4");
    clear();
    input_send_key(0x1b, INPUT_MOD_CTRL | INPUT_MOD_SHIFT);
    check(strcmp(out, "key down 80a2 m2\nkey down 80a0 m3\n"
                      "key down 801b m3\nkey up 801b m3\n"
                      "key up 80a0 m2\nkey up 80a2\n") == 0,
          "Ctrl+Shift+Esc");
    clear();
    {
        struct timespec t0, t1;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        input_send_key(0x0d, 0);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        check(strcmp(out, "key down 800d\nkey up 800d\n") == 0, "Enter");
        check((t1.tv_sec - t0.tv_sec) * 1000000000LL +
              (t1.tv_nsec - t0.tv_nsec) >= KEY_HOLD_NS, "the key is held "
              "50 ms");
    }

    /* The rumble capability goes in the arrival event only with rumble
     * on. */
    I.opt.rumble = 0;
    check((arrival_caps() & LI_CCAP_ANALOG_TRIGGERS) &&
          !(arrival_caps() & LI_CCAP_RUMBLE), "no rumble: no rumble "
          "capability");
    I.opt.rumble = 1;
    check((arrival_caps() & LI_CCAP_ANALOG_TRIGGERS) &&
          (arrival_caps() & LI_CCAP_RUMBLE), "rumble: rumble capability");
    I.opt.rumble = 0;
}

int main(void)
{
    test_pad();
    test_menu_tap();
    test_menu_hold();
    test_mouse_map();
    test_motion();
    test_mode_changes();
    test_review_fixes();
    test_settings();
    printf("input-test: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
