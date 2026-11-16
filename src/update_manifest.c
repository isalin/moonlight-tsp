/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The pure part of the updater: version numbers and the release manifest.
 * tests/update-test.c covers it on the build host.
 */
#include "update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int update_version_valid(const char *v)
{
    int parts = 0, digits = 0;

    if (v == NULL)
        return 0;
    for (;; v++) {
        if (*v >= '0' && *v <= '9') {
            if (++digits > 6)
                return 0;
        } else if (*v == '.' || *v == '\0') {
            if (digits == 0 || ++parts > 4)
                return 0;
            if (*v == '\0')
                return 1;
            digits = 0;
        } else {
            return 0;
        }
    }
}

int update_version_cmp(const char *a, const char *b)
{
    while (*a != '\0' || *b != '\0') {
        unsigned long x = strtoul(a, (char **)&a, 10);
        unsigned long y = strtoul(b, (char **)&b, 10);

        if (x != y)
            return x < y ? -1 : 1;
        if (*a == '.')
            a++;
        if (*b == '.')
            b++;
    }
    return 0;
}

static int is_hex64(const char *s)
{
    int n;

    for (n = 0; s[n] != '\0'; n++)
        if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f')))
            return 0;
    return n == 64;
}

/* Printable text only, at most MAX bytes, never cutting a UTF-8 letter. */
static void copy_summary(char *dst, size_t max, const char *src)
{
    size_t n = 0;

    for (; *src != '\0' && n < max; src++)
        if ((unsigned char)*src >= 0x20 && *src != 0x7f)
            dst[n++] = *src;
    if (((unsigned char)*src & 0xc0) == 0x80) {
        /* Cut inside a letter: drop its first bytes too. */
        while (n > 0 && ((unsigned char)dst[n - 1] & 0xc0) == 0x80)
            n--;
        if (n > 0)
            n--;
    }
    while (n > 0 && dst[n - 1] == ' ')
        n--;
    dst[n] = '\0';
}

int update_manifest_parse(const char *text, size_t len,
                          struct update_manifest *m, const char **why)
{
    char line[256], expect[sizeof(m->zip)];
    int have_size = 0;
    size_t i = 0;
    const char *dummy;

    if (why == NULL)
        why = &dummy;
    memset(m, 0, sizeof(*m));
    if (len > UPDATE_MAX_MANIFEST) {
        *why = "manifest too large";
        return -1;
    }
    while (i < len) {
        size_t n = 0;
        char *eq, *v;

        while (i < len && text[i] != '\n') {
            if (text[i] == '\0') {
                *why = "manifest is not text";
                return -1;
            }
            if (n + 1 < sizeof(line))
                line[n++] = text[i];
            i++;
        }
        i++;
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' '))
            n--;
        line[n] = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;
        eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        v = eq + 1;
        if (strcmp(line, "version") == 0) {
            snprintf(m->version, sizeof(m->version), "%s", v);
            if (strlen(v) >= sizeof(m->version))
                m->version[0] = '\0';
        } else if (strcmp(line, "zip") == 0) {
            snprintf(m->zip, sizeof(m->zip), "%s", v);
        } else if (strcmp(line, "size") == 0) {
            char *end;

            m->size = strtoul(v, &end, 10);
            have_size = v[0] >= '0' && v[0] <= '9' && *end == '\0';
        } else if (strcmp(line, "sha256") == 0) {
            snprintf(m->sha256, sizeof(m->sha256), "%s", v);
            if (strlen(v) != 64)
                m->sha256[0] = '\0';
        } else if (strcmp(line, "summary") == 0) {
            copy_summary(m->summary, UPDATE_SUMMARY_MAX, v);
        }
    }
    if (!update_version_valid(m->version)) {
        *why = "no valid version";
        return -1;
    }
    snprintf(expect, sizeof(expect), UPDATE_ZIP_PREFIX "%s.zip", m->version);
    if (strcmp(m->zip, expect) != 0) {
        *why = "zip name does not match the version";
        return -1;
    }
    if (!have_size || m->size == 0 || m->size >= UPDATE_MAX_ZIP) {
        *why = "no valid size";
        return -1;
    }
    if (!is_hex64(m->sha256)) {
        *why = "no valid sha256";
        return -1;
    }
    return 0;
}
