/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_UI_STATUS_H
#define MOONLIGHT_TSP_UI_STATUS_H

/*
 * Battery and Wi-Fi state for the header of the menus.
 *
 * The battery comes from sysfs (power_supply). The Wi-Fi signal comes from
 * an nl80211 station dump: a read of the table that the driver already
 * keeps for the access point. It starts no scan, sends nothing over the
 * air and does not go through wpa_supplicant. No process is started.
 */
#define STATUS_NONE (-2)        /* no battery or no Wi-Fi interface */
#define STATUS_WIFI_DOWN (-1)   /* interface, but not connected */
#define STATUS_WIFI_BARS 3

struct ui_status {
    int bat_pct;        /* 0-100, or STATUS_NONE */
    int bat_charging;   /* battery status "Charging" or "Full" */
    int wifi_level;     /* 0-3 bars, STATUS_WIFI_DOWN or STATUS_NONE */
    int wifi_dbm;       /* signal of the access point; 0 if not known */

    /* private */
    char bat_dir[256];
    char net_dir[128];
    char ifname[32];
    int nl_fd;
    int nl_family;      /* 0: not resolved yet */
    unsigned nl_seq;
    int nl_noted;       /* the netlink failure was put in NOTE */

    /* A problem worth one log line (status_poll sets it, the caller logs
     * and clears it). */
    char note[96];
};

/*
 * POWER_DIR is normally /sys/class/power_supply and NET_DIR
 * /sys/class/net; IFNAME is the Wi-Fi interface (wlan0). Tests pass
 * temporary folders.
 */
void status_init(struct ui_status *st, const char *power_dir,
                 const char *net_dir, const char *ifname);

/* Reads everything again. Returns 1 if what the header shows changed. */
int status_poll(struct ui_status *st);

/*
 * Bars for a signal of DBM: 3 at -60 dBm or more, 2 at -70, 1 at -80,
 * else 0. PREV is the bars shown now (or < 0): a level goes up only 2 dB
 * past its threshold, so the icon does not flap.
 */
int status_wifi_level(int dbm, int prev);

void status_close(struct ui_status *st);

#endif
