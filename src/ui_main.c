/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * moonlight-tsp-ui: the menu front end. It draws only when something
 * changes, sleeps in poll() otherwise, and runs moonlight-tsp-stream for
 * every host operation. While a stream is on screen it does not read the
 * pad at all; it only watches the engine's heartbeat.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ui_config.h"
#include "ui_draw.h"
#include "ui_engine.h"
#include "ui_mdns.h"
#include "ui_pad.h"

#define UI_VERSION "0.3.0"
#define DEFAULT_DATA "/mnt/UDISK/trimui-tools/moonlight-tsp"
/* launch.sh sets MLTSP_STAGE to this folder on the app's SD card. */
#define DEFAULT_STAGE "/mnt/SDCARD/.moonlight-tsp-update"

/* Timeouts (ms). */
#define T_INFO 6000
#define T_LIST 15000
#define T_PAIR 300000      /* time to walk to the PC and log in to Sunshine */
#define T_QUIT 15000
#define T_CONNECT 60000
#define T_HEARTBEAT 6000
#define T_STOP 10000
#define T_TERM_GRACE 4000
#define T_UPDATE_CHECK 20000
#define T_UPDATE_STALL 45000    /* download: no progress for this long */
#define T_UPDATE_VERIFY 90000   /* unpack the download and test it */

/* The UI's exit status that tells launch.sh to install a staged update. */
#define EXIT_INSTALL_UPDATE 3

/* Colours. */
#define C_BG 0x0e1116ff
#define C_HEAD 0x161b22ff
#define C_ROW 0x1a212bff
#define C_SEL 0x2b62c9ff
#define C_TEXT 0xe8edf2ff
#define C_DIM 0x8b96a3ff
#define C_OK 0x52c98eff
#define C_WARN 0xf0b44cff
#define C_ERR 0xff6b6bff
#define C_ACCENT 0x6ea8ffff
#define C_KEY 0xd5dbe2ff
#define C_KEYTXT 0x161b22ff

/* List geometry. */
#define LIST_X 40
#define LIST_Y 112
#define LIST_W 1200
#define ROW_H 64
#define ROW_PITCH 72
#define LIST_ROWS 7

/* Settings rows are denser so the help line fits above the footer. */
static int row_h = ROW_H, row_pitch = ROW_PITCH;

enum screen { S_HOSTS, S_SEARCH, S_ADDIP, S_APPS, S_PAIR, S_SETTINGS,
              S_BUSY, S_CONNECTING, S_STREAM, S_MENU };

/* J_PROBE: "info" for a search result (J.host is its index in U.md).
 * The update jobs have no host (J.host is -1). */
enum job { J_NONE, J_INFO, J_LIST, J_PAIR, J_QUIT, J_STREAM, J_PROBE,
           J_UPDATE_CHECK, J_UPDATE_FETCH };

enum upd_result { UR_NONE, UR_AVAILABLE, UR_LATEST, UR_ERROR };

enum hstate { H_UNKNOWN, H_CHECKING, H_ONLINE, H_OFFLINE };

enum err { E_NONE, E_UNREACHABLE, E_NOT_PAIRED, E_PAIR_FAILED, E_OTHER_APP,
           E_LAUNCH, E_CONNECT, E_STAGE, E_QUIT, E_OTHER };

enum kill_reason { K_NONE, K_TIMEOUT, K_WATCHDOG, K_CANCEL };

struct hstatus {
    enum hstate st;
    int paired;
    int running;
};

struct app {
    int id;
    int running;
    char name[64];
};

#define MAX_APPS 64

struct listview {
    int sel, top;
};

struct dialog {
    int active;
    char title[64];
    char text[320];         /* '\n' starts a new line */
    int nbtn;
    char btn[3][32];
    int sel;
    void (*done)(int choice);   /* -1: dismissed with B */
};

static struct {
    /* paths */
    char data[PATH_MAX], cfg_path[PATH_MAX], keydir[PATH_MAX];
    char logdir[PATH_MAX], stream_log[PATH_MAX], engine_log[PATH_MAX];
    char engine_path[PATH_MAX];
    char update_log[PATH_MAX], update_result[PATH_MAX], stage[PATH_MAX];
    const char *update_url;     /* MLTSP_UPDATE_URL, for tests */
    const char *started_file;   /* MLTSP_STARTED_FILE, from update.sh */
    int exit_code;
    int perf_scene;             /* CrossMix CPU Performance scene is on */
    int fifo_fd;
    char fifo_buf[256];
    size_t fifo_len;
    char shot_path[PATH_MAX];

    struct ui_pad pad;
    struct engine eng;
    struct mdns md;
    struct ui_config cfg;
    int sfd;

    enum screen scr;
    struct dialog dlg;
    int dirty;
    int quit;
    uint64_t tick_at;       /* redraw for elapsed-time displays */
    uint64_t pad_retry_at;  /* look for a missing pad again */

    /* hosts */
    struct hstatus hs[CFG_MAX_HOSTS];
    struct listview lv_hosts, lv_search, lv_apps, lv_settings, lv_menu;
    int host;               /* selected host */

    /* apps of the selected host */
    struct app apps[MAX_APPS];
    int napps;
    int apps_loaded;

    /* search */
    uint64_t search_next, search_end;
    int search_round;
    struct hstatus ms[MDNS_MAX_RESULTS];    /* status of search results */

    /* add-by-IP keypad */
    char ip[16];
    int key_sel;

    /* busy screen */
    char busy_title[64], busy_text[160];
    int busy_cancel;
    uint64_t busy_since;

    /* stream */
    int pending_app;        /* stream after a quit job, or -1 */
    char pending_name[64];
    char live[160];
    double l_fps, l_lat, l_dec, l_kbps, l_cpu;
    unsigned l_rtt;
    unsigned long l_lost, l_drop;
    int l_valid;
    char pin[8];
    unsigned drain_delay_us;    /* test only: MLTSP_TEST_DRAIN_DELAY_US */

    /* updates */
    int upd_checked;        /* the start-up check ran (or is running) */
    int upd_manual;         /* the running check is from Settings */
    int upd_offer;          /* show the update dialog when possible */
    char upd_ver[24], upd_summary[72];
    unsigned long upd_size;
    enum screen upd_from;   /* where a download returns to */
} U;

/* The engine job in progress. */
static struct {
    enum job kind;
    int host;
    uint64_t start, deadline, kill_at, last_hb;
    uint64_t last_line, ipc_end_at;     /* for the watchdog's report */
    int ipc_end;                        /* engine_fill's eof value */
    enum kill_reason killed;
    int user_list;          /* list requested by the user (show busy) */
    /* results */
    int got_host, h_paired, h_running;
    int paired, quit_ok, started, ended, menu;
    enum err err;
    int err_code;
    char err_text[96];
    int got_term, term_code;
    char term_text[160];
    char stage[96];
    int app_id;
    char app_name[64];
    struct app apps[MAX_APPS];
    int napps;
    int mouse;              /* mouse mode toggled on in this stream */
    /* update jobs */
    enum upd_result upd;
    char upd_ver[24], upd_summary[72];
    unsigned long upd_size, upd_got, upd_total;
    int upd_verify, upd_staged;
    char upd_err[320];
} J;

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/*
 * Milliseconds from T to NOW, or 0 if T is later. The main loop reads the
 * clock once after poll(), then handlers stamp events with a fresh
 * now_ms(), so a stamp can be a millisecond ahead of NOW; a plain unsigned
 * NOW - T then wraps to 2^64 - 1 ms.
 */
static uint64_t ms_since(uint64_t now, uint64_t t)
{
    return now > t ? now - t : 0;
}

static void redraw(void)
{
    U.dirty = 1;
}

static void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_msg(const char *fmt, ...)
{
    va_list ap;
    time_t t = time(NULL);
    struct tm tm;
    char ts[32];

    localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    fprintf(stderr, "%s ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    char *p;

    snprintf(buf, sizeof(buf), "%s", path);
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

static void save_config(void)
{
    if (config_save(&U.cfg, U.cfg_path) != 0)
        log_msg("config: save failed: %s", strerror(errno));
}

/* ---- dialogs ---------------------------------------------------------- */

static void dialog(const char *title, const char *text, void (*done)(int),
                   const char *b0, const char *b1)
{
    struct dialog *d = &U.dlg;

    memset(d, 0, sizeof(*d));
    d->active = 1;
    snprintf(d->title, sizeof(d->title), "%s", title);
    snprintf(d->text, sizeof(d->text), "%s", text);
    snprintf(d->btn[0], sizeof(d->btn[0]), "%s", b0 ? b0 : "OK");
    d->nbtn = 1;
    if (b1 != NULL) {
        snprintf(d->btn[1], sizeof(d->btn[1]), "%s", b1);
        d->nbtn = 2;
    }
    d->done = done;
    redraw();
}

static void dialog3(const char *title, const char *text, void (*done)(int),
                    const char *b0, const char *b1, const char *b2)
{
    dialog(title, text, done, b0, b1);
    snprintf(U.dlg.btn[2], sizeof(U.dlg.btn[2]), "%s", b2);
    U.dlg.nbtn = 3;
}

static void message(const char *title, const char *text)
{
    dialog(title, text, NULL, "OK", NULL);
}

static void dialog_key(enum ui_key k)
{
    struct dialog *d = &U.dlg;
    void (*done)(int) = d->done;
    int choice;

    switch (k) {
    case UK_LEFT:
    case UK_UP:
        if (d->sel > 0)
            d->sel--;
        redraw();
        return;
    case UK_RIGHT:
    case UK_DOWN:
        if (d->sel + 1 < d->nbtn)
            d->sel++;
        redraw();
        return;
    case UK_A:
        choice = d->sel;
        break;
    case UK_B:
    case UK_MENU:
        choice = -1;
        break;
    default:
        return;
    }
    d->active = 0;
    redraw();
    if (done != NULL)
        done(choice);
}

/* ---- engine jobs ------------------------------------------------------ */

static void start_next_check(void);
static void start_next_probe(void);
static void go_hosts(void);
static void go_apps(int refresh);
static void list_clamp(struct listview *l, int n);

static const char *codec_arg(void)
{
    return U.cfg.codec == CODEC_HEVC ? "h265" :
           U.cfg.codec == CODEC_H264 ? "h264" : "auto";
}

static int job_start(enum job kind, int host, const char *cmd,
                     const char *arg, uint64_t timeout)
{
    static char fps[16], bitrate[16];
    const char *args[32];
    int upd = kind == J_UPDATE_CHECK || kind == J_UPDATE_FETCH;
    const char *log = kind == J_STREAM ? U.stream_log :
                      upd ? U.update_log : U.engine_log;
    const char *addr = "";
    int n = 0;

    if (U.eng.pid > 0 || (host < 0 && !upd))
        return -1;
    if (upd) {
        /* No host: these talk to GitHub. */
        host = -1;
        args[n++] = "--stage";
        args[n++] = U.stage;
        if (U.update_url != NULL) {
            args[n++] = "--update-url";
            args[n++] = U.update_url;
        }
    } else if (kind == J_PROBE) {
        if (host >= U.md.n)
            return -1;
        addr = U.md.r[host].addr;
    } else {
        if (host >= U.cfg.nhosts)
            return -1;
        addr = U.cfg.hosts[host].addr;
    }
    if (!upd) {
        args[n++] = "--host";
        args[n++] = addr;
        args[n++] = "--keydir";
        args[n++] = U.keydir;
    }
    if (kind == J_STREAM) {
        snprintf(fps, sizeof(fps), "%d", U.cfg.fps);
        snprintf(bitrate, sizeof(bitrate), "%d", U.cfg.bitrate);
        args[n++] = "--fps";
        args[n++] = fps;
        args[n++] = "--bitrate";
        args[n++] = bitrate;
        args[n++] = "--codec";
        args[n++] = codec_arg();
        if (U.cfg.swap_abxy)
            args[n++] = "--swap-abxy";
        if (U.cfg.quit_after)
            args[n++] = "--quit-after";
        if (U.cfg.sops)
            args[n++] = "--sops";
        if (!U.cfg.wifi_ps_off)
            args[n++] = "--keep-wifi-ps";
        /* Keep the previous session's log for comparison. */
        {
            char prev[PATH_MAX + 8];

            snprintf(prev, sizeof(prev), "%s.1", U.stream_log);
            rename(U.stream_log, prev);
        }
    }
    args[n++] = cmd;
    if (arg != NULL)
        args[n++] = arg;
    args[n] = NULL;

    /* The update log keeps a check and the download that follows it. */
    if (engine_spawn(&U.eng, U.engine_path, args, log, upd) != 0) {
        log_msg("engine: spawn %s failed: %s", U.engine_path, strerror(errno));
        return -1;
    }
    memset(&J, 0, sizeof(J));
    J.kind = kind;
    J.host = host;
    J.start = now_ms();
    J.deadline = J.start + timeout;
    log_msg("job %s%s%s%s%s (pid %d)", cmd, addr[0] ? " " : "", addr,
            arg ? " " : "", arg ? arg : "", (int)U.eng.pid);
    return 0;
}

/* Cancels a non-stream job without reporting its result. */
static void job_cancel(void)
{
    if (J.kind == J_NONE || J.kind == J_STREAM)
        return;
    if (J.kind == J_INFO && U.hs[J.host].st == H_CHECKING)
        U.hs[J.host].st = H_UNKNOWN;
    if (J.kind == J_PROBE && U.ms[J.host].st == H_CHECKING)
        U.ms[J.host].st = H_UNKNOWN;
    if (J.kind == J_UPDATE_CHECK && !U.upd_manual)
        U.upd_checked = 0;      /* another job came first: try again later */
    engine_kill(&U.eng);
    J.kind = J_NONE;
}

static void parse_stats(const char *s)
{
    if (sscanf(s, "fps=%lf lat=%lf dec=%lf rtt=%u kbps=%lf lost=%lu drop=%lu "
                  "cpu=%lf", &U.l_fps, &U.l_lat, &U.l_dec, &U.l_rtt,
               &U.l_kbps, &U.l_lost, &U.l_drop, &U.l_cpu) == 8)
        U.l_valid = 1;
}

static void enter_stream_screen(void)
{
    U.scr = S_STREAM;
    ui_pad_stop_repeat(&U.pad);
    redraw();
}

static void busy(const char *title, const char *text, int cancel);

static void fetch_progress_text(void)
{
    snprintf(U.busy_text, sizeof(U.busy_text), "%.1f of %.1f MB",
             J.upd_got / 1048576.0, J.upd_total / 1048576.0);
}

/* "update ..." lines from update-check and update-fetch. */
static void on_update_line(char *arg)
{
    char *rest = strchr(arg, ' ');

    if (rest != NULL)
        *rest++ = '\0';
    else
        rest = arg + strlen(arg);
    if (strcmp(arg, "available") == 0) {
        int off = 0;

        /* available VERSION SIZE SUMMARY */
        if (sscanf(rest, "%23s %lu %n", J.upd_ver, &J.upd_size, &off) >= 2) {
            J.upd = UR_AVAILABLE;
            snprintf(J.upd_summary, sizeof(J.upd_summary), "%s",
                     off > 0 ? rest + off : "");
        }
    } else if (strcmp(arg, "none") == 0) {
        J.upd = UR_LATEST;
        snprintf(J.upd_ver, sizeof(J.upd_ver), "%s", rest);
    } else if (strcmp(arg, "progress") == 0) {
        if (sscanf(rest, "%lu %lu", &J.upd_got, &J.upd_total) == 2 &&
            J.kind == J_UPDATE_FETCH && J.killed == K_NONE) {
            J.deadline = now_ms() + T_UPDATE_STALL;
            if (U.scr == S_BUSY && !J.upd_verify) {
                fetch_progress_text();
                redraw();
            }
        }
    } else if (strcmp(arg, "verify") == 0) {
        J.upd_verify = 1;
        if (J.kind == J_UPDATE_FETCH && J.killed == K_NONE)
            J.deadline = now_ms() + T_UPDATE_VERIFY;
        if (U.scr == S_BUSY) {
            snprintf(U.busy_text, sizeof(U.busy_text), "Testing version %s...",
                     U.upd_ver);
            redraw();
        }
    } else if (strcmp(arg, "staged") == 0) {
        J.upd_staged = 1;
    } else if (strcmp(arg, "error") == 0) {
        J.upd = UR_ERROR;
        snprintf(J.upd_err, sizeof(J.upd_err), "%s", rest);
    }
}

static void on_engine_line(char *line)
{
    char *arg = strchr(line, ' ');

    J.last_line = now_ms();
    if (arg != NULL)
        *arg++ = '\0';
    else
        arg = line + strlen(line);

    if (strcmp(line, "hb") == 0) {
        J.last_hb = now_ms();
    } else if (strcmp(line, "stats") == 0) {
        J.last_hb = now_ms();
        parse_stats(arg);
        if (U.scr == S_MENU)
            redraw();
    } else if (strcmp(line, "host") == 0) {
        int p = 0, r = 0, h = 0;

        if (sscanf(arg, "%d %d %d", &p, &r, &h) >= 2) {
            J.got_host = 1;
            J.h_paired = p;
            J.h_running = r;
        }
    } else if (strcmp(line, "app") == 0) {
        /* app ID RUNNING NAME */
        struct app a;
        int off = 0;

        memset(&a, 0, sizeof(a));
        if (sscanf(arg, "%d %d %n", &a.id, &a.running, &off) >= 2 &&
            off > 0 && J.napps < MAX_APPS) {
            snprintf(a.name, sizeof(a.name), "%s", arg + off);
            J.apps[J.napps++] = a;
        }
    } else if (strcmp(line, "pin") == 0) {
        snprintf(U.pin, sizeof(U.pin), "%s", arg);
        U.scr = S_PAIR;
        redraw();
    } else if (strcmp(line, "paired") == 0) {
        J.paired = 1;
    } else if (strcmp(line, "quit-done") == 0) {
        J.quit_ok = 1;
    } else if (strcmp(line, "stage") == 0) {
        snprintf(J.stage, sizeof(J.stage), "%s", arg);
        if (U.scr == S_CONNECTING)
            redraw();
    } else if (strcmp(line, "started") == 0) {
        J.started = 1;
        J.last_hb = now_ms();
        if (U.scr == S_CONNECTING)
            enter_stream_screen();
    } else if (strcmp(line, "menu") == 0) {
        if (U.scr == S_STREAM) {
            U.scr = S_MENU;
            U.lv_menu.sel = 0;
            ui_pad_drain(&U.pad);
            redraw();
        }
    } else if (strcmp(line, "term") == 0) {
        int off = 0;

        J.got_term = 1;
        if (sscanf(arg, "%d %n", &J.term_code, &off) >= 1 && off > 0) {
            snprintf(J.term_text, sizeof(J.term_text), "%s", arg + off);
            if (J.term_text[0] >= 'a' && J.term_text[0] <= 'z')
                J.term_text[0] = (char)(J.term_text[0] - 'a' + 'A');
        }
    } else if (strcmp(line, "ended") == 0) {
        J.ended = 1;
    } else if (strcmp(line, "update") == 0) {
        on_update_line(arg);
    } else if (strcmp(line, "error") == 0) {
        char kind[32] = "";
        int off = 0;

        sscanf(arg, "%31s %n", kind, &off);
        if (off > 0)
            snprintf(J.err_text, sizeof(J.err_text), "%s", arg + off);
        J.err_code = off > 0 ? atoi(arg + off) : 0;
        if (strcmp(kind, "stage-failed") == 0) {
            /* error stage-failed STAGE NAME CODE */
            char *sp = strrchr(J.err_text, ' ');

            if (sp != NULL) {
                J.err_code = atoi(sp + 1);
                *sp = '\0';
            }
            snprintf(J.stage, sizeof(J.stage), "%s", J.err_text);
            J.err = E_STAGE;
            return;
        }
        J.err = strcmp(kind, "unreachable") == 0 ? E_UNREACHABLE :
                strcmp(kind, "not-paired") == 0 ? E_NOT_PAIRED :
                strcmp(kind, "pair-failed") == 0 ? E_PAIR_FAILED :
                strcmp(kind, "other-app-running") == 0 ? E_OTHER_APP :
                strcmp(kind, "launch-failed") == 0 ? E_LAUNCH :
                strcmp(kind, "connect-failed") == 0 ?
                    (J.err == E_STAGE ? E_STAGE : E_CONNECT) :
                strcmp(kind, "quit-failed") == 0 ? E_QUIT : E_OTHER;
    }
}

static void drain_engine(void)
{
    char line[600];

    while (engine_fill(&U.eng) == 0 && U.eng.fd >= 0) {
        int got = 0;

        while (engine_read_line(&U.eng, line, sizeof(line))) {
            on_engine_line(line);
            got = 1;
        }
        if (!got)
            break;
    }
    while (engine_read_line(&U.eng, line, sizeof(line)))
        on_engine_line(line);
    if (U.eng.eof != 0 && U.eng.fd >= 0) {
        /* Everything was read; stop polling a socket that stays readable
         * (at EOF) until the engine is reaped. */
        J.ipc_end = U.eng.eof;
        J.ipc_end_at = now_ms();
        engine_close(&U.eng);
    }
}

/* What became of the engine's socket, for the watchdog's log line. */
static const char *ipc_state(uint64_t now, char *buf, size_t max)
{
    if (J.ipc_end > 0)
        snprintf(buf, max, "closed by the engine %llu ms ago",
                 (unsigned long long)ms_since(now, J.ipc_end_at));
    else if (J.ipc_end < 0)
        snprintf(buf, max, "read error %s %llu ms ago", strerror(-J.ipc_end),
                 (unsigned long long)ms_since(now, J.ipc_end_at));
    else if (U.eng.fd < 0)
        snprintf(buf, max, "not open");
    else if (fcntl(U.eng.fd, F_GETFD) < 0)
        snprintf(buf, max, "fd %d invalid", U.eng.fd);
    else
        snprintf(buf, max, "open, fd %d", U.eng.fd);
    return buf;
}

static const char *host_name(int i)
{
    return i >= 0 && i < U.cfg.nhosts ? U.cfg.hosts[i].name : "?";
}

static void busy(const char *title, const char *text, int cancel)
{
    snprintf(U.busy_title, sizeof(U.busy_title), "%s", title);
    snprintf(U.busy_text, sizeof(U.busy_text), "%s", text);
    U.busy_cancel = cancel;
    U.busy_since = now_ms();
    U.scr = S_BUSY;
    redraw();
}

static void start_pair(int host)
{
    U.pin[0] = '\0';
    if (job_start(J_PAIR, host, "pair", NULL, T_PAIR) != 0) {
        message("Cannot pair", "The stream engine did not start.");
        return;
    }
    busy("Pairing", "Contacting the PC...", 1);
}

static void start_list(int host, int user)
{
    if (job_start(J_LIST, host, "list", NULL, T_LIST) != 0) {
        message("Cannot load apps", "The stream engine did not start.");
        return;
    }
    J.user_list = user;
    if (user)
        busy("Loading apps", host_name(host), 1);
}

static void start_stream(int app_id, const char *name)
{
    char id[16];

    job_cancel();
    snprintf(id, sizeof(id), "%d", app_id);
    if (job_start(J_STREAM, U.host, "stream", id, T_CONNECT) != 0) {
        message("Cannot stream", "The stream engine did not start.");
        return;
    }
    J.app_id = app_id;
    snprintf(J.app_name, sizeof(J.app_name), "%s", name);
    U.l_valid = 0;
    U.scr = S_CONNECTING;
    redraw();
}

static void start_quit(void)
{
    job_cancel();
    if (job_start(J_QUIT, U.host, "quit", NULL, T_QUIT) != 0) {
        message("Cannot quit the app", "The stream engine did not start.");
        return;
    }
    busy("Quitting app", host_name(U.host), 0);
}

static const char *err_message(char *buf, size_t max)
{
    switch (J.err) {
    case E_UNREACHABLE:
        snprintf(buf, max, "%s (%s) did not answer. Make sure that the PC "
                 "and Sunshine are on.", host_name(J.host),
                 U.cfg.hosts[J.host].addr);
        break;
    case E_PAIR_FAILED:
        snprintf(buf, max, "Pairing did not complete. Open the PC again to "
                 "get a new PIN.");
        break;
    case E_OTHER_APP:
        snprintf(buf, max, "Another app is open on the PC. Quit that app "
                 "first.");
        break;
    case E_LAUNCH:
        snprintf(buf, max, "The PC did not start the app (error %d).",
                 J.err_code);
        break;
    case E_STAGE:
        if (J.stage[0])
            snprintf(buf, max, "The connection did not complete the %s "
                     "stage (error %d).", J.stage, J.err_code);
        else
            snprintf(buf, max, "The connection to the PC did not start "
                     "(error %d).", J.err_code);
        break;
    case E_CONNECT:
        snprintf(buf, max, "The connection to the PC did not start "
                 "(error %d).",
                 J.err_code);
        break;
    case E_QUIT:
        snprintf(buf, max, "The PC did not quit the app.");
        break;
    default:
        snprintf(buf, max, "An unknown error occurred. The logs are in %s.",
                 U.logdir);
        break;
    }
    return buf;
}

static void after_stream_dialog(int choice)
{
    (void)choice;
}

/* ---- updates ---------------------------------------------------------- */

static void start_update_fetch(void);

/* Where an update download or a check from Settings returns to. */
static void update_return(void)
{
    if (U.upd_from == S_HOSTS) {
        go_hosts();
    } else if (U.upd_from == S_APPS) {
        go_apps(0);
    } else {
        U.scr = U.upd_from;
        redraw();
    }
}

static void update_offer_done(int choice)
{
    if (choice == 0) {
        log_msg("update: the user chose to update to %s", U.upd_ver);
        start_update_fetch();
    } else if (choice == 2) {
        log_msg("update: the user skips %s", U.upd_ver);
        snprintf(U.cfg.update_skip, sizeof(U.cfg.update_skip), "%s",
                 U.upd_ver);
        save_config();
    } else {
        log_msg("update: later");
    }
}

/* Shows a found update as soon as no other dialog or job is in the way. */
static void show_update_offer(void)
{
    char text[320], size[32];

    if (!U.upd_offer || U.dlg.active ||
        (U.scr != S_HOSTS && U.scr != S_APPS && U.scr != S_SETTINGS))
        return;
    U.upd_offer = 0;
    snprintf(size, sizeof(size), "%.1f MB", U.upd_size / 1048576.0);
    snprintf(text, sizeof(text), "Version %s is available. You have "
             UI_VERSION ".\n%s%s%sThe download is %s. Then the app "
             "starts again.", U.upd_ver, U.upd_summary[0] ? "New: " : "",
             U.upd_summary, U.upd_summary[0] ? "\n" : "", size);
    dialog3("Update available", text, update_offer_done, "Update", "Later",
            "Skip");
    U.dlg.sel = 1;              /* a stray A press changes nothing */
}

/* The start-up check: once per run, when nothing else uses the engine. */
static void start_update_check(void)
{
    if (U.upd_checked || !U.cfg.update_check || J.kind != J_NONE ||
        U.dlg.active || (U.scr != S_HOSTS && U.scr != S_APPS))
        return;
    U.upd_checked = 1;
    U.upd_manual = 0;
    if (job_start(J_UPDATE_CHECK, -1, "update-check", NULL,
                  T_UPDATE_CHECK) != 0)
        log_msg("update: the check did not start");
}

static void start_manual_update_check(void)
{
    if (J.kind == J_STREAM || J.kind == J_UPDATE_FETCH)
        return;
    U.upd_manual = 1;           /* before job_cancel: keeps upd_checked */
    job_cancel();
    U.upd_checked = 1;
    U.upd_from = U.scr;
    if (job_start(J_UPDATE_CHECK, -1, "update-check", NULL,
                  T_UPDATE_CHECK) != 0) {
        U.upd_manual = 0;
        message("Cannot check for updates", "The stream engine did not "
                "start.");
        return;
    }
    busy("Checking for updates", "Asking GitHub for the newest version...",
         1);
}

static void update_check_done(void)
{
    int manual = U.upd_manual;
    char buf[400];

    U.upd_manual = 0;
    switch (J.upd) {
    case UR_AVAILABLE:
        log_msg("update: version %s is available (%lu bytes)", J.upd_ver,
                J.upd_size);
        break;
    case UR_LATEST:
        log_msg("update: none (newest release %s)", J.upd_ver);
        break;
    default:
        log_msg("update: check failed: %s", J.killed == K_TIMEOUT ?
                "timeout" : J.upd_err[0] ? J.upd_err : "no result");
        break;
    }
    if (manual)
        update_return();
    if (J.upd == UR_AVAILABLE) {
        if (!manual && strcmp(J.upd_ver, U.cfg.update_skip) == 0) {
            log_msg("update: %s was skipped before", J.upd_ver);
            return;
        }
        snprintf(U.upd_ver, sizeof(U.upd_ver), "%s", J.upd_ver);
        snprintf(U.upd_summary, sizeof(U.upd_summary), "%s", J.upd_summary);
        U.upd_size = J.upd_size;
        U.upd_offer = 1;
        show_update_offer();
    } else if (manual && J.upd == UR_LATEST) {
        message("No update", "You have the newest version (" UI_VERSION
                ").");
    } else if (manual) {
        if (J.killed == K_TIMEOUT)
            snprintf(buf, sizeof(buf), "GitHub did not answer in %d "
                     "seconds. Make sure that Wi-Fi is on.",
                     T_UPDATE_CHECK / 1000);
        else if (J.upd_err[0])
            snprintf(buf, sizeof(buf), "%s", J.upd_err);
        else
            snprintf(buf, sizeof(buf), "The check did not complete. The "
                     "logs are in %s.", U.logdir);
        message("Cannot check for updates", buf);
    }
}

static void start_update_fetch(void)
{
    if (J.kind == J_STREAM)
        return;
    job_cancel();
    U.upd_from = U.scr;
    if (job_start(J_UPDATE_FETCH, -1, "update-fetch", U.upd_ver,
                  T_UPDATE_STALL) != 0) {
        message("Cannot update", "The stream engine did not start.");
        return;
    }
    J.upd_total = U.upd_size;
    busy("Downloading update", "", 1);
    fetch_progress_text();
}

/* Removes what a stopped download left. The engine cleans the rest. */
static void update_remove_partial(void)
{
    char path[PATH_MAX + 32];

    snprintf(path, sizeof(path), "%s/download.zip.part", U.stage);
    unlink(path);
    snprintf(path, sizeof(path), "%s/download.zip", U.stage);
    unlink(path);
    rmdir(U.stage);
}

static void render(void);

static void update_fetch_done(int ok)
{
    char buf[400];

    if (ok && J.upd_staged) {
        log_msg("update: version %s is ready; exiting with status %d to "
                "install it", U.upd_ver, EXIT_INSTALL_UPDATE);
        snprintf(buf, sizeof(buf), "Moonlight HW %s starts in a moment.",
                 U.upd_ver);
        busy("Installing update", buf, 0);
        render();
        U.exit_code = EXIT_INSTALL_UPDATE;
        U.quit = 1;
        return;
    }
    log_msg("update: download failed: %s", J.killed == K_TIMEOUT ?
            "timeout" : J.upd_err[0] ? J.upd_err : "no result");
    if (J.killed == K_TIMEOUT)
        update_remove_partial();
    update_return();
    if (J.killed == K_TIMEOUT)
        snprintf(buf, sizeof(buf), "The download stopped. Make sure that "
                 "Wi-Fi is on, then try again.");
    else if (J.upd_err[0])
        snprintf(buf, sizeof(buf), "%s", J.upd_err);
    else
        snprintf(buf, sizeof(buf), "The download did not complete. The logs "
                 "are in %s.", U.logdir);
    message("Cannot update", buf);
}

/* What update.sh reported about the last install, shown once. */
static void read_update_result(void)
{
    char line[128], a[24] = "", b[24] = "", kind[16] = "", text[200];
    FILE *f = fopen(U.update_result, "re");

    if (f == NULL)
        return;
    if (fgets(line, sizeof(line), f) == NULL)
        line[0] = '\0';
    fclose(f);
    unlink(U.update_result);
    if (sscanf(line, "%15s %23s %23s", kind, a, b) != 3)
        return;
    log_msg("update: result %s %s %s", kind, a, b);
    if (strcmp(kind, "updated") == 0) {
        /* updated FROM TO */
        snprintf(text, sizeof(text), "Moonlight HW is updated from version "
                 "%s to version %s.", a, b);
        message("Update installed", text);
    } else if (strcmp(kind, "rolled-back") == 0) {
        /* rolled-back FAILED RESTORED */
        snprintf(U.cfg.update_skip, sizeof(U.cfg.update_skip), "%s", a);
        save_config();
        snprintf(text, sizeof(text), "Version %s did not start on this "
                 "device. Version %s is back. The app does not offer "
                 "version %s again.", a, b, a);
        message("Update failed", text);
    }
}

/* The engine has exited and was reaped. */
static void job_finished(void)
{
    enum job kind = J.kind;
    int st = U.eng.status, h = J.host;
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && J.killed == K_NONE;
    char buf[320];

    drain_engine();
    engine_close(&U.eng);
    J.kind = J_NONE;
    log_msg("job done: kind %d status 0x%x killed %d err %d", (int)kind, st,
            (int)J.killed, (int)J.err);

    switch (kind) {
    case J_INFO:
    case J_PROBE: {
        struct hstatus *hs = kind == J_PROBE ? &U.ms[h] : &U.hs[h];

        if (J.got_host) {
            hs->st = H_ONLINE;
            hs->paired = J.h_paired;
            hs->running = J.h_running;
        } else {
            hs->st = H_OFFLINE;
        }
        redraw();
        break;
    }

    case J_LIST:
        if (ok) {
            int i;

            memcpy(U.apps, J.apps, sizeof(J.apps));
            U.napps = J.napps;
            U.apps_loaded = 1;
            U.hs[h].st = H_ONLINE;
            U.hs[h].paired = 1;
            U.hs[h].running = 0;
            for (i = 0; i < U.napps; i++)
                if (U.apps[i].running)
                    U.hs[h].running = U.apps[i].id;
            if (U.scr == S_BUSY && J.user_list) {
                /* Opened from the host list: start on the running app. */
                U.lv_apps.sel = U.lv_apps.top = 0;
                for (i = 0; i < U.napps; i++)
                    if (U.apps[i].running)
                        U.lv_apps.sel = i;
                list_clamp(&U.lv_apps, U.napps);
                U.scr = S_APPS;
            }
        } else if (!J.user_list) {
            /* A background refresh: keep the list shown. */
            if (J.err == E_NOT_PAIRED)
                U.hs[h].paired = 0;
        } else if (J.err == E_NOT_PAIRED) {
            U.hs[h].st = H_ONLINE;
            U.hs[h].paired = 0;
            if (U.scr == S_BUSY)
                start_pair(h);
        } else if (U.scr == S_BUSY) {
            if (J.err == E_UNREACHABLE || J.killed == K_TIMEOUT)
                U.hs[h].st = H_OFFLINE;
            go_hosts();
            if (J.killed == K_TIMEOUT)
                snprintf(buf, sizeof(buf), "%s did not answer in time.",
                         host_name(h));
            else
                err_message(buf, sizeof(buf));
            message("Cannot load apps", buf);
        }
        redraw();
        break;

    case J_PAIR:
        if (J.paired || ok) {
            U.hs[h].st = H_ONLINE;
            U.hs[h].paired = 1;
            start_list(h, 1);
        } else if (J.killed == K_CANCEL) {
            go_hosts();
        } else {
            go_hosts();
            if (J.killed == K_TIMEOUT)
                snprintf(buf, sizeof(buf), "Sunshine on %s (%s) did not "
                         "get the PIN in %d minutes. Open the PC again to "
                         "get a new PIN.", host_name(h), U.cfg.hosts[h].addr,
                         T_PAIR / 60000);
            else
                err_message(buf, sizeof(buf));
            message("Pairing failed", buf);
        }
        break;

    case J_QUIT:
        if (J.quit_ok && U.pending_app >= 0) {
            int id = U.pending_app;

            U.pending_app = -1;
            start_stream(id, U.pending_name);
        } else {
            U.pending_app = -1;
            if (!J.quit_ok) {
                err_message(buf, sizeof(buf));
                if (J.killed == K_TIMEOUT)
                    snprintf(buf, sizeof(buf), "The PC did not answer.");
                message("Cannot quit the app", buf);
            }
            go_apps(1);
        }
        break;

    case J_STREAM: {
        int rc;

        /* Always: removes any layer and restores Wi-Fi power save. */
        rc = engine_run_reset(U.engine_path, U.stream_log, 5000);
        log_msg("stream ended: started %d ended %d term %d/%d killed %d, "
                "reset %d", J.started, J.ended, J.got_term, J.term_code,
                (int)J.killed, rc);
        ui_pad_drain(&U.pad);
        U.scr = S_APPS;
        if (J.killed == K_WATCHDOG) {
            snprintf(buf, sizeof(buf), "The stream engine did not respond "
                     "for %d seconds. Moonlight HW stopped it and wrote "
                     "hang.log in %s.", T_HEARTBEAT / 1000, U.logdir);
            message("Stream closed", buf);
        } else if (J.killed == K_TIMEOUT && !J.started) {
            snprintf(buf, sizeof(buf), "The stream did not start in %d "
                     "seconds.", T_CONNECT / 1000);
            message("Connection failed", buf);
        } else if (J.killed == K_CANCEL) {
            /* the user cancelled */
        } else if (WIFSIGNALED(st)) {
            snprintf(buf, sizeof(buf), "The stream engine stopped with "
                     "signal %d. The logs are in %s.", WTERMSIG(st),
                     U.logdir);
            message("Stream closed", buf);
        } else if (J.got_term && J.term_code != 0) {
            snprintf(buf, sizeof(buf), "%s The error code is %d.",
                     J.term_text[0] ? J.term_text :
                     "This device lost the connection to the PC.",
                     J.term_code);
            dialog("Disconnected", buf, after_stream_dialog, "OK", NULL);
        } else if (!J.started && J.err != E_NONE) {
            message("Cannot stream", err_message(buf, sizeof(buf)));
        } else if (!J.started) {
            message("Cannot stream", "The stream ended before it started.");
        }
        go_apps(1);
        break;
    }
    case J_UPDATE_CHECK:
        update_check_done();
        break;
    case J_UPDATE_FETCH:
        update_fetch_done(ok);
        break;
    default:
        break;
    }
    start_next_check();
    start_next_probe();
    start_update_check();
}

/* Records what a hung engine was waiting for before it is killed. */
static void dump_engine(void)
{
    char path[sizeof(U.logdir) + 16];

    snprintf(path, sizeof(path), "%s/hang.log", U.logdir);
    if (engine_dump(U.eng.pid, path) == 0)
        log_msg("engine: thread dump in %s", path);
    else
        log_msg("engine: thread dump failed: %s", strerror(errno));
}

/* Timeouts and the stream heartbeat. */
static void job_timers(uint64_t now)
{
    if (J.kind == J_NONE || U.eng.pid <= 0)
        return;
    if (J.kill_at != 0 && now >= J.kill_at) {
        log_msg("engine: no exit after SIGTERM, killing");
        dump_engine();
        engine_signal(&U.eng, SIGKILL);
        J.kill_at = 0;
        return;
    }
    if (J.kind == J_STREAM && J.started && J.killed == K_NONE &&
        ms_since(now, J.last_hb) > T_HEARTBEAT) {
        char st[96];

        log_msg("engine: heartbeat lost (last line %llu ms ago, %lu bytes "
                "received, socket %s)",
                (unsigned long long)(J.last_line ? ms_since(now, J.last_line)
                                                 : 0),
                U.eng.rx, ipc_state(now, st, sizeof(st)));
        dump_engine();
        J.killed = K_WATCHDOG;
        engine_signal(&U.eng, SIGKILL);
        return;
    }
    if (J.kind == J_STREAM && J.started)
        return;     /* the heartbeat rules once streaming */
    if (J.deadline != 0 && now >= J.deadline && J.killed == K_NONE) {
        log_msg("engine: job timeout");
        J.killed = K_TIMEOUT;
        J.deadline = 0;
        if (J.kind == J_STREAM) {
            /* Let it interrupt the connection and clean up. */
            engine_signal(&U.eng, SIGTERM);
            J.kill_at = now + T_TERM_GRACE;
        } else {
            engine_signal(&U.eng, SIGKILL);
        }
    }
}

static uint64_t job_next_deadline(void)
{
    uint64_t d = 0;

    if (J.kind == J_NONE)
        return 0;
    if (J.kill_at != 0)
        d = J.kill_at;
    if (J.kind == J_STREAM && J.started && J.killed == K_NONE) {
        uint64_t hb = J.last_hb + T_HEARTBEAT + 1;

        if (d == 0 || hb < d)
            d = hb;
    } else if (J.deadline != 0 && (d == 0 || J.deadline < d)) {
        d = J.deadline;
    }
    return d;
}

/* ---- host checks ------------------------------------------------------ */

static void start_next_check(void)
{
    int i;

    if (J.kind != J_NONE || U.scr != S_HOSTS || U.dlg.active)
        return;
    for (i = 0; i < U.cfg.nhosts; i++) {
        if (U.hs[i].st != H_UNKNOWN)
            continue;
        if (job_start(J_INFO, i, "info", NULL, T_INFO) == 0) {
            U.hs[i].st = H_CHECKING;
            redraw();
        } else {
            U.hs[i].st = H_OFFLINE;
        }
        return;
    }
}

/* Checks each search result in turn, so the list shows which PCs are
 * already paired with this device. */
static void start_next_probe(void)
{
    int i;

    if (J.kind != J_NONE || U.scr != S_SEARCH || U.dlg.active)
        return;
    for (i = 0; i < U.md.n; i++) {
        if (U.ms[i].st != H_UNKNOWN)
            continue;
        if (job_start(J_PROBE, i, "info", NULL, T_INFO) == 0) {
            U.ms[i].st = H_CHECKING;
            redraw();
        } else {
            U.ms[i].st = H_OFFLINE;
        }
        return;
    }
}

static void recheck_hosts(void)
{
    int i;

    for (i = 0; i < U.cfg.nhosts; i++)
        if (U.hs[i].st != H_CHECKING)
            U.hs[i].st = H_UNKNOWN;
}

/* ---- navigation ------------------------------------------------------- */

static void list_move(struct listview *l, int n, int d)
{
    if (n <= 0) {
        l->sel = l->top = 0;
        return;
    }
    l->sel += d;
    if (l->sel < 0)
        l->sel = d < -1 ? 0 : n - 1;        /* wrap on single steps */
    if (l->sel >= n)
        l->sel = d > 1 ? n - 1 : 0;
    if (l->sel < l->top)
        l->top = l->sel;
    if (l->sel >= l->top + LIST_ROWS)
        l->top = l->sel - LIST_ROWS + 1;
    redraw();
}

static void list_clamp(struct listview *l, int n)
{
    if (l->sel >= n)
        l->sel = n > 0 ? n - 1 : 0;
    if (l->sel < 0)
        l->sel = 0;
    if (l->top > l->sel)
        l->top = l->sel;
    if (l->sel >= l->top + LIST_ROWS)
        l->top = l->sel - LIST_ROWS + 1;
}

/*
 * The CrossMix "CPU Performance" scene runs "com.trimui.cpuperformance.sh 1"
 * and, while any app runs, pins the CPU at 2 GHz with the performance
 * governor every 5 s. We only warn (the user's choice); system CPU settings
 * are never changed here.
 */
static int perf_scene_running(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int found = 0;

    if (d == NULL)
        return 0;
    while (!found && (e = readdir(d)) != NULL) {
        char path[64], buf[512];
        ssize_t n;
        int fd;

        if (e->d_name[0] < '1' || e->d_name[0] > '9')
            continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            continue;
        buf[n] = '\0';
        /* argv is NUL-separated: find the script, then check its argument. */
        for (char *a = buf; a < buf + n; a += strlen(a) + 1) {
            if (strstr(a, "com.trimui.cpuperformance.sh") != NULL) {
                char *arg = a + strlen(a) + 1;

                found = arg < buf + n && strcmp(arg, "1") == 0;
                break;
            }
        }
    }
    closedir(d);
    return found;
}

static void go_hosts(void)
{
    U.perf_scene = perf_scene_running();
    if (J.kind == J_LIST)
        job_cancel();
    U.scr = S_HOSTS;
    list_clamp(&U.lv_hosts, U.cfg.nhosts + 2);
    redraw();
    start_next_check();
}

static void go_apps(int refresh)
{
    U.scr = S_APPS;
    list_clamp(&U.lv_apps, U.napps);
    redraw();
    if (refresh && J.kind == J_NONE)
        start_list(U.host, 0);
}

static void open_host(int i)
{
    job_cancel();
    U.host = i;
    U.cfg.last_host = i;
    U.napps = 0;
    U.apps_loaded = 0;
    if (U.hs[i].st == H_ONLINE && !U.hs[i].paired)
        start_pair(i);
    else
        start_list(i, 1);
}

static void remove_host_done(int choice)
{
    int i = U.lv_hosts.sel;

    if (choice != 0 || i >= U.cfg.nhosts)
        return;
    job_cancel();
    config_remove_host(&U.cfg, i);
    memmove(&U.hs[i], &U.hs[i + 1],
            (size_t)(CFG_MAX_HOSTS - i - 1) * sizeof(U.hs[0]));
    memset(&U.hs[CFG_MAX_HOSTS - 1], 0, sizeof(U.hs[0]));
    save_config();
    go_hosts();
}

static void start_search(void)
{
    if (U.md.fd < 0 && mdns_open(&U.md) != 0) {
        U.md.fd = -1;
        message("Cannot search", "The network socket for the search did "
                "not open. Make sure that Wi-Fi is on.");
        return;
    }
    if (J.kind == J_PROBE)
        job_cancel();
    log_msg("search: %s", U.md.legacy ? "unicast replies only (port 5353 "
            "not shared)" : "multicast on port 5353");
    U.md.n = 0;
    memset(U.ms, 0, sizeof(U.ms));
    U.lv_search.sel = U.lv_search.top = 0;
    U.search_round = 0;
    U.search_next = now_ms();
    U.search_end = U.search_next + 4000;
    U.scr = S_SEARCH;
    redraw();
}

static void stop_search(void)
{
    if (J.kind == J_PROBE)
        job_cancel();
    mdns_close(&U.md);
    U.search_next = U.search_end = 0;
}

/* Prefills the keypad with this device's /24 prefix. */
static void start_addip(void)
{
    struct ifaddrs *ifa, *i;

    U.ip[0] = '\0';
    if (getifaddrs(&ifa) == 0) {
        for (i = ifa; i != NULL; i = i->ifa_next) {
            if (i->ifa_addr == NULL || i->ifa_addr->sa_family != AF_INET ||
                (i->ifa_flags & IFF_LOOPBACK))
                continue;
            {
                unsigned char *b = (unsigned char *)
                    &((struct sockaddr_in *)i->ifa_addr)->sin_addr;

                snprintf(U.ip, sizeof(U.ip), "%u.%u.%u.", b[0], b[1], b[2]);
            }
            break;
        }
        freeifaddrs(ifa);
    }
    U.key_sel = 0;
    U.scr = S_ADDIP;
    redraw();
}

/* KNOWN: a finished search probe of this address, used as the first check. */
static void add_host_and_check(const char *addr, const char *name,
                               const struct hstatus *known)
{
    int i = config_add_host(&U.cfg, addr, name);

    if (i < 0) {
        message("Cannot add PC", "The PC list is full. Remove a PC from the "
                "list first.");
        return;
    }
    memset(&U.hs[i], 0, sizeof(U.hs[i]));
    if (known != NULL && known->st == H_ONLINE)
        U.hs[i] = *known;
    U.lv_hosts.sel = i;
    save_config();
    stop_search();
    go_hosts();
}

static void add_search_result(int r)
{
    struct mdns_result res = U.md.r[r];
    struct hstatus st = U.ms[r];

    add_host_and_check(res.addr, res.name, &st);
}

/* ---- stream menu ------------------------------------------------------ */

enum { MI_RESUME, MI_MOUSE, MI_GUIDE, MI_DISCONNECT, MI_QUIT, MENU_N };

static const char *menu_item(int i)
{
    switch (i) {
    case MI_RESUME: return "Resume";
    case MI_MOUSE: return J.mouse ? "Mouse mode: On" : "Mouse mode: Off";
    case MI_GUIDE: return "Send Home (Guide) button";
    case MI_DISCONNECT: return "Disconnect";
    default: return "Quit app and disconnect";
    }
}

static void stream_stop(int quit_app)
{
    engine_send(&U.eng, quit_app ? "quit-app" : "stop");
    J.deadline = 0;
    J.kill_at = now_ms() + T_STOP;
    busy(quit_app ? "Quitting app" : "Disconnecting", J.app_name, 0);
}

static void menu_key(enum ui_key k)
{
    switch (k) {
    case UK_UP: list_move(&U.lv_menu, MENU_N, -1); break;
    case UK_DOWN: list_move(&U.lv_menu, MENU_N, 1); break;
    case UK_B:
    case UK_MENU:
        engine_send(&U.eng, "resume");
        enter_stream_screen();
        break;
    case UK_A:
        switch (U.lv_menu.sel) {
        case MI_RESUME:
            engine_send(&U.eng, "resume");
            enter_stream_screen();
            break;
        case MI_MOUSE:
            /* Toggle and go straight back to the stream. */
            J.mouse = !J.mouse;
            log_msg("stream: mouse mode %s", J.mouse ? "on" : "off");
            engine_send(&U.eng, J.mouse ? "mouse on" : "mouse off");
            engine_send(&U.eng, "resume");
            enter_stream_screen();
            break;
        case MI_GUIDE:
            engine_send(&U.eng, "resume");
            engine_send(&U.eng, "guide");
            enter_stream_screen();
            break;
        case MI_DISCONNECT:
            stream_stop(0);
            break;
        case MI_QUIT:
            stream_stop(1);
            break;
        }
        break;
    default:
        break;
    }
}

/* ---- settings --------------------------------------------------------- */

static const int bitrates[] = { 5000, 8000, 10000, 15000, 20000, 30000 };
#define NBITRATES 6

enum { ST_FPS, ST_BITRATE, ST_CODEC, ST_SWAP, ST_QUIT_AFTER, ST_WIFI_PS,
       ST_SOPS, ST_UPDATE_CHECK, ST_UPDATE_NOW, SETTINGS_N };

static void setting_text(int i, const char **label, char *value, size_t max,
                         const char **help)
{
    const struct ui_config *c = &U.cfg;

    switch (i) {
    case ST_FPS:
        *label = "Frame rate";
        snprintf(value, max, "%d fps", c->fps);
        *help = "30 fps uses less battery and network data. 60 fps gives "
                "smoother motion.";
        break;
    case ST_BITRATE:
        *label = "Bitrate";
        snprintf(value, max, "%d Mbps", c->bitrate / 1000);
        *help = "A higher bitrate gives a sharper picture but needs a strong "
                "Wi-Fi signal. 10 Mbps is good for 720p.";
        break;
    case ST_CODEC:
        *label = "Video codec";
        snprintf(value, max, "%s", c->codec == CODEC_HEVC ? "HEVC" :
                 c->codec == CODEC_H264 ? "H.264" : "Automatic");
        *help = "Automatic lets the PC choose (HEVC when it can). The "
                "hardware decodes both codecs.";
        break;
    case ST_SWAP:
        *label = "Swap A/B and X/Y";
        snprintf(value, max, "%s", c->swap_abxy ? "On" : "Off");
        *help = "Off: the bottom button sends A, as on an Xbox pad. On: each "
                "button sends its printed letter.";
        break;
    case ST_QUIT_AFTER:
        *label = "Quit the PC app on disconnect";
        snprintf(value, max, "%s", c->quit_after ? "On" : "Off");
        *help = "Off keeps the app open on the PC, so you can resume it "
                "later.";
        break;
    case ST_WIFI_PS:
        *label = "Wi-Fi power saving off while streaming";
        snprintf(value, max, "%s", c->wifi_ps_off ? "On" : "Off");
        *help = "On (recommended) prevents Wi-Fi stalls but uses a little "
                "more battery during the stream.";
        break;
    case ST_SOPS:
        *label = "Let the PC optimize game settings";
        snprintf(value, max, "%s", c->sops ? "On" : "Off");
        *help = "If On, Sunshine or GeForce Experience can change the game "
                "settings for this resolution.";
        break;
    case ST_UPDATE_CHECK:
        *label = "Check for updates at start";
        snprintf(value, max, "%s", c->update_check ? "On" : "Off");
        *help = "If On, the app asks GitHub for a new version each time it "
                "starts.";
        break;
    default:
        *label = "Check for updates now";
        snprintf(value, max, "v%s", UI_VERSION);
        *help = "Press A to ask GitHub for a new version. You have version "
                UI_VERSION ".";
        break;
    }
}

static void setting_change(int i, int d)
{
    struct ui_config *c = &U.cfg;
    int j;

    switch (i) {
    case ST_FPS:
        c->fps = c->fps == 60 ? 30 : 60;
        break;
    case ST_BITRATE:
        for (j = 0; j < NBITRATES && bitrates[j] < c->bitrate; j++)
            ;
        j += d;
        if (j < 0)
            j = NBITRATES - 1;
        if (j >= NBITRATES)
            j = 0;
        c->bitrate = bitrates[j];
        break;
    case ST_CODEC:
        c->codec = (c->codec + (d > 0 ? 1 : 2)) % 3;
        break;
    case ST_SWAP: c->swap_abxy = !c->swap_abxy; break;
    case ST_QUIT_AFTER: c->quit_after = !c->quit_after; break;
    case ST_WIFI_PS: c->wifi_ps_off = !c->wifi_ps_off; break;
    case ST_SOPS: c->sops = !c->sops; break;
    case ST_UPDATE_CHECK: c->update_check = !c->update_check; break;
    default: break;     /* an action, not a value */
    }
    redraw();
}

/* Where B leaves the settings screen. */
static enum screen settings_from;

static void open_settings(void)
{
    settings_from = U.scr;
    U.scr = S_SETTINGS;
    redraw();
}

/* ---- key handling ----------------------------------------------------- */

static const char keypad[] = "123456789.0<";
#define KEYPAD_N 13     /* 12 keys and Done */

static void addip_key(enum ui_key k)
{
    size_t n = strlen(U.ip);
    int s = U.key_sel;

    switch (k) {
    case UK_UP:
        s = s == 12 ? 10 : s >= 3 ? s - 3 : 12;
        break;
    case UK_DOWN:
        s = s == 12 ? 1 : s + 3 >= 12 ? 12 : s + 3;
        break;
    case UK_LEFT:
        if (s != 12)
            s = s % 3 == 0 ? s + 2 : s - 1;
        break;
    case UK_RIGHT:
        if (s != 12)
            s = s % 3 == 2 ? s - 2 : s + 1;
        break;
    case UK_Y:
        if (n > 0)
            U.ip[n - 1] = '\0';
        break;
    case UK_B:
        if (n > 0)
            U.ip[n - 1] = '\0';
        else
            go_hosts();
        break;
    case UK_MENU:
        go_hosts();
        return;
    case UK_START:
        s = 12;
        /* fall through */
    case UK_A:
        if (s == 12) {
            struct in_addr a;

            if (inet_pton(AF_INET, U.ip, &a) == 1)
                add_host_and_check(U.ip, U.ip, NULL);
            else
                message("Not an address", "Type an IPv4 address, for "
                        "example 192.168.0.10.");
            break;
        }
        if (keypad[s] == '<') {
            if (n > 0)
                U.ip[n - 1] = '\0';
        } else if (n + 1 < sizeof(U.ip)) {
            U.ip[n] = keypad[s];
            U.ip[n + 1] = '\0';
        }
        break;
    default:
        break;
    }
    U.key_sel = s;
    redraw();
}

static void quit_running_done(int choice)
{
    if (choice == 0) {
        U.pending_app = -1;
        start_quit();
    }
}

static void switch_app_done(int choice)
{
    if (choice == 0)
        start_quit();      /* pending_app is set: streams afterwards */
    else
        U.pending_app = -1;
}

static void apps_key(enum ui_key k)
{
    int running = U.hs[U.host].running;
    int i;

    switch (k) {
    case UK_UP: list_move(&U.lv_apps, U.napps, -1); break;
    case UK_DOWN: list_move(&U.lv_apps, U.napps, 1); break;
    case UK_L1: list_move(&U.lv_apps, U.napps, -LIST_ROWS); break;
    case UK_R1: list_move(&U.lv_apps, U.napps, LIST_ROWS); break;
    case UK_B:
    case UK_MENU:
        go_hosts();
        break;
    case UK_X:
        open_settings();
        break;
    case UK_Y:
        if (running == 0)
            break;
        for (i = 0; i < U.napps; i++)
            if (U.apps[i].id == running)
                break;
        {
            char text[200];

            snprintf(text, sizeof(text), "Quit %s on %s? You will lose "
                     "unsaved progress.", i < U.napps ? U.apps[i].name :
                     "the app",
                     host_name(U.host));
            dialog("Quit app", text, quit_running_done, "Quit", "Cancel");
            U.dlg.sel = 1;
        }
        break;
    case UK_A:
        if (U.napps == 0)
            break;
        {
            struct app *a = &U.apps[U.lv_apps.sel];

            if (running != 0 && running != a->id) {
                char text[240];

                for (i = 0; i < U.napps; i++)
                    if (U.apps[i].id == running)
                        break;
                U.pending_app = a->id;
                snprintf(U.pending_name, sizeof(U.pending_name), "%s",
                         a->name);
                snprintf(text, sizeof(text), "Quit %s and start %s? You "
                         "will lose unsaved progress.", i < U.napps ?
                         U.apps[i].name : "the open app", a->name);
                dialog("Switch app", text, switch_app_done, "Quit and start",
                       "Cancel");
                U.dlg.sel = 1;
            } else {
                start_stream(a->id, a->name);
            }
        }
        break;
    default:
        break;
    }
}

static void hosts_key(enum ui_key k)
{
    int n = U.cfg.nhosts + 2, s = U.lv_hosts.sel;

    switch (k) {
    case UK_UP: list_move(&U.lv_hosts, n, -1); break;
    case UK_DOWN: list_move(&U.lv_hosts, n, 1); break;
    case UK_B:
    case UK_MENU:
        U.quit = 1;
        break;
    case UK_X:
        open_settings();
        break;
    case UK_SELECT:
        recheck_hosts();
        start_next_check();
        redraw();
        break;
    case UK_Y:
        if (s < U.cfg.nhosts) {
            char text[160];

            snprintf(text, sizeof(text), "Remove %s (%s) from the list? "
                     "The PC keeps the pairing.", U.cfg.hosts[s].name,
                     U.cfg.hosts[s].addr);
            dialog("Remove PC", text, remove_host_done, "Remove", "Cancel");
            U.dlg.sel = 1;
        }
        break;
    case UK_A:
        if (s < U.cfg.nhosts) {
            if (U.hs[s].st == H_OFFLINE) {
                /* Try again; open it if the check succeeds. */
                job_cancel();
                U.hs[s].st = H_UNKNOWN;
            }
            open_host(s);
        } else if (s == U.cfg.nhosts) {
            job_cancel();
            start_search();
        } else {
            job_cancel();
            start_addip();
        }
        break;
    default:
        break;
    }
}

static void search_key(enum ui_key k)
{
    switch (k) {
    case UK_UP: list_move(&U.lv_search, U.md.n, -1); break;
    case UK_DOWN: list_move(&U.lv_search, U.md.n, 1); break;
    case UK_X:
        start_search();
        break;
    case UK_B:
    case UK_MENU:
        stop_search();
        go_hosts();
        break;
    case UK_A:
        if (U.md.n > 0)
            add_search_result(U.lv_search.sel);
        break;
    default:
        break;
    }
}

static void settings_key(enum ui_key k)
{
    switch (k) {
    case UK_UP: list_move(&U.lv_settings, SETTINGS_N, -1); break;
    case UK_DOWN: list_move(&U.lv_settings, SETTINGS_N, 1); break;
    case UK_LEFT: setting_change(U.lv_settings.sel, -1); break;
    case UK_RIGHT: setting_change(U.lv_settings.sel, 1); break;
    case UK_A:
        if (U.lv_settings.sel == ST_UPDATE_NOW) {
            save_config();
            start_manual_update_check();
        } else {
            setting_change(U.lv_settings.sel, 1);
        }
        break;
    case UK_B:
    case UK_MENU:
        save_config();
        U.scr = settings_from;
        redraw();
        if (U.scr == S_HOSTS)
            start_next_check();
        break;
    default:
        break;
    }
}

static void cancel_job_from_ui(void)
{
    if (J.kind == J_STREAM) {
        J.killed = K_CANCEL;
        engine_signal(&U.eng, SIGTERM);
        J.deadline = 0;
        J.kill_at = now_ms() + T_TERM_GRACE;
        busy("Canceling", J.app_name, 0);
        return;
    }
    if (J.kind == J_PAIR || J.kind == J_LIST) {
        job_cancel();
        go_hosts();
    } else if (J.kind == J_UPDATE_CHECK) {
        job_cancel();
        U.upd_manual = 0;
        update_return();
    } else if (J.kind == J_UPDATE_FETCH) {
        log_msg("update: download stopped by the user");
        job_cancel();
        update_remove_partial();
        update_return();
        message("Update stopped", "You can update later. Use \"Check for "
                "updates now\" in Settings.");
    }
}

static void handle_key(enum ui_key k)
{
    if (k == UK_NONE)
        return;
    if (U.dlg.active) {
        dialog_key(k);
        return;
    }
    switch (U.scr) {
    case S_HOSTS: hosts_key(k); break;
    case S_SEARCH: search_key(k); break;
    case S_ADDIP: addip_key(k); break;
    case S_APPS: apps_key(k); break;
    case S_SETTINGS: settings_key(k); break;
    case S_MENU: menu_key(k); break;
    case S_PAIR:
    case S_BUSY:
    case S_CONNECTING:
        if ((k == UK_B || k == UK_MENU) &&
            (U.scr != S_BUSY || U.busy_cancel))
            cancel_job_from_ui();
        break;
    case S_STREAM:
        break;
    }
}

/* ---- drawing ---------------------------------------------------------- */

struct hint {
    const char *key;    /* "A", "B", ... or a word for pill keys */
    const char *label;
};

static void draw_hints(const struct hint *h, int n)
{
    int x = 48, y = 672, i;

    draw_rect(0, 640, UI_W, 80, C_HEAD);
    for (i = 0; i < n; i++) {
        int klen = (int)strlen(h[i].key);

        if (klen == 1) {
            draw_disc(x + 18, y + 8, 18, C_KEY);
            draw_text(F_SMALL, x + 18, y - 6, C_KEYTXT, h[i].key, AL_CENTER,
                      0);
            x += 46;
        } else {
            int w = draw_text_width(F_SMALL, h[i].key) + 24;

            draw_round_rect(x, y - 10, w, 36, 18, C_KEY);
            draw_text(F_SMALL, x + w / 2, y - 6, C_KEYTXT, h[i].key,
                      AL_CENTER, 0);
            x += w + 10;
        }
        x += draw_text(F_SMALL, x, y - 6, C_TEXT, h[i].label, AL_LEFT, 0);
        x += 36;
    }
}

static const char *status_text(const struct hstatus *s, uint32_t *col)
{
    *col = C_DIM;
    switch (s->st) {
    case H_OFFLINE:
        *col = C_ERR;
        return "offline";
    case H_ONLINE:
        *col = s->paired ? C_OK : C_WARN;
        return s->paired ? "paired" : "not paired";
    default:
        return "checking...";
    }
}

static void draw_header(const char *title, const char *right)
{
    draw_rect(0, 0, UI_W, 88, C_HEAD);
    draw_text(F_TITLE, 48, 20, C_TEXT, title, AL_LEFT, 760);
    if (right != NULL)
        draw_text(F_SMALL, UI_W - 48, 32, C_DIM, right, AL_RIGHT, 420);
}

static void draw_row(int vis, int selected, const char *title,
                     const char *right, uint32_t right_color,
                     const char *badge, uint32_t badge_color)
{
    int y = LIST_Y + vis * row_pitch;
    int th = draw_font_height(F_BODY);
    int x = LIST_X + 28, rw = 0;

    draw_round_rect(LIST_X, y, LIST_W, row_h, 12, selected ? C_SEL : C_ROW);
    if (right != NULL && right[0] != '\0')
        rw = draw_text(F_SMALL, LIST_X + LIST_W - 28,
                       y + (row_h - draw_font_height(F_SMALL)) / 2,
                       selected ? C_TEXT : right_color, right, AL_RIGHT, 520);
    x += draw_text(F_BODY, x, y + (row_h - th) / 2, C_TEXT, title, AL_LEFT,
                   LIST_W - 80 - rw - (badge ? 160 : 0));
    if (badge != NULL) {
        int bw = draw_text_width(F_SMALL, badge) + 24;
        int by = y + (row_h - 32) / 2;

        draw_round_rect(x + 16, by, bw, 32, 16, badge_color);
        draw_text(F_SMALL, x + 16 + bw / 2,
                  by + (32 - draw_font_height(F_SMALL)) / 2, C_KEYTXT,
                  badge, AL_CENTER, 0);
    }
}

static void draw_scrollbar(const struct listview *l, int n)
{
    int h, y;

    if (n <= LIST_ROWS)
        return;
    h = LIST_ROWS * row_pitch * LIST_ROWS / n;
    y = LIST_Y + (LIST_ROWS * row_pitch - h) * l->top / (n - LIST_ROWS);
    draw_round_rect(LIST_X + LIST_W + 12, y, 8, h, 4, C_DIM);
}

static void draw_centered_note(const char *a, const char *b)
{
    draw_text(F_BODY, UI_W / 2, 300, C_TEXT, a, AL_CENTER, 1100);
    if (b != NULL)
        draw_text(F_SMALL, UI_W / 2, 350, C_DIM, b, AL_CENTER, 1100);
}

static void render_hosts(void)
{
    struct listview *l = &U.lv_hosts;
    int n = U.cfg.nhosts + 2, i;
    static const struct hint h1[] = {
        { "A", "Open" }, { "Y", "Remove" }, { "X", "Settings" },
        { "SELECT", "Refresh" }, { "B", "Exit" },
    };
    static const struct hint h2[] = {
        { "A", "Select" }, { "X", "Settings" }, { "B", "Exit" },
    };

    draw_header("Moonlight HW", U.perf_scene ? NULL :
                "v" UI_VERSION ", hardware decoding");
    if (U.perf_scene)
        draw_text(F_SMALL, UI_W - 48, 32, C_WARN,
                  "The CPU Performance scene holds the CPU at 2 GHz and "
                  "uses more battery.", AL_RIGHT, 840);
    list_clamp(l, n);
    for (i = l->top; i < n && i < l->top + LIST_ROWS; i++) {
        int sel = i == l->sel;

        if (i < U.cfg.nhosts) {
            const struct hstatus *s = &U.hs[i];
            char right[160];
            uint32_t col;
            const char *st = status_text(s, &col);

            if (s->st == H_ONLINE && s->paired)
                st = "online";
            snprintf(right, sizeof(right), "%s, %s", U.cfg.hosts[i].addr,
                     st);
            draw_row(i - l->top, sel, U.cfg.hosts[i].name, right, col,
                     s->st == H_ONLINE && s->paired && s->running ?
                     "in game" : NULL,
                     C_OK);
        } else if (i == U.cfg.nhosts) {
            draw_row(i - l->top, sel, "Search for PCs", "Sunshine on this "
                     "network", C_DIM, NULL, 0);
        } else {
            draw_row(i - l->top, sel, "Add PC by IP address", NULL, 0, NULL,
                     0);
        }
    }
    draw_scrollbar(l, n);
    if (U.cfg.nhosts == 0)
        draw_text(F_SMALL, UI_W / 2, 560, C_DIM, "No PCs yet. Start "
                  "Sunshine on your PC. Then search for it, or add it by its "
                  "IP address.",
                  AL_CENTER, 1150);
    if (l->sel < U.cfg.nhosts)
        draw_hints(h1, 5);
    else
        draw_hints(h2, 3);
}

static void render_search(void)
{
    struct listview *l = &U.lv_search;
    int i, j;
    static const struct hint h[] = {
        { "A", "Add" }, { "X", "Search again" }, { "B", "Back" },
    };
    int nh = U.md.n > 0 ? 3 : 2;

    draw_header("Search for PCs", U.search_end ? "searching..." : NULL);
    list_clamp(l, U.md.n);
    for (i = l->top; i < U.md.n && i < l->top + LIST_ROWS; i++) {
        int saved = 0;
        char right[160];
        uint32_t col;
        const char *st = status_text(&U.ms[i], &col);

        for (j = 0; j < U.cfg.nhosts; j++)
            if (strcmp(U.cfg.hosts[j].addr, U.md.r[i].addr) == 0)
                saved = 1;
        snprintf(right, sizeof(right), "%s, %s", U.md.r[i].addr, st);
        draw_row(i - l->top, i == l->sel, U.md.r[i].name, right, col,
                 saved ? "saved" : NULL, C_DIM);
    }
    if (U.md.n > 0 && U.md.n < LIST_ROWS)
        draw_text(F_SMALL, LIST_X + 8, LIST_Y + U.md.n * ROW_PITCH + 12, C_DIM,
                  "A \"paired\" PC already accepts this device. Other PCs "
                  "need a PIN that you type in Sunshine.", AL_LEFT,
                  LIST_W - 16);
    if (U.md.n == 0)
        draw_centered_note(U.search_end ? "Searching..." :
                           "No PCs found",
                           U.search_end ? NULL : "Make sure that Sunshine "
                           "is on, or add the PC by its IP address.");
    draw_hints(U.md.n > 0 ? h : h + 1, nh);
}

static void render_addip(void)
{
    static const struct hint h[] = {
        { "A", "Press" }, { "Y", "Delete" }, { "START", "Done" },
        { "B", "Back" },
    };
    int i, kx = 440, ky = 220, kw = 120, kh = 76, gap = 12;
    char shown[24];

    draw_header("Add PC by IP address", NULL);
    draw_round_rect(340, 116, 600, 80, 12, C_ROW);
    snprintf(shown, sizeof(shown), "%s_", U.ip);
    draw_text(F_TITLE, UI_W / 2, 132, C_TEXT, shown, AL_CENTER, 560);
    for (i = 0; i < 12; i++) {
        int x = kx + (i % 3) * (kw + gap), y = ky + (i / 3) * (kh + gap);
        char lab[8];

        if (keypad[i] == '<')
            snprintf(lab, sizeof(lab), "Del");
        else
            snprintf(lab, sizeof(lab), "%c", keypad[i]);
        draw_round_rect(x, y, kw, kh, 12, i == U.key_sel ? C_SEL : C_ROW);
        draw_text(F_TITLE, x + kw / 2, y + (kh - draw_font_height(F_TITLE)) / 2,
                  C_TEXT, lab, AL_CENTER, 0);
    }
    {
        int y = ky + 4 * (kh + gap), w = 3 * kw + 2 * gap;

        draw_round_rect(kx, y, w, 60, 12, U.key_sel == 12 ? C_SEL : C_ROW);
        draw_text(F_BODY, kx + w / 2, y + (60 - draw_font_height(F_BODY)) / 2,
                  C_TEXT, "Done", AL_CENTER, 0);
    }
    draw_hints(h, 4);
}

static void render_apps(void)
{
    struct listview *l = &U.lv_apps;
    char right[128];
    int i;
    static const struct hint h1[] = {
        { "A", "Stream" }, { "Y", "Quit running app" }, { "X", "Settings" },
        { "B", "Back" },
    };
    static const struct hint h2[] = {
        { "A", "Stream" }, { "X", "Settings" }, { "B", "Back" },
    };

    snprintf(right, sizeof(right), "%s%s  720p%d  %d Mbps  %s",
             J.kind == J_LIST ? "updating...  " : "", U.cfg.hosts[U.host].addr,
             U.cfg.fps, U.cfg.bitrate / 1000,
             U.cfg.codec == CODEC_HEVC ? "HEVC" :
             U.cfg.codec == CODEC_H264 ? "H.264" : "auto codec");
    draw_header(host_name(U.host), right);
    list_clamp(l, U.napps);
    for (i = l->top; i < U.napps && i < l->top + LIST_ROWS; i++)
        draw_row(i - l->top, i == l->sel, U.apps[i].name, NULL, 0,
                 U.apps[i].running ? "running" : NULL, C_OK);
    draw_scrollbar(l, U.napps);
    if (U.napps == 0)
        draw_centered_note(U.apps_loaded ? "No apps on this PC" :
                           "Loading...", NULL);
    if (U.hs[U.host].running)
        draw_hints(h1, 4);
    else
        draw_hints(h2, 3);
}

static void render_pair(void)
{
    static const struct hint h[] = { { "B", "Cancel" } };
    int i, w = 150, gap = 28, x0 = UI_W / 2 - (4 * w + 3 * gap) / 2;
    char line[200];

    draw_header("Pairing", NULL);
    snprintf(line, sizeof(line), "Type this PIN in Sunshine on %s (%s):",
             host_name(J.host), U.cfg.hosts[J.host].addr);
    draw_text(F_BODY, UI_W / 2, 140, C_TEXT, line, AL_CENTER, 1180);
    for (i = 0; i < 4 && U.pin[i] != '\0'; i++) {
        char d[2] = { U.pin[i], '\0' };
        int x = x0 + i * (w + gap);

        draw_round_rect(x, 220, w, 200, 18, C_ROW);
        draw_text(F_HUGE, x + w / 2, 220 + (200 - draw_font_height(F_HUGE)) / 2,
                  C_ACCENT, d, AL_CENTER, 0);
    }
    draw_text(F_SMALL, UI_W / 2, 470, C_DIM, "Open the PIN page in the "
              "Sunshine web UI. You can use a device name of your choice.",
              AL_CENTER, 1180);
    draw_text(F_SMALL, UI_W / 2, 506, C_DIM, "Only that PC gets this "
              "request.", AL_CENTER, 1180);
    snprintf(line, sizeof(line), "This screen closes after the PC accepts "
             "the PIN. It waits for %d minutes at most.", T_PAIR / 60000);
    draw_text(F_SMALL, UI_W / 2, 542, C_DIM, line, AL_CENTER, 1180);
    draw_hints(h, 1);
}

static void render_busy(void)
{
    static const struct hint h[] = { { "B", "Cancel" } };
    char el[32];
    uint64_t now = now_ms();

    draw_header(U.busy_title, NULL);
    snprintf(el, sizeof(el), "%llu s",
             (unsigned long long)(ms_since(now, U.busy_since) / 1000));
    draw_centered_note(U.busy_text, el);
    if (U.busy_cancel)
        draw_hints(h, 1);
    else
        draw_hints(NULL, 0);
}

static void render_connecting(void)
{
    static const struct hint h[] = { { "B", "Cancel" } };
    char title[128], sub[160];
    uint64_t now = now_ms();

    snprintf(title, sizeof(title), "Starting %s", J.app_name);
    draw_header(title, host_name(J.host));
    snprintf(sub, sizeof(sub), "%s, %llu s", J.stage[0] ? J.stage :
             "contacting the PC",
             (unsigned long long)(ms_since(now, J.start) / 1000));
    draw_centered_note("Connecting...", sub);
    draw_text(F_SMALL, UI_W / 2, 540, C_DIM, "During the stream, press MENU "
              "for the stream menu, or hold MENU to use the pad as a mouse.",
              AL_CENTER, 1180);
    draw_text(F_SMALL, UI_W / 2, 576, C_DIM, "To disconnect at once, hold "
              "START+SELECT+L1+R1 for one second.", AL_CENTER, 1180);
    draw_hints(h, 1);
}

static void render_menu(void)
{
    static const struct hint h[] = {
        { "A", "Select" }, { "B", "Resume" },
    };
    char title[128];
    int i, x = 720, y = LIST_Y;

    snprintf(title, sizeof(title), "Paused: %s", J.app_name);
    draw_header(title, host_name(J.host));
    for (i = 0; i < MENU_N; i++) {
        int ry = LIST_Y + i * ROW_PITCH;

        draw_round_rect(LIST_X, ry, 620, ROW_H, 12,
                        i == U.lv_menu.sel ? C_SEL : C_ROW);
        draw_text(F_BODY, LIST_X + 28, ry + (ROW_H -
                  draw_font_height(F_BODY)) / 2, C_TEXT, menu_item(i),
                  AL_LEFT, 570);
    }
    draw_round_rect(x, y, 520, 4 * ROW_PITCH - 8, 12, C_ROW);
    draw_text(F_BODY, x + 24, y + 14, C_TEXT, "Stream", AL_LEFT, 0);
    if (U.l_valid) {
        char b[6][96];

        snprintf(b[0], sizeof(b[0]), "%.0f fps, %.1f Mbps", U.l_fps,
                 U.l_kbps / 1000.0);
        snprintf(b[1], sizeof(b[1]), "Latency %.1f ms (decode %.1f)",
                 U.l_lat, U.l_dec);
        snprintf(b[2], sizeof(b[2]), "Network round trip %u ms", U.l_rtt);
        snprintf(b[3], sizeof(b[3]), "Audio lost %lu, dropped %lu",
                 U.l_lost, U.l_drop);
        snprintf(b[4], sizeof(b[4]), "Engine CPU %.1f%% of one core",
                 U.l_cpu);
        snprintf(b[5], sizeof(b[5]), "720p%d, %s", U.cfg.fps, codec_arg());
        for (i = 0; i < 6; i++)
            draw_text(F_SMALL, x + 24, y + 62 + i * 34, C_DIM, b[i],
                      AL_LEFT, 470);
    }
    draw_text(F_SMALL, LIST_X, 504, C_DIM, "Hold MENU during play to use the "
              "pad as a mouse. \"Mouse mode: On\" keeps the mouse on.",
              AL_LEFT, 1200);
    draw_text(F_SMALL, LIST_X, 536, C_DIM, "Right stick: pointer. Left stick: "
              "scroll. A, L1, R1: left click. B, L2, R2: right click.",
              AL_LEFT, 1200);
    draw_text(F_SMALL, LIST_X, 568, C_DIM, "X: middle click. Y: Enter. D-pad: "
              "arrow keys.", AL_LEFT, 1200);
    draw_text(F_SMALL, LIST_X, 602, C_DIM, "Hold START+SELECT+L1+R1 for one "
              "second during play to disconnect at once.", AL_LEFT, 1200);
    draw_hints(h, 2);
}

static void render_settings(void)
{
    struct listview *l = &U.lv_settings;
    static const struct hint h[] = {
        { "<>", "Change" }, { "B", "Save and back" },
    };
    static const struct hint ha[] = {
        { "A", "Check" }, { "B", "Save and back" },
    };
    const char *help = "";
    int i;

    draw_header("Settings", "720p (1280x720), the native screen size");
    row_h = 56;
    row_pitch = 64;
    list_clamp(l, SETTINGS_N);
    for (i = l->top; i < SETTINGS_N && i < l->top + LIST_ROWS; i++) {
        const char *label, *hp;
        char value[64];

        setting_text(i, &label, value, sizeof(value), &hp);
        if (i == l->sel)
            help = hp;
        draw_row(i - l->top, i == l->sel, label, value, C_ACCENT, NULL, 0);
    }
    draw_scrollbar(l, SETTINGS_N);
    draw_text(F_SMALL, LIST_X + 4, LIST_Y + LIST_ROWS * row_pitch + 12, C_DIM,
              help, AL_LEFT, LIST_W);
    row_h = ROW_H;
    row_pitch = ROW_PITCH;
    draw_hints(l->sel == ST_UPDATE_NOW ? ha : h, 2);
}

#define DIALOG_LINES 5

/*
 * Wraps T into at most MAX lines of at most WIDTH pixels, on word
 * boundaries; '\n' starts a new line. Returns the number of lines.
 */
static int wrap_text(int font, const char *t, int width, char out[][320],
                     int max)
{
    int line = 0;

    while (*t != '\0' && line < max) {
        size_t n = 0, best = 0;

        while (t[n] != '\0' && t[n] != '\n') {
            size_t m = n;

            while (t[m] == ' ')
                m++;
            while (t[m] != '\0' && t[m] != ' ' && t[m] != '\n')
                m++;
            if (m >= 320)
                break;
            memcpy(out[line], t, m);
            out[line][m] = '\0';
            if (draw_text_width(font, out[line]) > width && best > 0)
                break;
            best = m;
            n = m;
        }
        memcpy(out[line], t, best);
        out[line][best] = '\0';
        line++;
        t += best;
        while (*t == ' ')
            t++;
        if (*t == '\n')
            t++;
    }
    return line;
}

static void render_dialog(void)
{
    struct dialog *d = &U.dlg;
    char lines[DIALOG_LINES][320];
    int w = 860, nl = wrap_text(F_BODY, d->text, w - 80, lines, DIALOG_LINES);
    int h = 330 + (nl > 3 ? (nl - 3) * 40 : 0);
    int x = (UI_W - w) / 2, y = (UI_H - h) / 2, i;
    int bw = d->nbtn > 2 ? 240 : 260, bx;

    draw_rect(0, 0, UI_W, UI_H, 0x000000a0);
    draw_round_rect(x, y, w, h, 18, C_HEAD);
    draw_text(F_TITLE, x + 40, y + 28, C_TEXT, d->title, AL_LEFT, w - 80);
    for (i = 0; i < nl; i++)
        draw_text(F_BODY, x + 40, y + 96 + i * 40, C_TEXT, lines[i], AL_LEFT,
                  w - 80);
    bx = x + w - 40 - d->nbtn * bw - (d->nbtn - 1) * 20;
    for (i = 0; i < d->nbtn; i++) {
        int b = bx + i * (bw + 20);

        draw_round_rect(b, y + h - 92, bw, 60, 12,
                        i == d->sel ? C_SEL : C_ROW);
        draw_text(F_BODY, b + bw / 2, y + h - 92 +
                  (60 - draw_font_height(F_BODY)) / 2, C_TEXT, d->btn[i],
                  AL_CENTER, bw - 20);
    }
}

static void render(void)
{
    draw_clear(U.scr == S_STREAM ? 0x000000ff : C_BG);
    switch (U.scr) {
    case S_HOSTS: render_hosts(); break;
    case S_SEARCH: render_search(); break;
    case S_ADDIP: render_addip(); break;
    case S_APPS: render_apps(); break;
    case S_PAIR: render_pair(); break;
    case S_SETTINGS: render_settings(); break;
    case S_BUSY: render_busy(); break;
    case S_CONNECTING: render_connecting(); break;
    case S_MENU: render_menu(); break;
    case S_STREAM: break;
    }
    if (U.dlg.active && U.scr != S_STREAM)
        render_dialog();
    if (U.shot_path[0] != '\0') {
        if (draw_screenshot(U.shot_path) != 0)
            log_msg("screenshot %s failed", U.shot_path);
        U.shot_path[0] = '\0';
    }
    draw_present();
    /* Screens with elapsed time redraw once a second. */
    U.tick_at = (U.scr == S_BUSY || U.scr == S_CONNECTING) ?
                now_ms() + 1000 : 0;
}

/* ---- debug FIFO ------------------------------------------------------- */

static void fifo_command(char *line)
{
    if (strncmp(line, "shot ", 5) == 0) {
        snprintf(U.shot_path, sizeof(U.shot_path), "%s", line + 5);
        redraw();
    } else if (strcmp(line, "quit") == 0) {
        U.quit = 1;
    } else if (strncmp(line, "mdns ", 5) == 0 && U.scr == S_SEARCH &&
               U.md.n < MDNS_MAX_RESULTS) {
        /* "mdns ADDR NAME": a fake search result for UI tests. */
        struct mdns_result *r = &U.md.r[U.md.n];
        char *sp = strchr(line + 5, ' ');

        if (sp != NULL) {
            *sp = '\0';
            memset(r, 0, sizeof(*r));
            snprintf(r->addr, sizeof(r->addr), "%s", line + 5);
            snprintf(r->name, sizeof(r->name), "%s", sp + 1);
            r->port = 47989;
            U.ms[U.md.n].st = H_UNKNOWN;
            U.md.n++;
            redraw();
            start_next_probe();
        }
    } else {
        enum ui_key k = ui_key_from_name(line);

        if (k != UK_NONE && U.scr != S_STREAM)
            handle_key(k);
        else if (k == UK_MENU && U.scr == S_STREAM)
            log_msg("fifo: MENU is read by the engine during a stream");
        else if (k == UK_NONE)
            log_msg("fifo: unknown command \"%s\"", line);
    }
}

static void fifo_read(void)
{
    ssize_t n = read(U.fifo_fd, U.fifo_buf + U.fifo_len,
                     sizeof(U.fifo_buf) - 1 - U.fifo_len);
    char *nl;

    if (n <= 0)
        return;
    U.fifo_len += (size_t)n;
    U.fifo_buf[U.fifo_len] = '\0';
    while ((nl = strchr(U.fifo_buf, '\n')) != NULL) {
        *nl = '\0';
        fifo_command(U.fifo_buf);
        U.fifo_len -= (size_t)(nl + 1 - U.fifo_buf);
        memmove(U.fifo_buf, nl + 1, U.fifo_len + 1);
    }
    if (U.fifo_len >= sizeof(U.fifo_buf) - 1)
        U.fifo_len = 0;
}

/* ---- main ------------------------------------------------------------- */

static void shutdown_engine(void)
{
    if (U.eng.pid <= 0)
        return;
    if (J.kind == J_STREAM) {
        uint64_t end = now_ms() + 3000;

        engine_send(&U.eng, "stop");
        while (now_ms() < end && !engine_reap(&U.eng)) {
            struct timespec ts = { 0, 50 * 1000000L };

            nanosleep(&ts, NULL);
        }
        engine_kill(&U.eng);
        engine_run_reset(U.engine_path, U.stream_log, 5000);
    } else {
        engine_kill(&U.eng);
    }
    J.kind = J_NONE;
}

static void setup_paths(const char *argv0, const char *engine)
{
    const char *d = getenv("MLTSP_DATA");
    const char *dd = getenv("MLTSP_TEST_DRAIN_DELAY_US");
    char self[PATH_MAX];
    ssize_t n;

    if (dd != NULL)
        U.drain_delay_us = (unsigned)strtoul(dd, NULL, 10);

    snprintf(U.data, sizeof(U.data), "%s", d && d[0] ? d : DEFAULT_DATA);
    snprintf(U.cfg_path, sizeof(U.cfg_path), "%s/config.ini", U.data);
    snprintf(U.keydir, sizeof(U.keydir), "%s/keys", U.data);
    snprintf(U.logdir, sizeof(U.logdir), "%s/logs", U.data);
    snprintf(U.stream_log, sizeof(U.stream_log), "%s/stream.log", U.logdir);
    snprintf(U.engine_log, sizeof(U.engine_log), "%s/engine.log", U.logdir);
    snprintf(U.update_log, sizeof(U.update_log), "%s/update.log", U.logdir);
    snprintf(U.update_result, sizeof(U.update_result), "%s/update-result",
             U.data);
    d = getenv("MLTSP_STAGE");
    snprintf(U.stage, sizeof(U.stage), "%s", d && d[0] ? d : DEFAULT_STAGE);
    d = getenv("MLTSP_UPDATE_URL");
    U.update_url = d && d[0] ? d : NULL;
    d = getenv("MLTSP_STARTED_FILE");
    U.started_file = d && d[0] ? d : NULL;
    mkdir_p(U.logdir);
    if (engine != NULL) {
        snprintf(U.engine_path, sizeof(U.engine_path), "%s", engine);
        return;
    }
    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        snprintf(self, sizeof(self), "%s", argv0);
        n = (ssize_t)strlen(self);
    }
    self[n] = '\0';
    if (strrchr(self, '/') != NULL)
        *strrchr(self, '/') = '\0';
    else
        snprintf(self, sizeof(self), ".");
    snprintf(U.engine_path, sizeof(U.engine_path), "%s/moonlight-tsp-stream",
             self);
}

static void usage(FILE *f)
{
    fprintf(f, "moonlight-tsp-ui " UI_VERSION "\n"
               "usage: moonlight-tsp-ui [--engine PATH] [--debug-fifo PATH]\n"
               "       moonlight-tsp-ui --version\n"
               "data: $MLTSP_DATA or " DEFAULT_DATA "\n"
               "exit status " "3: install the update in $MLTSP_STAGE\n");
}

int main(int argc, char **argv)
{
    const char *engine = NULL, *fifo = NULL;
    sigset_t set;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc) {
            engine = argv[++i];
        } else if (strcmp(argv[i], "--debug-fifo") == 0 && i + 1 < argc) {
            fifo = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            /* update-fetch runs this to prove that a download starts. */
            printf("moonlight-tsp-ui " UI_VERSION "\n");
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    memset(&U, 0, sizeof(U));
    U.fifo_fd = -1;
    U.md.fd = -1;
    U.pending_app = -1;
    engine_init(&U.eng);
    setup_paths(argv[0], engine);
    config_load(&U.cfg, U.cfg_path);
    log_msg("moonlight-tsp-ui " UI_VERSION ": data %s, engine %s, %d hosts",
            U.data, U.engine_path, U.cfg.nhosts);
    log_msg("update: check at start %s, stage %s%s%s",
            U.cfg.update_check ? "on" : "off", U.stage,
            U.cfg.update_skip[0] ? ", skipping " : "", U.cfg.update_skip);

    /* Signals before SDL so its handlers stay out of the way. */
    signal(SIGPIPE, SIG_IGN);
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigprocmask(SIG_BLOCK, &set, NULL);
    U.sfd = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);

    if (fifo != NULL) {
        if (mkfifo(fifo, 0600) != 0 && errno != EEXIST)
            log_msg("fifo %s: %s", fifo, strerror(errno));
        /* O_RDWR: never sees EOF when a writer goes away. */
        U.fifo_fd = open(fifo, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    }
    if (ui_pad_open(&U.pad) != 0)
        log_msg("pad: not found; only the debug FIFO can drive the UI");
    if (draw_init() != 0) {
        log_msg("display init failed");
        return 1;
    }
    log_msg("%s", draw_info());

    U.lv_hosts.sel = U.cfg.last_host;
    if (U.cfg.nhosts == 0)
        start_search();
    else
        go_hosts();
    read_update_result();

    while (!U.quit) {
        struct pollfd p[8];
        int np = 0, i_sig, i_pad = -1, i_rep = -1, i_eng = -1, i_md = -1;
        int i_fifo = -1, timeout = -1, j;
        uint64_t now, next = 0;
        enum ui_key keys[16];

        if (U.dirty) {
            U.dirty = 0;
            render();
            if (U.started_file != NULL) {
                /* update.sh: this version starts and draws. */
                int fd = open(U.started_file, O_WRONLY | O_CREAT |
                              O_CLOEXEC, 0644);

                if (fd >= 0)
                    close(fd);
                log_msg("update: wrote %s", U.started_file);
                U.started_file = NULL;
            }
        }

        i_sig = np;
        p[np].fd = U.sfd;
        p[np++].events = POLLIN;
        if (U.scr != S_STREAM && U.pad.fd >= 0) {
            i_pad = np;
            p[np].fd = U.pad.fd;
            p[np++].events = POLLIN;
            i_rep = np;
            p[np].fd = U.pad.repeat_fd;
            p[np++].events = POLLIN;
        }
        if (U.eng.fd >= 0) {
            i_eng = np;
            p[np].fd = U.eng.fd;
            p[np++].events = POLLIN;
        }
        if (U.md.fd >= 0) {
            i_md = np;
            p[np].fd = U.md.fd;
            p[np++].events = POLLIN;
        }
        if (U.fifo_fd >= 0) {
            i_fifo = np;
            p[np].fd = U.fifo_fd;
            p[np++].events = POLLIN;
        }

        now = now_ms();
        if (U.pad.fd < 0 && U.pad_retry_at == 0)
            U.pad_retry_at = now + 2000;
        next = job_next_deadline();
        if (U.pad_retry_at != 0 && (next == 0 || U.pad_retry_at < next))
            next = U.pad_retry_at;
        if (U.tick_at != 0 && (next == 0 || U.tick_at < next))
            next = U.tick_at;
        if (U.search_end != 0) {
            uint64_t s = U.search_next != 0 && U.search_next < U.search_end ?
                         U.search_next : U.search_end;

            if (next == 0 || s < next)
                next = s;
        }
        if (next != 0)
            timeout = next <= now ? 0 : (int)(next - now);

        if (poll(p, (nfds_t)np, timeout) < 0 && errno != EINTR)
            break;
        now = now_ms();

        if (p[i_sig].revents & POLLIN) {
            struct signalfd_siginfo si;

            while (read(U.sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                if (si.ssi_signo == SIGCHLD)
                    continue;
                log_msg("signal %u: exiting", si.ssi_signo);
                U.quit = 1;
            }
        }
        if (i_eng >= 0 && (p[i_eng].revents & (POLLIN | POLLHUP | POLLERR))) {
            /* Tests: lines arrive in a later millisecond than NOW. */
            if (U.drain_delay_us > 0)
                usleep(U.drain_delay_us);
            drain_engine();
        }
        if (J.kind != J_NONE && engine_reap(&U.eng))
            job_finished();
        if (i_pad >= 0 && (p[i_pad].revents & POLLIN)) {
            int n = ui_pad_read(&U.pad, keys, 16);

            for (j = 0; j < n && U.scr != S_STREAM; j++)
                handle_key(keys[j]);
        }
        if (i_rep >= 0 && (p[i_rep].revents & POLLIN) && U.scr != S_STREAM)
            handle_key(ui_pad_repeat(&U.pad));
        if (i_md >= 0 && (p[i_md].revents & POLLIN) &&
            mdns_read(&U.md) > 0) {
            start_next_probe();
            redraw();
        }
        if (i_fifo >= 0 && (p[i_fifo].revents & POLLIN))
            fifo_read();

        if (U.search_end != 0) {
            if (now >= U.search_end) {
                /* Close the socket (it sees all mDNS traffic on the LAN)
                 * but let the probes of the results finish. */
                U.search_end = U.search_next = 0;
                mdns_read(&U.md);
                mdns_close(&U.md);
                log_msg("search: %d found", U.md.n);
                start_next_probe();
                redraw();
            } else if (U.search_next != 0 && now >= U.search_next) {
                mdns_query(&U.md);
                U.search_round++;
                U.search_next = U.search_round < 3 ?
                                now + (uint64_t)(U.search_round * 1000) : 0;
            }
        }
        job_timers(now);
        if (U.pad_retry_at != 0 && now >= U.pad_retry_at) {
            U.pad_retry_at = 0;
            if (ui_pad_reopen(&U.pad) == 0) {
                log_msg("pad: found");
                ui_pad_drain(&U.pad);
            }
        }
        if (U.tick_at != 0 && now >= U.tick_at)
            redraw();
        if (U.scr == S_HOSTS && !U.dlg.active)
            start_next_check();
        show_update_offer();
        start_update_check();
    }

    log_msg("exiting");
    shutdown_engine();
    stop_search();
    save_config();
    draw_quit();
    ui_pad_close(&U.pad);
    return U.exit_code;
}
