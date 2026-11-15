/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "input_evdev.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <Limelight.h>

/* Pad inputs by printed label, whatever --swap-abxy says. */
enum {
    P_A = 1 << 0,       /* right face button: BTN_B (codes are positional) */
    P_B = 1 << 1,       /* bottom: BTN_A */
    P_X = 1 << 2,       /* top: BTN_Y (0x134) */
    P_Y = 1 << 3,       /* left: BTN_X (0x133) */
    P_L1 = 1 << 4,
    P_R1 = 1 << 5,
    P_L2 = 1 << 6,      /* trigger pressed past TRIG_ON */
    P_R2 = 1 << 7,
    P_UP = 1 << 8,
    P_DOWN = 1 << 9,
    P_LEFT = 1 << 10,
    P_RIGHT = 1 << 11,
    P_START = 1 << 12,
    P_SELECT = 1 << 13,
    P_L3 = 1 << 14,
    P_R3 = 1 << 15,
    /* Analog inputs away from rest; used only to mask them. */
    P_LS = 1 << 16,     /* left stick beyond its dead zone */
    P_RS = 1 << 17,
    P_LT = 1 << 18,     /* trigger at TRIG_REST or more */
    P_RT = 1 << 19,
};
#define P_DPAD (P_UP | P_DOWN | P_LEFT | P_RIGHT)

#define COMBO_KEYS (P_START | P_SELECT | P_L1 | P_R1)
#define COMBO_NS 1000000000ull
#define MENU_TAP_NS 1000000000ull

/* Mouse mode: toggled from the UI menu, or for as long as MENU is held. */
#define MOUSE_TICK_NS 10000000ull   /* pointer and wheel update period */
#define MENU_STICK_NS 200000000ull  /* sticks act once MENU is held this long */
#define MOUSE_MAX_DT_NS 50000000ull /* longer gaps count as one tick */
#define STICK_DEADZONE 0.12f
#define POINTER_MAX 1800.0f         /* host pixels/s at full deflection */
#define SCROLL_MAX 2400.0f          /* wheel units/s (120 = one notch) */
#define SCROLL_MIN 15               /* smallest wheel event sent */
#define TRIG_ON 100                 /* L2/R2 count as pressed above this */
#define TRIG_OFF 60                 /* and as released below this */
#define TRIG_REST 16                /* below this a trigger is at rest */

/* Win32 virtual key codes (the protocol's key codes). Like the other
 * Moonlight clients, the engine sends them with 0x80 in the high byte. */
#define VK_FLAG 0x8000
#define VK_RETURN 0x0D
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28

#define SUPPORTED_FLAGS (A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG |        \
                         DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | LB_FLAG |       \
                         RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG |      \
                         RS_CLK_FLAG | SPECIAL_FLAG)

enum { MODE_OFF, MODE_PAD, MODE_MOUSE };

static const struct { unsigned keys; int button; } mouse_buttons[] = {
    { P_A | P_L1 | P_R1, BUTTON_LEFT },
    { P_B | P_L2 | P_R2, BUTTON_RIGHT },
    { P_X, BUTTON_MIDDLE },
};
static const struct { unsigned key; short vk; } mouse_keys[] = {
    { P_UP, VK_UP }, { P_DOWN, VK_DOWN }, { P_LEFT, VK_LEFT },
    { P_RIGHT, VK_RIGHT }, { P_Y, VK_RETURN },
};
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

struct axis {
    int min, max, flat;
    int valid;
};

static struct {
    int fd;
    pthread_t thread;
    int running;
    int stop_pipe[2];   /* also wakes the reader after a mode change */
    int wake_fd;
    struct input_options opt;
    struct axis ax[ABS_CNT];

    /* Everything below is guarded by lock: the reader thread updates the
     * pad per batch of events, the main thread changes the mode. */
    pthread_mutex_t lock;
    unsigned pad;               /* P_* inputs held */
    unsigned masked;            /* held across a mode change: unseen until
                                 * released */
    int hat_x, hat_y;
    short lx, ly, rx, ry;
    unsigned char lt, rt;
    int dirty;
    int dropped;
    uint64_t combo_since;
    int combo_fired;
    uint64_t menu_down;         /* MENU press time; 0 if up or not ours */
    int menu_used;              /* this MENU hold drove the mouse */
    int mono_times;             /* event times are CLOCK_MONOTONIC */
    uint64_t forward_since;     /* when forwarding was last switched on */

    int forwarding;
    int mouse_on;               /* mouse mode toggled on */
    int mode;                   /* MODE_* being sent */
    int neutral_sent;
    unsigned sent_buttons;      /* 1 << BUTTON_* held on the host */
    unsigned sent_keys;         /* P_* bits held as keys on the host */
    uint64_t last_tick, next_tick;
    float acc_x, acc_y, acc_v, acc_h;
    atomic_uint events;
} I = {
    .fd = -1,
    .stop_pipe = { -1, -1 },
    .wake_fd = -1,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .forwarding = 1,
    .mode = MODE_PAD,
};

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int open_pad(void)
{
    char path[32], name[64];
    int i, fd;

    for (i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
            strcmp(name, INPUT_DEVICE_NAME) == 0) {
            int clk = CLOCK_MONOTONIC;

            /* Event times on now_ns()'s clock: see before_resume(). */
            I.mono_times = ioctl(fd, EVIOCSCLOCKID, &clk) == 0;
            return fd;
        }
        close(fd);
    }
    errno = ENODEV;
    return -1;
}

static void read_absinfo(int fd)
{
    static const int codes[] = { ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY,
                                 ABS_RZ };
    unsigned i;

    for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        struct input_absinfo ai;
        struct axis *a = &I.ax[codes[i]];

        a->valid = ioctl(fd, EVIOCGABS(codes[i]), &ai) == 0 &&
                   ai.maximum > ai.minimum;
        if (a->valid) {
            a->min = ai.minimum;
            a->max = ai.maximum;
            a->flat = ai.flat;
        }
    }
}

/* Stick axis to -32768..32767; invert for Y (evdev down is positive). */
static short stick(const struct axis *a, int v, int invert)
{
    long range, s;
    int mid;

    if (!a->valid)
        return 0;
    mid = a->min + (a->max - a->min) / 2;
    if (a->flat > 0 && v > mid - a->flat && v < mid + a->flat)
        return 0;
    range = (long)a->max - a->min;
    s = ((long)(v - a->min) * 65535L) / range - 32768L;
    if (invert)
        s = -s - 1;
    if (s < -32768)
        s = -32768;
    if (s > 32767)
        s = 32767;
    return (short)s;
}

static unsigned char trigger(const struct axis *a, int v)
{
    long s;

    if (!a->valid)
        return 0;
    s = ((long)(v - a->min) * 255L) / ((long)a->max - a->min);
    return (unsigned char)(s < 0 ? 0 : s > 255 ? 255 : s);
}

static unsigned pad_bit(int code)
{
    switch (code) {
    case BTN_B: return P_A;
    case BTN_A: return P_B;
    case BTN_Y: return P_X;
    case BTN_X: return P_Y;
    case BTN_TL: return P_L1;
    case BTN_TR: return P_R1;
    case BTN_START: return P_START;
    case BTN_SELECT: return P_SELECT;
    case BTN_THUMBL: return P_L3;
    case BTN_THUMBR: return P_R3;
    default: return 0;
    }
}

/* Xbox button flags. The pad is positional (the bottom button, labelled B,
 * is Xbox A); --swap-abxy maps by label instead. */
static int pad_flags(unsigned p)
{
    int swap = I.opt.swap_abxy, f = 0;

    if (p & P_A) f |= swap ? A_FLAG : B_FLAG;
    if (p & P_B) f |= swap ? B_FLAG : A_FLAG;
    if (p & P_X) f |= swap ? X_FLAG : Y_FLAG;
    if (p & P_Y) f |= swap ? Y_FLAG : X_FLAG;
    if (p & P_L1) f |= LB_FLAG;
    if (p & P_R1) f |= RB_FLAG;
    if (p & P_START) f |= PLAY_FLAG;
    if (p & P_SELECT) f |= BACK_FLAG;
    if (p & P_L3) f |= LS_CLK_FLAG;
    if (p & P_R3) f |= RS_CLK_FLAG;
    if (p & P_UP) f |= UP_FLAG;
    if (p & P_DOWN) f |= DOWN_FLAG;
    if (p & P_LEFT) f |= LEFT_FLAG;
    if (p & P_RIGHT) f |= RIGHT_FLAG;
    return f;
}

static void raise_event(unsigned ev)
{
    atomic_fetch_or(&I.events, ev);
    if (I.wake_fd >= 0) {
        char c = (char)ev;
        ssize_t r = write(I.wake_fd, &c, 1);

        (void)r;
    }
}

/* Makes the reader recompute its poll timeout. */
static void kick_reader(void)
{
    char c = 1;
    ssize_t r;

    if (I.stop_pipe[1] >= 0) {
        r = write(I.stop_pipe[1], &c, 1);
        (void)r;
    }
}

static void send_neutral(void)
{
    if (!I.neutral_sent) {
        LiSendMultiControllerEvent(0, 0x1, 0, 0, 0, 0, 0, 0, 0);
        I.neutral_sent = 1;
    }
}

static void send_pad(void)
{
    unsigned p = I.pad & ~I.masked, m = I.masked;

    LiSendMultiControllerEvent(0, 0x1, pad_flags(p),
                               m & P_LT ? 0 : I.lt, m & P_RT ? 0 : I.rt,
                               m & P_LS ? 0 : I.lx, m & P_LS ? 0 : I.ly,
                               m & P_RS ? 0 : I.rx, m & P_RS ? 0 : I.ry);
    I.neutral_sent = 0;
}

/* Presses and releases host mouse buttons and keys to match p; returns
 * 1 if anything was pressed. */
static int send_mouse_buttons(unsigned p)
{
    unsigned i;
    int pressed = 0;

    for (i = 0; i < NELEM(mouse_buttons); i++) {
        unsigned bit = 1u << mouse_buttons[i].button;
        int want = (p & mouse_buttons[i].keys) != 0;

        if (want == ((I.sent_buttons & bit) != 0))
            continue;
        LiSendMouseButtonEvent(want ? BUTTON_ACTION_PRESS :
                                      BUTTON_ACTION_RELEASE,
                               mouse_buttons[i].button);
        I.sent_buttons ^= bit;
        pressed |= want;
    }
    for (i = 0; i < NELEM(mouse_keys); i++) {
        unsigned bit = mouse_keys[i].key;
        int want = (p & bit) != 0;

        if (want == ((I.sent_keys & bit) != 0))
            continue;
        LiSendKeyboardEvent((short)(VK_FLAG | mouse_keys[i].vk),
                            want ? KEY_ACTION_DOWN : KEY_ACTION_UP, 0);
        I.sent_keys ^= bit;
        pressed |= want;
    }
    return pressed;
}

static int sticks_active(uint64_t now)
{
    return I.mouse_on ||
           (I.menu_down != 0 && now - I.menu_down >= MENU_STICK_NS);
}

/* 0..1 beyond the dead zone. */
static float beyond_deadzone(float m)
{
    if (m <= STICK_DEADZONE)
        return 0;
    m = (m - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
    return m > 1.0f ? 1.0f : m;
}

static int stick_out(short x, short y)
{
    float fx = x / 32767.0f, fy = y / 32767.0f;

    return fx * fx + fy * fy > STICK_DEADZONE * STICK_DEADZONE;
}

/* Inputs held now, analog ones included. */
static unsigned held(void)
{
    return I.pad | (stick_out(I.lx, I.ly) ? P_LS : 0) |
           (stick_out(I.rx, I.ry) ? P_RS : 0) |
           (I.lt >= TRIG_REST ? P_LT : 0) | (I.rt >= TRIG_REST ? P_RT : 0);
}

/* A MENU hold counts as used (no menu on release) when it drove the
 * mouse; with mouse mode on, MENU is not a shift and a tap always opens
 * the menu. */
static void mark_menu_used(void)
{
    if (I.menu_down != 0 && !I.mouse_on)
        I.menu_used = 1;
}

static void stop_motion(void)
{
    I.acc_x = I.acc_y = I.acc_v = I.acc_h = 0;
    I.last_tick = I.next_tick = 0;
}

/* Accumulates wheel units at rate and sends whole chunks; returns 1 if
 * it sent one. */
static int scroll(float *acc, float rate, float dt, int horizontal)
{
    short n;

    if (rate == 0) {
        *acc = 0;
        return 0;
    }
    *acc += rate * dt;
    if (fabsf(*acc) < SCROLL_MIN)
        return 0;
    n = (short)*acc;
    *acc -= n;
    if (horizontal)
        LiSendHighResHScrollEvent(n);
    else
        LiSendHighResScrollEvent(n);
    return 1;
}

/* Right stick moves the pointer, left stick scrolls; runs every
 * MOUSE_TICK_NS while a stick is out of its dead zone. */
static void mouse_motion(uint64_t now)
{
    int rmask = (I.masked & P_RS) != 0, lmask = (I.masked & P_LS) != 0;
    short lx = lmask ? 0 : I.lx, ly = lmask ? 0 : I.ly;
    float px = rmask ? 0 : I.rx / 32767.0f, py = rmask ? 0 : -I.ry / 32767.0f;
    float m = sqrtf(px * px + py * py), u = beyond_deadzone(m);
    float sv = beyond_deadzone(fabsf(ly / 32767.0f));
    float sh = beyond_deadzone(fabsf(lx / 32767.0f));
    float dt;
    int sent = 0;

    if (u == 0 && sv == 0 && sh == 0) {
        stop_motion();
        return;
    }
    if (!sticks_active(now)) {
        /* A short MENU tap must not move anything: wait for the hold. */
        stop_motion();
        I.next_tick = I.menu_down + MENU_STICK_NS;
        return;
    }
    if (now < I.next_tick)
        return;
    dt = I.last_tick == 0 || now - I.last_tick > MOUSE_MAX_DT_NS ?
         MOUSE_TICK_NS / 1e9f : (float)(now - I.last_tick) / 1e9f;
    I.last_tick = now;
    I.next_tick = now + MOUSE_TICK_NS;
    if (u > 0) {
        /* Cubic-ish curve: slow and precise near the centre. */
        float speed = POINTER_MAX * u * u * (0.3f + 0.7f * u) / m;
        int dx, dy;

        I.acc_x += px * speed * dt;
        I.acc_y += py * speed * dt;
        dx = (int)I.acc_x;
        dy = (int)I.acc_y;
        I.acc_x -= (float)dx;
        I.acc_y -= (float)dy;
        if (dx != 0 || dy != 0) {
            LiSendMouseMoveEvent((short)dx, (short)dy);
            sent = 1;
        }
    } else {
        I.acc_x = I.acc_y = 0;
    }
    /* Stick up scrolls up (positive), stick right scrolls right. */
    sent |= scroll(&I.acc_v, copysignf(SCROLL_MAX * sv * sv, (float)ly),
                   dt, 0);
    sent |= scroll(&I.acc_h, copysignf(SCROLL_MAX * sh * sh, (float)lx),
                   dt, 1);
    if (sent)
        mark_menu_used();
}

static int want_mode(void)
{
    if (!I.forwarding)
        return MODE_OFF;
    return I.mouse_on || I.menu_down != 0 ? MODE_MOUSE : MODE_PAD;
}

/* Sends what the host should see now. Called with lock held after any
 * change to the pad or the mode, and on each poll wakeup. */
static void sync_host(uint64_t now)
{
    int mode = want_mode();

    if (mode != I.mode) {
        if (I.mode == MODE_MOUSE)
            send_mouse_buttons(0);
        I.mode = mode;
        /* Whatever is held now (the button that resumed the stream from
         * the UI menu, a button held while MENU went down, a stick still
         * pushed) stays unseen by the new mode until it is released or
         * back at rest. */
        I.masked = held();
        stop_motion();
        I.dirty = 1;
    }
    I.masked &= held();
    switch (I.mode) {
    case MODE_OFF:
        send_neutral();
        break;
    case MODE_PAD:
        if (I.dirty)
            send_pad();
        break;
    case MODE_MOUSE:
        send_neutral();
        if (send_mouse_buttons(I.pad & ~I.masked))
            mark_menu_used();
        mouse_motion(now);
        break;
    }
    I.dirty = 0;
}

static void set_trigger(unsigned char *t, unsigned bit, unsigned char v)
{
    *t = v;
    if (v >= TRIG_ON)
        I.pad |= bit;
    else if (v < TRIG_OFF)
        I.pad &= ~bit;
}

static unsigned hat_bits(void)
{
    return (I.hat_y < 0 ? P_UP : 0) | (I.hat_y > 0 ? P_DOWN : 0) |
           (I.hat_x < 0 ? P_LEFT : 0) | (I.hat_x > 0 ? P_RIGHT : 0);
}

static int abs_now(int code, int *v)
{
    struct input_absinfo ai;

    if (I.fd < 0 || ioctl(I.fd, EVIOCGABS(code), &ai) != 0)
        return 0;
    *v = ai.value;
    return 1;
}

/* After SYN_DROPPED: reads the pad's state back from the driver (all
 * released if that fails). A MENU release lost in the gap ends the hold
 * without opening the menu. */
static void resync(uint64_t now)
{
    unsigned char keys[KEY_MAX / 8 + 1];
    int code, v, have_keys;

    memset(keys, 0, sizeof(keys));
    have_keys = I.fd >= 0 &&
                ioctl(I.fd, EVIOCGKEY(sizeof(keys)), keys) >= 0;
    I.pad = 0;
    for (code = BTN_A; have_keys && code <= BTN_THUMBR; code++)
        if (keys[code / 8] & (1u << (code % 8)))
            I.pad |= pad_bit(code);
    if (!have_keys || !(keys[BTN_MODE / 8] & (1u << (BTN_MODE % 8))))
        I.menu_down = 0;
    I.lx = abs_now(ABS_X, &v) ? stick(&I.ax[ABS_X], v, 0) : 0;
    I.ly = abs_now(ABS_Y, &v) ? stick(&I.ax[ABS_Y], v, 1) : 0;
    I.rx = abs_now(ABS_RX, &v) ? stick(&I.ax[ABS_RX], v, 0) : 0;
    I.ry = abs_now(ABS_RY, &v) ? stick(&I.ax[ABS_RY], v, 1) : 0;
    I.lt = abs_now(ABS_Z, &v) ? trigger(&I.ax[ABS_Z], v) : 0;
    I.rt = abs_now(ABS_RZ, &v) ? trigger(&I.ax[ABS_RZ], v) : 0;
    I.pad |= (I.lt >= TRIG_ON ? P_L2 : 0) | (I.rt >= TRIG_ON ? P_R2 : 0);
    I.hat_x = abs_now(ABS_HAT0X, &v) ? v : 0;
    I.hat_y = abs_now(ABS_HAT0Y, &v) ? v : 0;
    I.pad |= hat_bits();
    if ((I.pad & COMBO_KEYS) != COMBO_KEYS) {
        I.combo_since = 0;
        I.combo_fired = 0;
    } else if (I.combo_since == 0) {
        I.combo_since = now;
    }
    I.dirty = 1;
}

/* A press made before forwarding resumed belongs to the UI (it resumes
 * the stream on a press). The reader may handle it after the engine has
 * already switched forwarding on, so the event time decides. */
static int before_resume(const struct input_event *ev)
{
    uint64_t t = (uint64_t)ev->input_event_sec * 1000000000ull +
                 (uint64_t)ev->input_event_usec * 1000ull;

    return I.mono_times && t < I.forward_since;
}

static void handle(const struct input_event *ev, uint64_t now)
{
    if (ev->type == EV_SYN) {
        if (ev->code == SYN_DROPPED) {
            I.dropped = 1;
        } else if (ev->code == SYN_REPORT) {
            if (I.dropped) {
                I.dropped = 0;
                resync(now);
            }
            if (I.dirty)
                sync_host(now);
        }
        return;
    }
    if (I.dropped)
        return;
    if (ev->type == EV_KEY) {
        unsigned bit;

        if (ev->code == BTN_MODE) {
            /* Never forwarded. Held, it is a mouse mode shift; a tap
             * that did not drive the mouse opens the menu. A press while
             * paused belongs to the UI (it resumes on the press), so its
             * release must not reopen the menu. */
            if (ev->value == 1) {
                I.menu_down = I.forwarding && !before_resume(ev) ? now : 0;
                I.menu_used = 0;
            } else if (ev->value == 0 && I.menu_down != 0) {
                if (!I.menu_used && now - I.menu_down < MENU_TAP_NS)
                    raise_event(INPUT_EV_MENU);
                I.menu_down = 0;
            }
            sync_host(now);
            return;
        }
        bit = pad_bit(ev->code);
        if (bit == 0 || ev->value == 2)
            return;
        if (ev->value) {
            I.pad |= bit;
            if (before_resume(ev))
                I.masked |= bit;
        } else {
            I.pad &= ~bit;
        }
        I.dirty = 1;
        if ((I.pad & COMBO_KEYS) == COMBO_KEYS) {
            if (I.combo_since == 0)
                I.combo_since = now;
        } else {
            I.combo_since = 0;
            I.combo_fired = 0;
        }
        return;
    }
    if (ev->type == EV_ABS) {
        switch (ev->code) {
        case ABS_X: I.lx = stick(&I.ax[ABS_X], ev->value, 0); break;
        case ABS_Y: I.ly = stick(&I.ax[ABS_Y], ev->value, 1); break;
        case ABS_RX: I.rx = stick(&I.ax[ABS_RX], ev->value, 0); break;
        case ABS_RY: I.ry = stick(&I.ax[ABS_RY], ev->value, 1); break;
        case ABS_Z:
            set_trigger(&I.lt, P_L2, trigger(&I.ax[ABS_Z], ev->value));
            break;
        case ABS_RZ:
            set_trigger(&I.rt, P_R2, trigger(&I.ax[ABS_RZ], ev->value));
            break;
        case ABS_HAT0X: I.hat_x = ev->value; break;
        case ABS_HAT0Y: I.hat_y = ev->value; break;
        default: return;
        }
        if (ev->code == ABS_HAT0X || ev->code == ABS_HAT0Y)
            I.pad = (I.pad & ~P_DPAD) | hat_bits();
        I.dirty = 1;
    }
}

static void check_combo(uint64_t now)
{
    if (I.combo_since != 0 && !I.combo_fired &&
        now - I.combo_since >= COMBO_NS) {
        I.combo_fired = 1;
        I.forwarding = 0;
        I.menu_down = 0;
        sync_host(now);
        raise_event(INPUT_EV_EXIT);
    }
}

static int poll_timeout(uint64_t now)
{
    int t = -1;

    if (I.fd < 0)
        return 1000;
    if (I.combo_since != 0 && !I.combo_fired)
        t = 50;
    if (I.next_tick != 0) {
        int ms = I.next_tick <= now ? 0 :
                 (int)((I.next_tick - now + 999999) / 1000000);

        if (t < 0 || ms < t)
            t = ms;
    }
    return t;
}

/* The pad went away (inputd restart) or failed: release everything on
 * the host until it is back. */
static void pad_lost(void)
{
    close(I.fd);
    pthread_mutex_lock(&I.lock);
    I.fd = -1;
    I.pad = I.masked = 0;
    I.hat_x = I.hat_y = 0;
    I.lx = I.ly = I.rx = I.ry = 0;
    I.lt = I.rt = 0;
    I.menu_down = 0;
    I.combo_since = 0;
    I.combo_fired = 0;
    I.dirty = 1;
    sync_host(now_ns());
    pthread_mutex_unlock(&I.lock);
}

static void *reader(void *arg)
{
    struct input_event evs[64];

    (void)arg;
    while (I.running) {
        struct pollfd p[2];
        int timeout, n, i;
        ssize_t r = 0;

        if (I.fd < 0) {
            I.fd = open_pad();
            if (I.fd >= 0)
                read_absinfo(I.fd);
        }
        p[0].fd = I.stop_pipe[0];
        p[0].events = POLLIN;
        p[0].revents = 0;
        p[1].fd = I.fd;
        p[1].events = POLLIN;
        p[1].revents = 0;
        pthread_mutex_lock(&I.lock);
        timeout = poll_timeout(now_ns());
        pthread_mutex_unlock(&I.lock);
        n = poll(p, I.fd < 0 ? 1 : 2, timeout);
        if (n < 0 && errno != EINTR)
            break;
        if (p[0].revents) {
            char drain[16];

            while (read(I.stop_pipe[0], drain, sizeof(drain)) > 0)
                ;
            if (!I.running)
                break;
        }
        if (I.fd >= 0 && (p[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            pad_lost();
            continue;
        }
        if (I.fd >= 0 && (p[1].revents & POLLIN)) {
            r = read(I.fd, evs, sizeof(evs));
            if (r < 0 && errno != EAGAIN && errno != EINTR) {
                pad_lost();
                continue;
            }
        }
        pthread_mutex_lock(&I.lock);
        {
            uint64_t now = now_ns();

            for (i = 0; r > 0 && i < (int)(r / (ssize_t)sizeof(evs[0]));
                 i++)
                handle(&evs[i], now);
            check_combo(now);
            sync_host(now);
        }
        pthread_mutex_unlock(&I.lock);
    }
    return NULL;
}

int input_start(const struct input_options *opt, int wake_fd)
{
    if (I.running)
        return 0;
    if (opt != NULL)
        I.opt = *opt;
    I.wake_fd = wake_fd;
    I.pad = I.masked = 0;
    I.hat_x = I.hat_y = 0;
    I.lx = I.ly = I.rx = I.ry = 0;
    I.lt = I.rt = 0;
    I.combo_since = 0;
    I.combo_fired = 0;
    I.menu_down = 0;
    I.menu_used = 0;
    I.forward_since = 0;
    I.forwarding = 1;
    I.mouse_on = I.opt.mouse;
    I.mode = MODE_PAD;
    I.neutral_sent = 0;
    I.sent_buttons = I.sent_keys = 0;
    stop_motion();
    atomic_store(&I.events, 0);
    if (pipe2(I.stop_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
        return -1;
    I.fd = open_pad();
    if (I.fd < 0) {
        fprintf(stderr, "input: %s not found\n", INPUT_DEVICE_NAME);
    } else {
        read_absinfo(I.fd);
        printf("input: %s, menu tap pauses, menu held is a mouse, "
               "start+select+L1+R1 held 1 s exits%s%s\n", INPUT_DEVICE_NAME,
               I.opt.swap_abxy ? ", A/B X/Y swapped" : "",
               I.mouse_on ? ", mouse mode on" : "");
        fflush(stdout);
    }
    LiSendControllerArrivalEvent(0, 0x1, LI_CTYPE_XBOX, SUPPORTED_FLAGS,
                                 LI_CCAP_ANALOG_TRIGGERS);
    pthread_mutex_lock(&I.lock);
    sync_host(now_ns());
    pthread_mutex_unlock(&I.lock);
    I.running = 1;
    if (pthread_create(&I.thread, NULL, reader, NULL) != 0) {
        I.running = 0;
        return -1;
    }
    return 0;
}

void input_stop(void)
{
    if (!I.running)
        return;
    I.running = 0;
    kick_reader();
    pthread_join(I.thread, NULL);
    close(I.stop_pipe[0]);
    close(I.stop_pipe[1]);
    I.stop_pipe[0] = I.stop_pipe[1] = -1;
    if (I.fd >= 0)
        close(I.fd);
    I.fd = -1;
}

unsigned input_take_events(void)
{
    return atomic_exchange(&I.events, 0);
}

void input_set_forwarding(int enabled)
{
    pthread_mutex_lock(&I.lock);
    if (enabled && !I.forwarding)
        I.forward_since = now_ns();
    I.forwarding = enabled != 0;
    if (!I.forwarding)
        I.menu_down = 0;
    if (I.running)
        sync_host(now_ns());
    pthread_mutex_unlock(&I.lock);
    kick_reader();
}

void input_set_mouse(int on)
{
    pthread_mutex_lock(&I.lock);
    if (I.mouse_on != (on != 0)) {
        I.mouse_on = on != 0;
        printf("input: mouse mode %s\n", I.mouse_on ? "on" : "off");
        fflush(stdout);
    }
    if (I.running)
        sync_host(now_ns());
    pthread_mutex_unlock(&I.lock);
    kick_reader();
}

void input_send_guide(void)
{
    struct timespec ts = { 0, 100000000 };

    pthread_mutex_lock(&I.lock);
    LiSendMultiControllerEvent(0, 0x1, SPECIAL_FLAG, 0, 0, 0, 0, 0, 0);
    I.neutral_sent = 0;
    pthread_mutex_unlock(&I.lock);
    nanosleep(&ts, NULL);
    pthread_mutex_lock(&I.lock);
    LiSendMultiControllerEvent(0, 0x1, 0, 0, 0, 0, 0, 0, 0);
    I.neutral_sent = 1;
    pthread_mutex_unlock(&I.lock);
}

static volatile sig_atomic_t dump_stop;

static void dump_sig(int sig)
{
    (void)sig;
    dump_stop = 1;
}

static const char *key_name(int code)
{
    switch (code) {
    case BTN_A: return "BTN_A(south)";
    case BTN_B: return "BTN_B(east)";
    case BTN_X: return "BTN_X(0x133)";
    case BTN_Y: return "BTN_Y(0x134)";
    case BTN_TL: return "BTN_TL";
    case BTN_TR: return "BTN_TR";
    case BTN_SELECT: return "BTN_SELECT";
    case BTN_START: return "BTN_START";
    case BTN_MODE: return "BTN_MODE";
    case BTN_THUMBL: return "BTN_THUMBL";
    case BTN_THUMBR: return "BTN_THUMBR";
    default: return "?";
    }
}

int input_dump(void)
{
    static const char *const names[] = { "X", "Y", "Z", "RX", "RY", "RZ" };
    struct input_event evs[64];
    struct sigaction sa;
    int fd, i;

    fd = open_pad();
    if (fd < 0) {
        fprintf(stderr, "input: %s not found\n", INPUT_DEVICE_NAME);
        return 1;
    }
    read_absinfo(fd);
    for (i = 0; i < 6; i++)
        printf("abs %s: %s min=%d max=%d flat=%d\n", names[i],
               I.ax[i].valid ? "ok" : "absent", I.ax[i].min, I.ax[i].max,
               I.ax[i].flat);
    printf("press buttons; Ctrl-C ends\n");
    fflush(stdout);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = dump_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    while (!dump_stop) {
        struct pollfd p = { fd, POLLIN, 0 };
        ssize_t r;

        if (poll(&p, 1, 500) <= 0)
            continue;
        r = read(fd, evs, sizeof(evs));
        for (i = 0; r > 0 && i < (int)(r / (ssize_t)sizeof(evs[0])); i++) {
            const struct input_event *e = &evs[i];

            if (e->type == EV_KEY)
                printf("key %s %s\n", key_name(e->code),
                       e->value == 1 ? "down" : e->value ? "repeat" : "up");
            else if (e->type == EV_ABS)
                printf("abs %#x = %d\n", e->code, e->value);
        }
        fflush(stdout);
    }
    close(fd);
    return 0;
}
