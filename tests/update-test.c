/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for the update manifest and versions (run by "make check"). */
#include "update.h"

#include <stdio.h>
#include <string.h>

#define HASH "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static int failures;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        failures++;
}

static void cmp(const char *a, const char *b, int want)
{
    char name[96];
    int got = update_version_cmp(a, b);

    got = got < 0 ? -1 : got > 0;
    snprintf(name, sizeof(name), "cmp %s %s = %d", a, b, want);
    check(name, got == want);
}

static void valid(const char *v, int want)
{
    char name[96];

    snprintf(name, sizeof(name), "valid \"%s\" = %d", v, want);
    check(name, update_version_valid(v) == want);
}

/* Parses TEXT; WANT_OK says whether it must be accepted. */
static void parse(const char *name, const char *text, int want_ok,
                  struct update_manifest *m)
{
    const char *why = "";
    int rc = update_manifest_parse(text, strlen(text), m, &why);
    char full[128];

    snprintf(full, sizeof(full), "parse %s%s%s", name,
             rc != 0 ? ": " : "", rc != 0 ? why : "");
    check(full, (rc == 0) == want_ok);
}

int main(void)
{
    static char big[UPDATE_MAX_MANIFEST + 16];
    struct update_manifest m;

    valid("0.3.0", 1);
    valid("1", 1);
    valid("1.2.3.4", 1);
    valid("1.2.3.4.5", 0);
    valid("", 0);
    valid("1.", 0);
    valid(".1", 0);
    valid("1..2", 0);
    valid("v1.2", 0);
    valid("1.2-rc1", 0);
    valid("1234567.0", 0);
    valid("123456.0", 1);

    cmp("0.3.0", "0.3.0", 0);
    cmp("0.3", "0.3.0", 0);
    cmp("0.3.0", "0.3", 0);
    cmp("0.3.1", "0.3", 1);
    cmp("0.2.99", "0.3.0", -1);
    cmp("0.10.0", "0.9.0", 1);
    cmp("1.0", "0.99.99", 1);
    cmp("0.2.0", "0.10", -1);

    parse("valid",
          "version=0.3.0\n"
          "zip=trimui-moonlight-tsp-0.3.0.zip\n"
          "size=4312345\n"
          "sha256=" HASH "\n"
          "summary=Checks GitHub for new versions\n", 1, &m);
    check("fields", strcmp(m.version, "0.3.0") == 0 &&
          strcmp(m.zip, "trimui-moonlight-tsp-0.3.0.zip") == 0 &&
          m.size == 4312345 && strcmp(m.sha256, HASH) == 0 &&
          strcmp(m.summary, "Checks GitHub for new versions") == 0);

    parse("crlf, comments, blank lines, unknown keys, no summary",
          "# made by scripts/release.sh\r\n"
          "\r\n"
          "version=0.3.0\r\n"
          "zip=trimui-moonlight-tsp-0.3.0.zip\r\n"
          "size=10\r\n"
          "new_key=whatever\r\n"
          "sha256=" HASH "\r\n", 1, &m);
    check("crlf fields", strcmp(m.zip, "trimui-moonlight-tsp-0.3.0.zip") ==
          0 && m.size == 10 && strcmp(m.sha256, HASH) == 0 &&
          m.summary[0] == '\0');

    parse("no final newline",
          "version=1\nzip=trimui-moonlight-tsp-1.zip\nsize=1\nsha256=" HASH,
          1, &m);

    parse("no version",
          "zip=trimui-moonlight-tsp-0.3.0.zip\nsize=1\nsha256=" HASH "\n",
          0, &m);
    parse("bad version",
          "version=0.3.0-beta\nzip=trimui-moonlight-tsp-0.3.0-beta.zip\n"
          "size=1\nsha256=" HASH "\n", 0, &m);
    parse("zip name for another version",
          "version=0.3.0\nzip=trimui-moonlight-tsp-0.3.1.zip\nsize=1\n"
          "sha256=" HASH "\n", 0, &m);
    parse("zip with a path",
          "version=0.3.0\nzip=../trimui-moonlight-tsp-0.3.0.zip\nsize=1\n"
          "sha256=" HASH "\n", 0, &m);
    parse("no size",
          "version=0.3.0\nzip=trimui-moonlight-tsp-0.3.0.zip\n"
          "sha256=" HASH "\n", 0, &m);
    parse("size 0",
          "version=0.3.0\nzip=trimui-moonlight-tsp-0.3.0.zip\nsize=0\n"
          "sha256=" HASH "\n", 0, &m);
    parse("size not a number",
          "version=0.3.0\nzip=trimui-moonlight-tsp-0.3.0.zip\nsize=4MB\n"
          "sha256=" HASH "\n", 0, &m);
    parse("size negative",
          "version=0.3.0\nzip=trimui-moonlight-tsp-0.3.0.zip\nsize=-1\n"
          "sha256=" HASH "\n", 0, &m);
    parse("size 64 MB",
          "version=0.3.0\nzip=trimui-moonlight-tsp-0.3.0.zip\n"
          "size=67108864\nsha256=" HASH "\n", 0, &m);
    parse("size just under 64 MB",
          "version=0.3.0\nzip=trimui-moonlight-tsp-0.3.0.zip\n"
          "size=67108863\nsha256=" HASH "\n", 1, &m);
    parse("short hash",
          "version=0.3.0\nzip=trimui-moonlight-tsp-0.3.0.zip\nsize=1\n"
          "sha256=0123\n", 0, &m);
    parse("long hash",
          "version=0.3.0\nzip=trimui-moonlight-tsp-0.3.0.zip\nsize=1\n"
          "sha256=" HASH "0\n", 0, &m);
    parse("upper-case hash",
          "version=0.3.0\nzip=trimui-moonlight-tsp-0.3.0.zip\nsize=1\n"
          "sha256=0123456789ABCDEF0123456789abcdef0123456789abcdef"
          "0123456789abcdef\n", 0, &m);
    parse("html instead of a manifest",
          "<!DOCTYPE html>\n<html><body>Not Found</body></html>\n", 0, &m);
    {
        const char text[] = "version=0.3.0\0\nzip=x\n";

        check("nul byte rejected",
              update_manifest_parse(text, sizeof(text) - 1, &m, NULL) != 0);
    }
    memset(big, '#', sizeof(big));
    check("too large rejected",
          update_manifest_parse(big, sizeof(big), &m, NULL) != 0);

    /* The summary: control bytes go, length is capped, UTF-8 is kept. */
    parse("summary with a tab",
          "version=1\nzip=trimui-moonlight-tsp-1.zip\nsize=1\nsha256=" HASH
          "\nsummary=a\tb\x7f" "c\n", 1, &m);
    check("summary cleaned", strcmp(m.summary, "abc") == 0);
    parse("long summary",
          "version=1\nzip=trimui-moonlight-tsp-1.zip\nsize=1\nsha256=" HASH
          "\nsummary=0123456789012345678901234567890123456789"
          "0123456789012345678901234567890123456789\n", 1, &m);
    check("summary capped", strlen(m.summary) == UPDATE_SUMMARY_MAX);
    /* 59 ASCII bytes, then a two-byte letter that does not fit. */
    parse("summary cut in a letter",
          "version=1\nzip=trimui-moonlight-tsp-1.zip\nsize=1\nsha256=" HASH
          "\nsummary=01234567890123456789012345678901234567890123456789"
          "012345678\xc3\xa5zz\n", 1, &m);
    check("summary keeps whole letters", strlen(m.summary) == 59);
    parse("summary with a letter that fits",
          "version=1\nzip=trimui-moonlight-tsp-1.zip\nsize=1\nsha256=" HASH
          "\nsummary=Sm\xc3\xa5 fix\n", 1, &m);
    check("summary utf-8", strcmp(m.summary, "Sm\xc3\xa5 fix") == 0);

    if (failures)
        printf("%d failed\n", failures);
    return failures != 0;
}
