/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Rumble PWM test: a normal file stands in for the GPIO value file. */
#include "../src/rumble.c"

#include <stdlib.h>
#include <sys/wait.h>

static int failed;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failed = 1;
}

static char gpio(const char *path)
{
    char c = '?';
    int fd = open(path, O_RDONLY);

    if (fd >= 0) {
        if (pread(fd, &c, 1, 0) != 1)
            c = '?';
        close(fd);
    }
    return c;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

/* Share of 1 ms samples over MS milliseconds in which the motor is on. */
static double on_share(const char *path, int ms)
{
    int i, on = 0;

    for (i = 0; i < ms; i++) {
        on += gpio(path) == '1';
        sleep_ms(1);
    }
    return (double)on / ms;
}

/* Waits up to MS milliseconds for the motor to be WANT. */
static int wait_gpio(const char *path, char want, int ms)
{
    int i;

    for (i = 0; i < ms && gpio(path) != want; i++)
        sleep_ms(1);
    return gpio(path) == want;
}

/* Like on_share, but the host sends the same value V again before each
 * sample. */
static double share_repeating(const char *path, int ms, unsigned short v)
{
    int i, on = 0;

    for (i = 0; i < ms; i++) {
        rumble_set(v, v);
        on += gpio(path) == '1';
        sleep_ms(1);
    }
    return (double)on / ms;
}

/* 1 if the motor is never on in MS milliseconds of 1 ms samples. */
static int stays_off(const char *path, int ms)
{
    int i;

    for (i = 0; i < ms; i++) {
        if (gpio(path) != '0')
            return 0;
        sleep_ms(1);
    }
    return 1;
}

/* Runs the 0.5 s test in a child, stops it with SIG after the motor is
 * on, and returns 1 if it exited with 128 + SIG and the motor is off. */
static int test_stopped_by(const char *path, int sig)
{
    pid_t pid;
    int st = 0, on;

    fflush(stdout);
    pid = fork();
    if (pid == 0)
        _exit(rumble_test(path, 100));
    on = wait_gpio(path, '1', 400);
    kill(pid, sig);
    waitpid(pid, &st, 0);
    return on && WIFEXITED(st) && WEXITSTATUS(st) == 128 + sig &&
           gpio(path) == '0';
}

static double ms_since(const struct timespec *t0)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1e3 + (t.tv_nsec - t0->tv_nsec) / 1e6;
}

int main(void)
{
    char path[] = "/tmp/rumble-test.XXXXXX";
    int fd = mkstemp(path), p, rising = 1, felt = 1;
    double share;
    struct timespec t0;

    if (fd < 0 || write(fd, "1", 1) != 1) {
        perror("mkstemp");
        return 1;
    }
    close(fd);

    check(rumble_on_ns(0, 100) == 0, "0 is off");
    check(rumble_on_ns(65535, 0) == 0, "setting 0 is off");
    check(rumble_on_ns(1, 100) == RUMBLE_MIN_ON_NS, "a weak value gets the "
          "shortest pulse");
    check(rumble_on_ns(32768, 100) > 9900000 &&
          rumble_on_ns(32768, 100) < 10100000,
          "half strength is half the period");
    check(rumble_on_ns(60000, 100) > 18000000 &&
          rumble_on_ns(60000, 100) < 19000000, "92% is still PWM");
    check(rumble_on_ns(62300, 100) == RUMBLE_PERIOD_NS, "95% and more is "
          "constantly on");
    check(rumble_on_ns(65535, 100) == RUMBLE_PERIOD_NS, "full strength");
    check(rumble_on_ns(65535, 5) == RUMBLE_MIN_ON_NS, "setting 5%: full "
          "strength is the shortest pulse");
    check(rumble_on_ns(65535, 1) == RUMBLE_MIN_ON_NS, "settings under 5% "
          "are 5%");
    check(rumble_on_ns(65535, 50) > 11500000 &&
          rumble_on_ns(65535, 50) < 11700000, "setting 50%: full strength "
          "is 11.6 ms");
    check(rumble_on_ns(32768, 50) > 5700000 &&
          rumble_on_ns(32768, 50) < 5900000, "setting 50%: half strength "
          "is half of that");
    check(rumble_on_ns(65535, 95) == RUMBLE_PERIOD_NS, "setting 95%: "
          "constantly on");
    for (p = 5; p <= 100; p += 5)
        if (rumble_on_ns(1, p) != RUMBLE_MIN_ON_NS ||
            rumble_on_ns(65535, p) < RUMBLE_MIN_ON_NS)
            felt = 0;
    check(felt, "no setting gives a pulse shorter than the motor feels");
    for (p = 5; p < 95; p += 5)
        if (rumble_on_ns(65535, p) >= rumble_on_ns(65535, p + 5))
            rising = 0;
    check(rising, "each 5% step of the setting is stronger, up to 95%");

    check(rumble_start("/nonexistent/gpio/value", 100) == -1, "a missing "
          "GPIO file gives no rumble");
    rumble_set(65535, 0);   /* must do nothing */

    check(rumble_start(path, 100) == 0, "start");
    check(gpio(path) == '0', "start sets the motor off");
    {
        /* A stray close() of the guarded descriptor is refused. */
        int gfd = atomic_load(&R.fd);

        close(gfd);
        check(fcntl(gfd, F_GETFD) >= 0, "a stray close is refused");
    }

    rumble_set(65535, 0);
    check(wait_gpio(path, '1', 200), "full strength: on");
    rumble_set(0, 0);
    check(wait_gpio(path, '0', 200), "0: off");

    rumble_set(0, 32768);
    sleep_ms(30);
    share = on_share(path, 400);
    printf("    half strength: on %.0f%% of the time\n", share * 100);
    check(share > 0.2 && share < 0.8, "half strength (high motor) "
          "makes PWM");

    /* A host that sends its value again (here every 1 ms, more often
     * than the 4 ms pulse) must not keep the motor on. */
    rumble_set(1, 1);
    sleep_ms(30);
    share = share_repeating(path, 400, 1);
    printf("    weak value sent again each 1 ms: on %.0f%% of the time\n",
           share * 100);
    check(share > 0.05 && share < 0.5, "the same value again does not "
          "restart the pulse");

    rumble_set(65535, 65535);
    rumble_pause(1);
    check(wait_gpio(path, '0', 200), "pause stops the motor");
    rumble_pause(0);
    check(wait_gpio(path, '1', 200), "resume gives the last value again");

    rumble_stop();
    check(gpio(path) == '0', "stop sets the motor off");
    rumble_set(65535, 0);   /* after stop: must do nothing */
    sleep_ms(20);
    check(gpio(path) == '0', "no rumble after stop");

    /* A second session in the same process, at the setting 50%. */
    check(rumble_start(path, 50) == 0, "start again at 50%");
    rumble_set(65535, 0);
    sleep_ms(30);
    share = on_share(path, 400);
    printf("    full strength at 50%%: on %.0f%% of the time\n", share * 100);
    check(share > 0.2 && share < 0.8, "setting 50%: full strength makes "
          "PWM");

    /* Emergency off in the middle of PWM: the thread must not turn the
     * motor on again, also with new values and a resume. */
    check(wait_gpio(path, '1', 200), "PWM: on before emergency off");
    rumble_emergency_off();
    check(gpio(path) == '0', "emergency off");
    rumble_set(40000, 0);
    rumble_pause(1);
    rumble_pause(0);
    check(stays_off(path, 100), "after emergency off the motor stays off");
    rumble_stop();
    check(gpio(path) == '0', "second session: off");

    check(rumble_start(path, 100) == 0, "start again at 100%");
    rumble_set(65535, 0);
    check(wait_gpio(path, '1', 200), "a new session after emergency off "
          "rumbles");
    rumble_stop();
    check(gpio(path) == '0', "third session: off");

    /* The Settings test: one 0.5 s rumble. */
    check(rumble_test(path, 0) == 0 && gpio(path) == '0', "test at 0%: "
          "nothing");
    check(rumble_test("/nonexistent/gpio/value", 50) == 1, "test without "
          "the GPIO file fails");
    clock_gettime(CLOCK_MONOTONIC, &t0);
    check(rumble_test(path, 100) == 0, "test at 100%");
    printf("    test took %.0f ms\n", ms_since(&t0));
    check(ms_since(&t0) >= 490 && ms_since(&t0) < 1500, "the test takes "
          "0.5 s");
    check(gpio(path) == '0', "the motor is off after the test");

    /* A signal in the middle of a test (in a child): the motor goes off
     * and the child exits with 128 + the signal. */
    check(test_stopped_by(path, SIGTERM), "SIGTERM stops the test with the "
          "motor off (exit 143)");
    check(test_stopped_by(path, SIGUSR1), "SIGUSR1 stops the test with the "
          "motor off");

    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        if (write(fd, "1", 1) != 1)
            failed = 1;
        close(fd);
    }
    check(rumble_reset(path) == 0 && gpio(path) == '0', "reset writes 0");
    check(rumble_reset("/nonexistent/gpio/value") == -1, "reset of a "
          "missing file fails");

    unlink(path);
    printf(failed ? "rumble-test: FAILED\n" : "rumble-test: all passed\n");
    return failed;
}
