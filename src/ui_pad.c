/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_pad.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "input_evdev.h"    /* INPUT_DEVICE_NAME */

#define REPEAT_DELAY_MS 380
#define REPEAT_RATE_MS 110

static int open_device(void)
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
            strcmp(name, INPUT_DEVICE_NAME) == 0)
            return fd;
        close(fd);
    }
    errno = ENODEV;
    return -1;
}

int ui_pad_open(struct ui_pad *p)
{
    memset(p, 0, sizeof(*p));
    p->fd = -1;
    p->repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    return ui_pad_reopen(p);
}

int ui_pad_reopen(struct ui_pad *p)
{
    int i;

    if (p->fd >= 0)
        return 0;
    p->fd = open_device();
    p->stick_x = p->stick_y = 0;
    for (i = 0; i < 2; i++) {
        struct input_absinfo ai;

        p->abs_min[i] = -32768;
        p->abs_max[i] = 32767;
        if (p->fd >= 0 && ioctl(p->fd, EVIOCGABS(i == 0 ? ABS_X : ABS_Y),
                                &ai) == 0 && ai.maximum > ai.minimum) {
            p->abs_min[i] = ai.minimum;
            p->abs_max[i] = ai.maximum;
        }
    }
    return p->fd >= 0 ? 0 : -1;
}

void ui_pad_close(struct ui_pad *p)
{
    if (p->fd >= 0)
        close(p->fd);
    if (p->repeat_fd >= 0)
        close(p->repeat_fd);
    p->fd = p->repeat_fd = -1;
}

static void arm(struct ui_pad *p, enum ui_key k)
{
    struct itimerspec its = {
        { REPEAT_RATE_MS / 1000, (REPEAT_RATE_MS % 1000) * 1000000L },
        { REPEAT_DELAY_MS / 1000, (REPEAT_DELAY_MS % 1000) * 1000000L },
    };

    p->held = k;
    timerfd_settime(p->repeat_fd, 0, &its, NULL);
}

void ui_pad_stop_repeat(struct ui_pad *p)
{
    struct itimerspec off;

    memset(&off, 0, sizeof(off));
    p->held = UK_NONE;
    if (p->repeat_fd >= 0)
        timerfd_settime(p->repeat_fd, 0, &off, NULL);
}

void ui_pad_drain(struct ui_pad *p)
{
    struct input_event ev[32];

    if (p->fd >= 0)
        while (read(p->fd, ev, sizeof(ev)) > 0)
            ;
    p->stick_x = p->stick_y = 0;
    ui_pad_stop_repeat(p);
}

enum ui_key ui_pad_repeat(struct ui_pad *p)
{
    uint64_t n;

    if (read(p->repeat_fd, &n, sizeof(n)) != (ssize_t)sizeof(n))
        return UK_NONE;
    return p->held;
}

/* Positional codes from trimui_inputd (xpad convention), by label. */
static enum ui_key key_of(int code)
{
    switch (code) {
    case BTN_EAST: return UK_A;     /* right face button, labelled A */
    case BTN_SOUTH: return UK_B;    /* bottom, labelled B */
    case BTN_WEST: return UK_X;     /* 0x134: top, labelled X */
    case BTN_NORTH: return UK_Y;    /* 0x133: left, labelled Y */
    case BTN_TL: return UK_L1;
    case BTN_TR: return UK_R1;
    case BTN_START: return UK_START;
    case BTN_SELECT: return UK_SELECT;
    case BTN_MODE: return UK_MENU;
    default: return UK_NONE;
    }
}

/* -1, 0 or 1 with hysteresis around 50% / 30% of the range. */
static int stick_dir(const struct ui_pad *p, int axis, int v, int prev)
{
    int mid = (p->abs_min[axis] + p->abs_max[axis]) / 2;
    int half = (p->abs_max[axis] - p->abs_min[axis]) / 2;
    int on = half / 2, off = half * 3 / 10;

    v -= mid;
    if (prev != 0)
        return (v * prev > off) ? prev : 0;
    if (v > on)
        return 1;
    if (v < -on)
        return -1;
    return 0;
}

static void press_dir(struct ui_pad *p, enum ui_key k, enum ui_key *keys,
                      int *n, int max)
{
    if (*n < max)
        keys[(*n)++] = k;
    arm(p, k);
}

static void release_dir(struct ui_pad *p, enum ui_key k)
{
    if (p->held == k)
        ui_pad_stop_repeat(p);
}

int ui_pad_read(struct ui_pad *p, enum ui_key *keys, int max)
{
    struct input_event ev[32];
    ssize_t r;
    int n = 0, i;

    while ((r = read(p->fd, ev, sizeof(ev))) > 0) {
        for (i = 0; i < (int)(r / (ssize_t)sizeof(ev[0])); i++) {
            const struct input_event *e = &ev[i];

            if (e->type == EV_KEY) {
                enum ui_key k = key_of(e->code);

                if (k != UK_NONE && e->value == 1 && n < max)
                    keys[n++] = k;
            } else if (e->type == EV_ABS &&
                       (e->code == ABS_HAT0X || e->code == ABS_HAT0Y)) {
                enum ui_key neg = e->code == ABS_HAT0X ? UK_LEFT : UK_UP;
                enum ui_key pos = e->code == ABS_HAT0X ? UK_RIGHT : UK_DOWN;

                if (e->value < 0)
                    press_dir(p, neg, keys, &n, max);
                else if (e->value > 0)
                    press_dir(p, pos, keys, &n, max);
                else {
                    release_dir(p, neg);
                    release_dir(p, pos);
                }
            } else if (e->type == EV_ABS &&
                       (e->code == ABS_X || e->code == ABS_Y)) {
                int axis = e->code == ABS_X ? 0 : 1;
                int *cur = axis == 0 ? &p->stick_x : &p->stick_y;
                int d = stick_dir(p, axis, e->value, *cur);
                enum ui_key neg = axis == 0 ? UK_LEFT : UK_UP;
                enum ui_key pos = axis == 0 ? UK_RIGHT : UK_DOWN;

                if (d == *cur)
                    continue;
                if (*cur != 0)
                    release_dir(p, *cur < 0 ? neg : pos);
                *cur = d;
                if (d != 0)
                    press_dir(p, d < 0 ? neg : pos, keys, &n, max);
            }
        }
    }
    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
        /* Device gone (inputd restarted): ui_pad_reopen finds it again. */
        close(p->fd);
        p->fd = -1;
        ui_pad_stop_repeat(p);
        ui_pad_reopen(p);
    }
    return n;
}

enum ui_key ui_key_from_name(const char *name)
{
    static const struct { const char *n; enum ui_key k; } map[] = {
        { "up", UK_UP }, { "down", UK_DOWN }, { "left", UK_LEFT },
        { "right", UK_RIGHT }, { "a", UK_A }, { "b", UK_B }, { "x", UK_X },
        { "y", UK_Y }, { "l1", UK_L1 }, { "r1", UK_R1 },
        { "start", UK_START }, { "select", UK_SELECT }, { "menu", UK_MENU },
    };
    unsigned i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcasecmp(name, map[i].n) == 0)
            return map[i].k;
    return UK_NONE;
}
