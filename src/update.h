/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_UPDATE_H
#define MOONLIGHT_TSP_UPDATE_H

#include <stddef.h>

/*
 * In-app updates from GitHub releases. Each release vX.Y.Z carries the app
 * zip and a small "update.txt" manifest:
 *
 *   version=0.3.0
 *   zip=trimui-moonlight-tsp-0.3.0.zip
 *   size=4312345
 *   sha256=<64 hex digits>
 *   summary=One line about the release
 *
 * There is no signature: trust comes from HTTPS to GitHub. The size and
 * SHA-256 only catch damaged or cut-off downloads.
 */

#define UPDATE_ZIP_PREFIX "trimui-moonlight-tsp-"
#define UPDATE_MAX_ZIP (64ul * 1024 * 1024)
#define UPDATE_MAX_MANIFEST 8192
#define UPDATE_SUMMARY_MAX 60
/* The stage folder's name; the engine refuses to clean any other. */
#define UPDATE_STAGE_NAME ".moonlight-tsp-update"

struct update_manifest {
    char version[24];
    char zip[96];
    unsigned long size;
    char sha256[65];            /* lower case */
    char summary[UPDATE_SUMMARY_MAX + 4];
};

/* 1 if V is 1 to 4 dot-separated numbers of at most 6 digits each. */
int update_version_valid(const char *v);

/* <0, 0 or >0 like strcmp, numerically; missing parts count as 0. Both
 * versions must be valid. */
int update_version_cmp(const char *a, const char *b);

/*
 * Parses a manifest of LEN bytes. Blank lines, "#" comments and unknown
 * keys are ignored; CRLF line ends are accepted. Returns 0, or -1 with a
 * short reason in WHY.
 */
int update_manifest_parse(const char *text, size_t len,
                          struct update_manifest *m, const char **why);

struct update_opts {
    const char *base_url;       /* https://github.com/OWNER/REPO/releases */
    const char *ca_file;        /* PEM root certificates */
    const char *stage;          /* .../.moonlight-tsp-update */
    const char *unzip;          /* unzip program */
    const char *version;        /* the running version */
    void (*send)(const char *fmt, ...)
        __attribute__((format(printf, 1, 2)));    /* IPC line to the UI */
};

/*
 * update-check: reads the newest release's manifest and reports
 * "update available VER SIZE SUMMARY", "update none VER", "update none -"
 * (no release yet) or "update error TEXT". Returns 0 unless it failed.
 */
int update_check(const struct update_opts *o);

/*
 * update-fetch VERSION: downloads, checks and unpacks that release into
 * STAGE/new, proves that its programs start, writes STAGE/ready
 * ("NEW OLD") and reports "update staged VER". On the way it reports
 * "update progress GOT TOTAL" and "update verify"; on failure
 * "update error TEXT". The launcher installs a staged update.
 */
int update_fetch(const struct update_opts *o, const char *version);

#endif
