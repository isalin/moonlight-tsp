/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rumble.h"

#include "fdguard.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    int running;            /* under lock */
    atomic_int fd;          /* also read by rumble_emergency_off */
    unsigned strength;      /* 0-65535, under lock */
    unsigned percent;       /* the user's setting, 1-100 */
    int paused;
    unsigned gen;           /* changes on every new value */
    int on;                 /* what the GPIO was last set to */
    atomic_int killed;      /* set by rumble_emergency_off */
} R = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .fd = -1,
};

unsigned rumble_on_ns(unsigned strength, unsigned percent)
{
    unsigned long long on, full;

    if (strength == 0 || percent == 0)
        return 0;
    if (strength > 65535)
        strength = 65535;
    if (percent > 100)
        percent = 100;
    if (percent < 5)
        percent = 5;
    /* A pulse shorter than RUMBLE_MIN_ON_NS does not move the motor, so
     * the settings 5-100% go from that pulse to the full period: each
     * step of the setting then feels different. */
    full = RUMBLE_MIN_ON_NS + (unsigned long long)(RUMBLE_PERIOD_NS -
           RUMBLE_MIN_ON_NS) * (percent - 5) / 95u;
    on = full * strength / 65535u;
    if (on < RUMBLE_MIN_ON_NS)
        on = RUMBLE_MIN_ON_NS;
    /* 95% and more is constantly on: the motor cannot feel a 1 ms gap,
     * and the thread can sleep. */
    if (on >= (unsigned long long)RUMBLE_PERIOD_NS * 95 / 100)
        return RUMBLE_PERIOD_NS;
    return (unsigned)on;
}

static void gpio_write(int fd, int on)
{
    ssize_t r = pwrite(fd, on ? "1" : "0", 1, 0);

    (void)r;
}

/* Under lock. After rumble_emergency_off the motor stays off. */
static void motor(int on)
{
    int fd = atomic_load(&R.fd);

    if (atomic_load(&R.killed))
        on = 0;
    if (fd >= 0 && on != R.on) {
        gpio_write(fd, on);
        R.on = on;
        /* rumble_emergency_off can write its 0 between the check above
         * and this 1; then this thread writes the 0 again. */
        if (on && atomic_load(&R.killed)) {
            gpio_write(fd, 0);
            R.on = 0;
        }
    }
}

static void deadline(struct timespec *ts, unsigned ns)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_nsec += (long)ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

/* Waits ns or until a new value; returns 1 if the value changed. */
static int wait_for(unsigned ns, unsigned gen)
{
    struct timespec ts;

    deadline(&ts, ns);
    while (R.running && R.gen == gen)
        if (pthread_cond_timedwait(&R.cond, &R.lock, &ts) == ETIMEDOUT)
            return 0;
    return 1;
}

static void *pwm(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&R.lock);
    while (R.running) {
        unsigned gen = R.gen;
        unsigned on = R.paused ? 0 : rumble_on_ns(R.strength, R.percent);

        if (on == 0 || on >= RUMBLE_PERIOD_NS) {
            /* Off or constantly on: sleep until the next value. */
            motor(on != 0);
            while (R.running && R.gen == gen)
                pthread_cond_wait(&R.cond, &R.lock);
            continue;
        }
        motor(1);
        if (wait_for(on, gen))
            continue;
        motor(0);
        wait_for(RUMBLE_PERIOD_NS - on, gen);
    }
    motor(0);
    pthread_mutex_unlock(&R.lock);
    return NULL;
}

int rumble_start(const char *path, unsigned percent)
{
    pthread_condattr_t ca;
    int fd;

    if (atomic_load(&R.fd) >= 0)
        return 0;           /* already started */
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "rumble: %s: %s; no rumble\n", path, strerror(errno));
        return -1;
    }
    /* Guarded: a stray close() elsewhere must not let the number go to
     * another file that then gets our writes. */
    fdguard_add(fd, "rumble gpio");
    gpio_write(fd, 0);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&R.cond, &ca);
    pthread_condattr_destroy(&ca);
    R.strength = 0;
    R.percent = percent > 100 ? 100 : percent;
    R.paused = 0;
    R.on = 0;
    R.gen = 0;
    atomic_store(&R.killed, 0);
    atomic_store(&R.fd, fd);
    pthread_mutex_lock(&R.lock);
    R.running = 1;
    pthread_mutex_unlock(&R.lock);
    if (pthread_create(&R.thread, NULL, pwm, NULL) != 0) {
        pthread_mutex_lock(&R.lock);
        R.running = 0;
        pthread_mutex_unlock(&R.lock);
        atomic_store(&R.fd, -1);
        fdguard_close(fd);
        pthread_cond_destroy(&R.cond);
        return -1;
    }
    printf("rumble: on %s at %u%%\n", path, R.percent);
    fflush(stdout);
    return 0;
}

/* Called by common-c's control thread, so the check is under the lock.
 * The same value again does not wake the thread: a new value starts a
 * new period, so a host that sends its value again more often than the
 * pulse length would otherwise keep the motor constantly on. */
void rumble_set(unsigned short low, unsigned short high)
{
    unsigned strength = low > high ? low : high;

    pthread_mutex_lock(&R.lock);
    if (R.running && strength != R.strength) {
        R.strength = strength;
        R.gen++;
        pthread_cond_signal(&R.cond);
    }
    pthread_mutex_unlock(&R.lock);
}

void rumble_pause(int paused)
{
    pthread_mutex_lock(&R.lock);
    if (R.running) {
        R.paused = paused != 0;
        R.gen++;
        pthread_cond_signal(&R.cond);
    }
    pthread_mutex_unlock(&R.lock);
}

void rumble_stop(void)
{
    int fd;

    pthread_mutex_lock(&R.lock);
    if (!R.running) {
        pthread_mutex_unlock(&R.lock);
        return;
    }
    R.running = 0;
    R.gen++;
    pthread_cond_signal(&R.cond);
    pthread_mutex_unlock(&R.lock);
    pthread_join(R.thread, NULL);
    fd = atomic_exchange(&R.fd, -1);
    gpio_write(fd, 0);
    fdguard_close(fd);
    pthread_cond_destroy(&R.cond);
}

void rumble_emergency_off(void)
{
    int fd = atomic_load(&R.fd);

    atomic_store(&R.killed, 1);
    if (fd >= 0)
        gpio_write(fd, 0);
}

int rumble_reset(const char *path)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t r;

    if (fd < 0)
        return -1;
    r = pwrite(fd, "0", 1, 0);
    close(fd);
    return r == 1 ? 0 : -1;
}

int rumble_test(const char *path, unsigned percent)
{
    /* The signals that end a process by default and can be blocked. */
    static const int stop[] = { SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGUSR1,
                                SIGUSR2, SIGALRM, SIGPIPE };
    struct timespec end, now, left;
    sigset_t set;
    unsigned i;
    int sig = 0;

    /* Blocked before the thread starts, so it inherits the mask and the
     * signal can only arrive in the wait below: the motor never stays
     * on when something stops the test. */
    sigemptyset(&set);
    for (i = 0; i < sizeof(stop) / sizeof(stop[0]); i++)
        sigaddset(&set, stop[i]);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    if (percent == 0 || rumble_start(path, percent) != 0)
        return percent == 0 ? 0 : 1;
    rumble_set(65535, 65535);
    deadline(&end, 500000000u);
    for (;;) {
        long long ns;

        clock_gettime(CLOCK_MONOTONIC, &now);
        ns = (long long)(end.tv_sec - now.tv_sec) * 1000000000LL +
             (end.tv_nsec - now.tv_nsec);
        if (ns <= 0)
            break;
        left.tv_sec = (time_t)(ns / 1000000000LL);
        left.tv_nsec = (long)(ns % 1000000000LL);
        sig = sigtimedwait(&set, NULL, &left);
        if (sig > 0 || errno != EINTR)
            break;      /* a signal, or the time is up */
    }
    rumble_stop();
    printf("rumble: off%s\n", sig > 0 ? " (stopped by a signal)" : "");
    return sig > 0 ? 128 + sig : 0;
}
