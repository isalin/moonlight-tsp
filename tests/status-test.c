/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for the battery and Wi-Fi status (run by "make check"). */
#include "ui_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static char root[64];

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        failures++;
}

static void level(int dbm, int prev, int want)
{
    char name[96];
    int got = status_wifi_level(dbm, prev);

    snprintf(name, sizeof(name), "level %d dBm from %d = %d (got %d)", dbm,
             prev, want, got);
    check(name, got == want);
}

/* Writes TEXT (and a newline) to ROOT/PATH, making its folder. */
static void put(const char *path, const char *text)
{
    char full[256], dir[256];
    FILE *f;

    snprintf(full, sizeof(full), "%s/%s", root, path);
    snprintf(dir, sizeof(dir), "%s", full);
    for (char *p = dir + strlen(root) + 1; *p != '\0'; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(dir, 0755);
            *p = '/';
        }
    f = fopen(full, "w");
    if (f == NULL) {
        perror(full);
        exit(1);
    }
    fprintf(f, "%s\n", text);
    fclose(f);
}

static void dir(char *buf, size_t n, const char *sub)
{
    snprintf(buf, n, "%s/%s", root, sub);
}

/* "status-test --live [IFNAME]": the real values, for the handheld. */
static int live(const char *ifname)
{
    struct ui_status st;

    status_init(&st, "/sys/class/power_supply", "/sys/class/net", ifname);
    status_poll(&st);
    printf("battery dir %s\nbattery %d%%%s\nwifi %s level %d, %d dBm\n",
           st.bat_dir[0] ? st.bat_dir : "(none)", st.bat_pct,
           st.bat_charging ? " charging" : "", st.ifname, st.wifi_level,
           st.wifi_dbm);
    if (st.note[0] != '\0')
        printf("note: %s\n", st.note);
    status_close(&st);
    return 0;
}

int main(int argc, char **argv)
{
    struct ui_status st;
    char power[96], net[96], cmd[96];
    int r;

    if (argc > 1 && strcmp(argv[1], "--live") == 0)
        return live(argc > 2 ? argv[2] : "wlan0");

    /* ---- bars */
    level(-40, -1, 3);
    level(-60, -1, 3);
    level(-61, -1, 2);
    level(-70, -1, 2);
    level(-71, -1, 1);
    level(-80, -1, 1);
    level(-81, -1, 0);
    level(-127, -1, 0);
    /* down at once */
    level(-60, 3, 3);
    level(-61, 3, 2);
    level(-90, 3, 0);
    /* up only 2 dB past the threshold */
    level(-59, 2, 2);
    level(-58, 2, 3);
    level(-79, 0, 0);
    level(-78, 0, 1);
    level(-59, 1, 2);
    level(-50, 0, 3);
    level(-65, 2, 2);

    snprintf(root, sizeof(root), "/tmp/status-test.XXXXXX");
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    dir(power, sizeof(power), "power");
    dir(net, sizeof(net), "net");

    /* ---- nothing there */
    status_init(&st, "/nonexistent", "/nonexistent", "wlan0");
    r = status_poll(&st);
    check("no folders: no battery", st.bat_pct == STATUS_NONE);
    check("no folders: no Wi-Fi", st.wifi_level == STATUS_NONE);
    check("no folders: nothing changed", r == 0);
    status_close(&st);

    /* ---- battery: the mains supply and an absent battery are skipped */
    put("power/ac/type", "Mains");
    put("power/ac/online", "1");
    put("power/bat0/type", "Battery");
    put("power/bat0/present", "0");
    put("power/bat0/capacity", "11");
    put("power/bat1/type", "Battery");
    put("power/bat1/present", "1");
    put("power/bat1/capacity", "62");
    put("power/bat1/status", "Discharging");
    status_init(&st, power, net, "wlan0");
    check("battery found", strstr(st.bat_dir, "/power/bat1") != NULL);
    r = status_poll(&st);
    check("battery 62%", st.bat_pct == 62 && !st.bat_charging);
    check("battery: changed", r == 1);
    check("battery: same again", status_poll(&st) == 0);

    put("power/bat1/status", "Charging");
    check("charging: changed", status_poll(&st) == 1 && st.bat_charging);
    put("power/bat1/status", "Full");
    put("power/bat1/capacity", "105");
    status_poll(&st);
    check("full: charging, clamped to 100",
          st.bat_charging && st.bat_pct == 100);
    put("power/bat1/status", "Not charging");
    put("power/bat1/capacity", "-3");
    status_poll(&st);
    check("not charging, clamped to 0", !st.bat_charging && st.bat_pct == 0);
    put("power/bat1/capacity", "abc");
    status_poll(&st);
    check("bad capacity: no battery", st.bat_pct == STATUS_NONE);
    put("power/bat1/capacity", "47");
    snprintf(cmd, sizeof(cmd), "%s/power/bat1/status", root);
    unlink(cmd);
    status_poll(&st);
    check("no status file: 47%, not charging",
          st.bat_pct == 47 && !st.bat_charging);
    check("no Wi-Fi folder: no Wi-Fi", st.wifi_level == STATUS_NONE);
    status_close(&st);

    /* ---- Wi-Fi state from operstate */
    put("net/wlan0/operstate", "down");
    status_init(&st, power, net, "wlan0");
    status_poll(&st);
    check("down: not connected", st.wifi_level == STATUS_WIFI_DOWN);
    put("net/wlan0/operstate", "dormant");
    status_poll(&st);
    check("dormant: not connected", st.wifi_level == STATUS_WIFI_DOWN);
    status_close(&st);

    /* "up" on an interface that does not exist: nl80211 cannot answer, so
     * full bars and one note. */
    put("net/mltsp-test0/operstate", "up");
    status_init(&st, power, net, "mltsp-test0");
    r = status_poll(&st);
    check("up without nl80211: full bars",
          st.wifi_level == STATUS_WIFI_BARS && st.wifi_dbm == 0 && r == 1);
    check("up without nl80211: noted", st.note[0] != '\0');
    st.note[0] = '\0';
    status_poll(&st);
    check("up without nl80211: noted once", st.note[0] == '\0');
    put("net/mltsp-test0/operstate", "down");
    check("then down: changed", status_poll(&st) == 1 &&
          st.wifi_level == STATUS_WIFI_DOWN);
    status_close(&st);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0)
        failures++;
    printf("status-test: %d failed\n", failures);
    return failures != 0;
}
