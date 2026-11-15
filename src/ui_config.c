/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void config_defaults(struct ui_config *c)
{
    memset(c, 0, sizeof(*c));
    c->fps = 60;
    c->bitrate = 10000;
    c->codec = CODEC_AUTO;
    c->wifi_ps_off = 1;
}

/* Copies printable text only (no newlines in the file format). */
static void copy_clean(char *dst, size_t max, const char *src)
{
    size_t n = 0;

    for (; *src != '\0' && n + 1 < max; src++)
        if ((unsigned char)*src >= 0x20 && *src != 0x7f)
            dst[n++] = *src;
    while (n > 0 && dst[n - 1] == ' ')
        n--;
    dst[n] = '\0';
}

static int clamp(int v, int lo, int hi, int def)
{
    return v < lo || v > hi ? def : v;
}

int config_load(struct ui_config *c, const char *path)
{
    char line[256];
    FILE *f;

    config_defaults(c);
    f = fopen(path, "re");
    if (f == NULL)
        return errno == ENOENT ? 0 : -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *eq = strchr(line, '='), *v;

        line[strcspn(line, "\r\n")] = '\0';
        if (eq == NULL || line[0] == '#')
            continue;
        *eq = '\0';
        v = eq + 1;
        if (strcmp(line, "fps") == 0)
            c->fps = atoi(v) == 30 ? 30 : 60;
        else if (strcmp(line, "bitrate") == 0)
            c->bitrate = clamp(atoi(v), 1000, 50000, 10000);
        else if (strcmp(line, "codec") == 0)
            c->codec = strcmp(v, "hevc") == 0 ? CODEC_HEVC :
                       strcmp(v, "h264") == 0 ? CODEC_H264 : CODEC_AUTO;
        else if (strcmp(line, "swap_abxy") == 0)
            c->swap_abxy = atoi(v) != 0;
        else if (strcmp(line, "quit_after") == 0)
            c->quit_after = atoi(v) != 0;
        else if (strcmp(line, "wifi_ps_off") == 0)
            c->wifi_ps_off = atoi(v) != 0;
        else if (strcmp(line, "sops") == 0)
            c->sops = atoi(v) != 0;
        else if (strcmp(line, "last_host") == 0)
            c->last_host = atoi(v);
        else if (strcmp(line, "host") == 0 && c->nhosts < CFG_MAX_HOSTS) {
            /* host=ADDR NAME */
            char *sp = strchr(v, ' ');

            if (sp != NULL)
                *sp++ = '\0';
            if (v[0] != '\0')
                config_add_host(c, v, sp != NULL ? sp : v);
        }
    }
    fclose(f);
    if (c->last_host < 0 || c->last_host >= c->nhosts)
        c->last_host = 0;
    return 0;
}

int config_save(const struct ui_config *c, const char *path)
{
    char tmp[512];
    FILE *f;
    int i, ok;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "we");
    if (f == NULL)
        return -1;
    fprintf(f, "# moonlight-tsp settings\n");
    fprintf(f, "fps=%d\nbitrate=%d\ncodec=%s\n", c->fps, c->bitrate,
            c->codec == CODEC_HEVC ? "hevc" :
            c->codec == CODEC_H264 ? "h264" : "auto");
    fprintf(f, "swap_abxy=%d\nquit_after=%d\nwifi_ps_off=%d\nsops=%d\n",
            c->swap_abxy, c->quit_after, c->wifi_ps_off, c->sops);
    fprintf(f, "last_host=%d\n", c->last_host);
    for (i = 0; i < c->nhosts; i++)
        fprintf(f, "host=%s %s\n", c->hosts[i].addr, c->hosts[i].name);
    ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0)
        ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int config_add_host(struct ui_config *c, const char *addr, const char *name)
{
    char a[64];
    int i;

    copy_clean(a, sizeof(a), addr);
    if (strchr(a, ' ') != NULL || a[0] == '\0')
        return -1;
    for (i = 0; i < c->nhosts; i++)
        if (strcmp(c->hosts[i].addr, a) == 0)
            break;
    if (i == c->nhosts) {
        if (c->nhosts == CFG_MAX_HOSTS)
            return -1;
        c->nhosts++;
        snprintf(c->hosts[i].addr, sizeof(c->hosts[i].addr), "%s", a);
    }
    copy_clean(c->hosts[i].name, sizeof(c->hosts[i].name),
               name != NULL && name[0] != '\0' ? name : a);
    return i;
}

void config_remove_host(struct ui_config *c, int i)
{
    if (i < 0 || i >= c->nhosts)
        return;
    memmove(&c->hosts[i], &c->hosts[i + 1],
            (size_t)(c->nhosts - i - 1) * sizeof(c->hosts[0]));
    c->nhosts--;
    if (c->last_host >= c->nhosts)
        c->last_host = c->nhosts > 0 ? c->nhosts - 1 : 0;
}
