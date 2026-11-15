/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_UI_CONFIG_H
#define MOONLIGHT_TSP_UI_CONFIG_H

#define CFG_MAX_HOSTS 16

enum { CODEC_AUTO, CODEC_HEVC, CODEC_H264 };

struct host_entry {
    char addr[64];
    char name[64];
};

struct ui_config {
    int fps;            /* 30 or 60 */
    int bitrate;        /* kbps */
    int codec;          /* CODEC_* */
    int swap_abxy;
    int quit_after;     /* quit the host app when the stream ends */
    int wifi_ps_off;    /* Wi-Fi power save off while streaming */
    int sops;           /* let the host optimise game settings */
    int last_host;
    int nhosts;
    struct host_entry hosts[CFG_MAX_HOSTS];
};

void config_defaults(struct ui_config *c);

/* Missing file: defaults, returns 0. Unknown keys are ignored. */
int config_load(struct ui_config *c, const char *path);

/* Atomic (temporary file, fsync, rename). */
int config_save(const struct ui_config *c, const char *path);

/* Returns the host's index (existing entry for the same address updated). */
int config_add_host(struct ui_config *c, const char *addr, const char *name);
void config_remove_host(struct ui_config *c, int i);

#endif
