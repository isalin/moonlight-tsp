/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * update-check and update-fetch (see update.h). libcurl and libcrypto are
 * the engine's static copies. This curl has no built-in CA location, so
 * the system's root certificate file is always given explicitly.
 */
#include "update.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <openssl/evp.h>

#define APP_DIR "Apps/MoonlightTSP"

static const struct update_opts *opt;

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* One line to the log (stdout) and to the UI. */
static void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void report(const char *fmt, ...)
{
    char line[400];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    fflush(stdout);
    if (opt->send != NULL)
        opt->send("%s", line);
}

static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int fail(const char *fmt, ...)
{
    char text[300];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    report("update error %s", text);
    return 1;
}

/* ---- HTTPS ------------------------------------------------------------ */

struct sink {
    char *mem;                  /* the manifest, or NULL for a file */
    int fd;
    EVP_MD_CTX *md;
    unsigned long got, limit, total;
    int too_big, write_err;
    uint64_t last_progress;
};

static size_t on_data(char *p, size_t size, size_t n, void *arg)
{
    struct sink *s = arg;
    size_t len = size * n, off = 0;
    uint64_t now;

    if (len > s->limit - s->got) {
        s->too_big = 1;
        return 0;
    }
    if (s->mem != NULL) {
        memcpy(s->mem + s->got, p, len);
    } else {
        while (off < len) {
            ssize_t w = write(s->fd, p + off, len - off);

            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                s->write_err = w < 0 ? errno : ENOSPC;
                return 0;
            }
            off += (size_t)w;
        }
        EVP_DigestUpdate(s->md, p, len);
    }
    s->got += (unsigned long)len;
    now = now_ms();
    if (s->total != 0 && now - s->last_progress >= 250) {
        s->last_progress = now;
        report("update progress %lu %lu", s->got, s->total);
    }
    return len;
}

/* TIMEOUT_S 0: no overall limit (a stall still ends it after 30 s). */
static CURLcode get(const char *url, struct sink *s, long timeout_s,
                    long *http)
{
    char ua[64], err[CURL_ERROR_SIZE] = "";
    CURLcode rc;
    CURL *c;

    *http = 0;
    c = curl_easy_init();
    if (c == NULL)
        return CURLE_OUT_OF_MEMORY;
    snprintf(ua, sizeof(ua), "moonlight-tsp/%s", opt->version);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CAINFO, opt->ca_file);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    if (timeout_s > 0)
        curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, http);
    printf("get %s: %s, HTTP %ld, %lu bytes%s%s\n", url,
           rc == CURLE_OK ? "ok" : curl_easy_strerror(rc), *http, s->got,
           err[0] ? ": " : "", err);
    curl_easy_cleanup(c);
    return rc;
}

#define STOPPED "The download stopped. Make sure that Wi-Fi is on, then try " \
                "again."

static int net_fail(CURLcode rc, long http)
{
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
        return fail("The connection to GitHub failed. Make sure that "
                    "Wi-Fi is on.");
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CACERT_BADFILE:
        return fail("The secure connection to GitHub failed. Make sure "
                    "that the date and time of this device are correct.");
    case CURLE_HTTP_RETURNED_ERROR:
        return fail("GitHub answered with HTTP error %ld.", http);
    case CURLE_PARTIAL_FILE:
        return fail(STOPPED);
    case CURLE_UNSUPPORTED_PROTOCOL:
        return fail("The update address does not use HTTPS.");
    default:
        return fail("The download failed (%s).", curl_easy_strerror(rc));
    }
}

static int prepare(void)
{
    static int done;

    if (access(opt->ca_file, R_OK) != 0)
        return fail("This device has no root certificates (%s).",
                    opt->ca_file);
    if (!done && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return fail("The network library did not start.");
    done = 1;
    return 0;
}

/* Fetches and parses a manifest; 404 returns 2 without a report. */
static int get_manifest(const char *url, struct update_manifest *m)
{
    static char text[UPDATE_MAX_MANIFEST];
    struct sink s = { .mem = text, .fd = -1, .limit = sizeof(text) };
    const char *why = "";
    long http;
    CURLcode rc;

    rc = get(url, &s, 15, &http);
    if (rc == CURLE_HTTP_RETURNED_ERROR && http == 404)
        return 2;
    if (s.too_big ||
        (rc == CURLE_OK && update_manifest_parse(text, s.got, m, &why) != 0)) {
        printf("manifest: %s\n", s.too_big ? "too large" : why);
        return fail("The update information on GitHub is not valid.");
    }
    if (rc != CURLE_OK)
        return net_fail(rc, http);
    printf("manifest: version %s, %s, %lu bytes, sha256 %s\n", m->version,
           m->zip, m->size, m->sha256);
    return 0;
}

int update_check(const struct update_opts *o)
{
    struct update_manifest m;
    char url[1024];
    int rc;

    opt = o;
    if (prepare() != 0)
        return 1;
    snprintf(url, sizeof(url), "%s/latest/download/update.txt", o->base_url);
    rc = get_manifest(url, &m);
    if (rc == 2) {
        report("update none -");
        return 0;
    }
    if (rc != 0)
        return 1;
    if (update_version_cmp(m.version, o->version) > 0)
        report("update available %s %lu %s", m.version, m.size, m.summary);
    else
        report("update none %s", m.version);
    return 0;
}

/* ---- the stage folder -------------------------------------------------- */

static int rm_entry(const char *path, const struct stat *st, int type,
                    struct FTW *f)
{
    (void)st;
    (void)type;
    (void)f;
    if (remove(path) != 0 && errno != ENOENT)
        printf("remove %s: %s\n", path, strerror(errno));
    return 0;
}

static void rm_tree(const char *path)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode))
        nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
    else
        unlink(path);
}

static void stage_path(char *buf, size_t max, const char *name)
{
    snprintf(buf, max, "%s/%s", opt->stage, name);
}

/* Removes everything an earlier update left in the stage folder. */
static void stage_clean(void)
{
    static const char *const names[] = {
        "download.zip.part", "download.zip", "new.tmp", "new", "ready",
        "ready.tmp", "previous", "failed", "started",
    };
    char path[PATH_MAX];
    unsigned i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        stage_path(path, sizeof(path), names[i]);
        rm_tree(path);
    }
}

static int stage_name_ok(const char *stage)
{
    const char *b = strrchr(stage, '/');

    return b != NULL && strcmp(b + 1, UPDATE_STAGE_NAME) == 0;
}

static int mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    char *p;

    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf))
        return -1;
    for (p = buf + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return mkdir(buf, 0755) != 0 && errno != EEXIST ? -1 : 0;
}

/*
 * Runs ARGV (an absolute path) for at most TIMEOUT_MS. With OUT, its
 * standard output is captured (MAX bytes at most); otherwise it goes to
 * the log. Returns the exit status, or -1.
 */
static int run(char *const argv[], int timeout_ms, char *out, size_t max)
{
    uint64_t end = now_ms() + (uint64_t)timeout_ms;
    size_t got = 0;
    int p[2] = { -1, -1 }, st = 0;
    pid_t pid;

    if (out != NULL && pipe2(p, O_CLOEXEC) != 0)
        return -1;
    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        if (out != NULL) {
            close(p[0]);
            close(p[1]);
        }
        return -1;
    }
    if (pid == 0) {
        sigset_t none;
        int nul = open("/dev/null", O_RDONLY);

        /* The engine blocks these for its signal thread. */
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (nul >= 0)
            dup2(nul, 0);
        if (out != NULL)
            dup2(p[1], 1);
        execv(argv[0], argv);
        _exit(127);
    }
    if (out != NULL) {
        close(p[1]);
        for (;;) {
            struct pollfd pf = { .fd = p[0], .events = POLLIN };
            uint64_t now = now_ms();
            ssize_t n;

            if (now >= end || poll(&pf, 1, (int)(end - now)) <= 0)
                break;
            n = read(p[0], out + got, max - 1 - got);
            if (n <= 0)
                break;
            got += (size_t)n;
            if (got >= max - 1)
                break;
        }
        out[got] = '\0';
        close(p[0]);
    }
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        struct timespec ts = { 0, 20 * 1000000L };

        if (r == pid)
            break;
        if (r < 0 && errno != EINTR)
            return -1;
        if (now_ms() >= end) {
            printf("run %s: no exit in %d ms, killing\n", argv[0],
                   timeout_ms);
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return -1;
        }
        nanosleep(&ts, NULL);
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Checks that the staged PROGRAM prints "PROGRAM VERSION" for --version. */
static int starts(const char *dir, const char *program, const char *version)
{
    char path[PATH_MAX], out[256], expect[128];
    char *argv[] = { path, "--version", NULL };
    int rc;

    snprintf(path, sizeof(path), "%s/%s", dir, program);
    rc = run(argv, 10000, out, sizeof(out));
    out[strcspn(out, "\n")] = '\0';
    snprintf(expect, sizeof(expect), "%s %s", program, version);
    printf("%s --version: status %d, \"%s\"\n", program, rc, out);
    return rc == 0 && strcmp(out, expect) == 0 ? 0 : -1;
}

static int write_ready(const char *version)
{
    char tmp[PATH_MAX], path[PATH_MAX], line[64];
    int fd, n, ok;

    stage_path(tmp, sizeof(tmp), "ready.tmp");
    stage_path(path, sizeof(path), "ready");
    n = snprintf(line, sizeof(line), "%s %s\n", version, opt->version);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    ok = write(fd, line, (size_t)n) == n && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int stage_made;      /* the stage folder is ours to clean */

static int fetch(const struct update_opts *o, const char *version)
{
    static const char *const need[] = {
        "launch.sh", "update.sh", "config.json", "moonlight-tsp-ui",
        "moonlight-tsp-stream",
    };
    struct update_manifest m;
    struct statvfs vfs;
    struct sink s;
    char url[1024], part[PATH_MAX], zip[PATH_MAX], tmpdir[PATH_MAX];
    char app[PATH_MAX], newdir[PATH_MAX], hex[65];
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned mdlen = 0, i;
    unsigned long long avail, want;
    long http;
    CURLcode rc;
    int r;

    opt = o;
    if (!update_version_valid(version))
        return fail("Version \"%s\" is not valid.", version);
    if (!stage_name_ok(o->stage) || strlen(o->stage) > 1024)
        return fail("The stage folder must be called " UPDATE_STAGE_NAME
                    ".");
    if (prepare() != 0)
        return 1;

    /* The tag's own manifest: the newest release can change meanwhile. */
    snprintf(url, sizeof(url), "%s/download/v%s/update.txt", o->base_url,
             version);
    r = get_manifest(url, &m);
    if (r == 2)
        return fail("Version %s is not on GitHub.", version);
    if (r != 0)
        return 1;
    if (update_version_cmp(m.version, version) != 0) {
        printf("manifest: version %s in the v%s release\n", m.version,
               version);
        return fail("The update information on GitHub is not valid.");
    }

    if (mkdir_p(o->stage) != 0)
        return fail("Cannot make the folder %s (%s).", o->stage,
                    strerror(errno));
    stage_made = 1;
    stage_clean();
    want = 3ull * m.size + 8ull * 1024 * 1024;
    if (statvfs(o->stage, &vfs) == 0) {
        avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
        printf("space: %llu MB free, %llu MB needed\n", avail >> 20,
               want >> 20);
        if (avail < want)
            return fail("There is not enough free space on the SD card "
                        "(%llu MB needed).", (want + (1 << 20) - 1) >> 20);
    }

    stage_path(part, sizeof(part), "download.zip.part");
    stage_path(zip, sizeof(zip), "download.zip");
    memset(&s, 0, sizeof(s));
    s.limit = s.total = m.size;
    s.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (s.fd < 0)
        return fail("Cannot write to the SD card (%s).", strerror(errno));
    s.md = EVP_MD_CTX_new();
    if (s.md == NULL || EVP_DigestInit_ex(s.md, EVP_sha256(), NULL) != 1) {
        close(s.fd);
        return fail("The checksum library did not start.");
    }
    report("update progress 0 %lu", m.size);
    snprintf(url, sizeof(url), "%s/download/v%s/%s", o->base_url, version,
             m.zip);
    rc = get(url, &s, 0, &http);
    report("update progress %lu %lu", s.got, m.size);
    if (fsync(s.fd) != 0 && s.write_err == 0)
        s.write_err = errno;
    if (close(s.fd) != 0 && s.write_err == 0)
        s.write_err = errno;
    EVP_DigestFinal_ex(s.md, md, &mdlen);
    EVP_MD_CTX_free(s.md);
    if (s.write_err != 0)
        return fail("Cannot write the download to the SD card (%s).",
                    strerror(s.write_err));
    if (s.too_big)
        return fail("The download is damaged (too large). Try again.");
    if (rc != CURLE_OK && rc != CURLE_HTTP_RETURNED_ERROR && s.got > 0)
        return fail(STOPPED);   /* cut off, not refused */
    if (rc != CURLE_OK)
        return net_fail(rc, http);
    for (i = 0; i < mdlen && i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", md[i]);
    hex[64] = '\0';
    printf("download: %lu bytes, sha256 %s\n", s.got, hex);
    if (s.got != m.size || strcmp(hex, m.sha256) != 0)
        return fail("The download is damaged. Try again.");
    report("update verify");
    if (rename(part, zip) != 0)
        return fail("Cannot write to the SD card (%s).", strerror(errno));

    stage_path(tmpdir, sizeof(tmpdir), "new.tmp");
    {
        char *argv[] = { (char *)o->unzip, "-q", "-o", zip, "-d", tmpdir,
                         NULL };

        r = run(argv, 60000, NULL, 0);
        printf("unzip: status %d\n", r);
        if (r != 0)
            return fail("The download did not unpack (status %d).", r);
    }
    if (snprintf(app, sizeof(app), "%s/" APP_DIR, tmpdir) >= (int)sizeof(app))
        return fail("The stage folder name is too long.");
    for (i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        char path[PATH_MAX];
        struct stat st;

        if (snprintf(path, sizeof(path), "%s/%s", app, need[i]) >=
                (int)sizeof(path) ||
            stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            printf("package: no %s\n", path);
            return fail("The download is not a Moonlight HW package.");
        }
    }
    if (starts(app, "moonlight-tsp-stream", version) != 0 ||
        starts(app, "moonlight-tsp-ui", version) != 0)
        return fail("Version %s does not start on this device.", version);

    stage_path(newdir, sizeof(newdir), "new");
    if (rename(tmpdir, newdir) != 0 || write_ready(version) != 0)
        return fail("Cannot write to the SD card (%s).", strerror(errno));
    unlink(zip);
    sync();
    report("update staged %s", version);
    return 0;
}

int update_fetch(const struct update_opts *o, const char *version)
{
    int rc = fetch(o, version);

    if (rc != 0 && stage_made) {
        /* Leave nothing half-done on the SD card. */
        stage_clean();
        rmdir(o->stage);
    }
    return rc;
}
