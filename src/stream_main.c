/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * moonlight-tsp-stream: the headless streaming engine. The UI runs it with
 * --ipc-fd; it is also a CLI for tests over SSH.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <Limelight.h>
#include <client.h>
#include <errors.h>

#include "audio_alsa.h"
#include "cedar_sync.h"
#include "fdguard.h"
#include "input_evdev.h"
#include "update.h"
#include "video_cedar.h"
#include "wifi_ps.h"

#define ENGINE_VERSION "0.3.1"
#define DEFAULT_KEYDIR "/mnt/UDISK/trimui-tools/moonlight-tsp/keys"
#define LEGACY_KEYDIR "/.cache/moonlight"
#define DEFAULT_UPDATE_URL "https://github.com/isalin/moonlight-tsp/releases"
#define DEFAULT_CA_FILE "/etc/ssl/certs/ca-certificates.crt"
#define DEFAULT_STAGE "/mnt/SDCARD/" UPDATE_STAGE_NAME
#define DEFAULT_UNZIP "/usr/bin/unzip"

static struct {
    const char *keydir;
    const char *host;
    unsigned port;
    int verbose;
    int no_import;
    int width, height, fps, bitrate;
    const char *codec;
    int audio_latency;
    const char *audio_device;
    int swap_abxy;
    int mouse;
    int quit_after;
    int sops;
    int duration;
    int stats;
    int ipc_fd;
    int no_input;
    int keep_wifi_ps;
    const char *update_url, *ca_file, *stage, *unzip;
} O = {
    .keydir = DEFAULT_KEYDIR,
    .width = 1280,
    .height = 720,
    .fps = 60,
    .bitrate = 10000,
    .codec = "auto",
    .audio_latency = 20,
    .ipc_fd = -1,
    .update_url = DEFAULT_UPDATE_URL,
    .ca_file = DEFAULT_CA_FILE,
    .stage = DEFAULT_STAGE,
    .unzip = DEFAULT_UNZIP,
};

/* Engine phase, read by the signal thread. */
enum { PH_PRE, PH_CONNECTING, PH_STREAMING, PH_STOPPING };
static atomic_int g_phase;
static atomic_int g_stop_sig;
static atomic_int g_terminated;
static atomic_int g_term_code;
static int g_evpipe[2] = { -1, -1 };
static pthread_mutex_t g_ipc_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_ipc_drops;      /* under g_ipc_lock */
static int g_fdguard;                   /* close() interposed process-wide */

static void wake(void)
{
    char c = 0;
    ssize_t r = write(g_evpipe[1], &c, 1);

    (void)r;
}

static void ipc_send(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int n;

    if (O.ipc_fd < 0)
        return;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    pthread_mutex_lock(&g_ipc_lock);
    /* Non-blocking: a stalled UI loses lines instead of stalling us. */
    if (O.ipc_fd >= 0 && write(O.ipc_fd, line, (size_t)n) < 0) {
        if (errno != EAGAIN) {
            fprintf(stderr, "ipc: write failed (%s); UI link lost\n",
                    strerror(errno));
            O.ipc_fd = -1;
        } else if (g_ipc_drops++ == 0) {
            fprintf(stderr, "ipc: UI is not reading; dropping lines\n");
        }
    }
    pthread_mutex_unlock(&g_ipc_lock);
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- signals ---------------------------------------------------------- */

static void fatal_signal(int sig)
{
    video_cedar_emergency_off();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void *signal_thread(void *arg)
{
    const sigset_t *set = arg;
    int sig;

    for (;;) {
        if (sigwait(set, &sig) != 0)
            continue;
        switch (atomic_load(&g_phase)) {
        case PH_PRE:
            /* Nothing on screen yet. */
            _exit(128 + sig);
        case PH_CONNECTING:
            atomic_store(&g_stop_sig, sig);
            LiInterruptConnection();
            break;
        case PH_STREAMING:
            atomic_store(&g_stop_sig, sig);
            wake();
            break;
        default:
            /* A second signal while stopping: do not hang. */
            video_cedar_emergency_off();
            _exit(128 + sig);
        }
    }
    return NULL;
}

static int setup_signals(void)
{
    static sigset_t set;
    struct sigaction sa;
    pthread_t t;
    static const int fatal[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    unsigned i;

    signal(SIGPIPE, SIG_IGN);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fatal_signal;
    sa.sa_flags = SA_RESETHAND;
    for (i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++)
        sigaction(fatal[i], &sa, NULL);

    /* Every thread created later inherits the mask. */
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGQUIT);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0)
        return -1;
    if (pthread_create(&t, NULL, signal_thread, &set) != 0)
        return -1;
    pthread_detach(t);
    return 0;
}

/* ---- identity --------------------------------------------------------- */

static int mkdir_p(const char *path, mode_t mode)
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
    if (mkdir(buf, mode) != 0 && errno != EEXIST)
        return -1;
    return chmod(buf, mode);
}

static int copy_file(const char *src, const char *dst)
{
    char tmp[PATH_MAX + 8], buf[4096];
    int in, out, ok = 0;
    ssize_t n;

    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) {
        close(in);
        return -1;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            n = -1;
            break;
        }
    }
    ok = n == 0 && fsync(out) == 0;
    close(in);
    if (close(out) != 0)
        ok = 0;
    if (!ok || rename(tmp, dst) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/*
 * Copies the stock client's pairing identity so the host needs no new
 * pairing. uniqueid.dat goes last: its presence marks a complete copy.
 * Nothing from these files is ever printed.
 */
static void import_identity(void)
{
    static const char *const files[] = { "client.pem", "key.pem",
                                         "client.p12", "uniqueid.dat" };
    char src[PATH_MAX], dst[PATH_MAX];
    unsigned i;

    if (mkdir_p(O.keydir, 0700) != 0) {
        fprintf(stderr, "keydir %s: %s\n", O.keydir, strerror(errno));
        return;
    }
    snprintf(dst, sizeof(dst), "%s/uniqueid.dat", O.keydir);
    if (O.no_import || access(dst, F_OK) == 0)
        return;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(src, sizeof(src), "%s/%s", LEGACY_KEYDIR, files[i]);
        if (access(src, R_OK) != 0)
            return;     /* gs_init creates a fresh identity */
    }
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(src, sizeof(src), "%s/%s", LEGACY_KEYDIR, files[i]);
        snprintf(dst, sizeof(dst), "%s/%s", O.keydir, files[i]);
        if (copy_file(src, dst) != 0) {
            fprintf(stderr, "identity: copy of %s failed: %s\n", files[i],
                    strerror(errno));
            return;
        }
    }
    printf("identity: copied from %s\n", LEGACY_KEYDIR);
}

/* ---- host commands ---------------------------------------------------- */

static int connect_host(PSERVER_DATA srv)
{
    static char addr[256];
    int rc;

    snprintf(addr, sizeof(addr), "%s", O.host);
    memset(srv, 0, sizeof(*srv));
    import_identity();
    rc = gs_init(srv, addr, (unsigned short)O.port, O.keydir, O.verbose,
                 true);
    if (rc == GS_OK)
        return 0;
    if (rc == GS_OUT_OF_MEMORY)
        fprintf(stderr, "host %s: out of memory\n", O.host);
    else if (rc == GS_ERROR || rc == GS_INVALID || rc == GS_UNSUPPORTED_VERSION)
        fprintf(stderr, "host %s: %s\n", O.host, gs_error ? gs_error : "error");
    else
        fprintf(stderr, "host %s: unreachable (%d)\n", O.host, rc);
    ipc_send("error unreachable");
    return -1;
}

static int require_paired(PSERVER_DATA srv)
{
    if (srv->paired)
        return 0;
    fprintf(stderr, "host %s: not paired\n", O.host);
    ipc_send("error not-paired");
    return -1;
}

static void free_apps(PAPP_LIST l)
{
    while (l != NULL) {
        PAPP_LIST next = l->next;

        free(l->name);
        free(l);
        l = next;
    }
}

static int cmd_info(PSERVER_DATA srv)
{
    int scm = srv->serverInfo.serverCodecModeSupport;
    PDISPLAY_MODE m;

    printf("host: %s http %u https %u\n", O.host, srv->httpPort,
           srv->httpsPort);
    printf("paired: %s\n", srv->paired ? "yes" : "no");
    printf("running: %d\n", srv->currentGame);
    printf("gpu: %s\n", srv->gpuType ? srv->gpuType : "?");
    printf("version: app %s, gfe %s\n",
           srv->serverInfo.serverInfoAppVersion ?
               srv->serverInfo.serverInfoAppVersion : "?",
           srv->serverInfo.serverInfoGfeVersion ?
               srv->serverInfo.serverInfoGfeVersion : "?");
    printf("codecs: 0x%x%s%s%s\n", scm, (scm & SCM_H264) ? " h264" : "",
           (scm & SCM_HEVC) ? " hevc" : "",
           (scm & SCM_MASK_AV1) ? " av1" : "");
    printf("modes:");
    for (m = srv->modes; m != NULL; m = m->next)
        printf(" %ux%ux%u", m->width, m->height, m->refresh);
    printf("\n");
    ipc_send("host %d %d %d", srv->paired ? 1 : 0, srv->currentGame,
             (scm & SCM_HEVC) ? 1 : 0);
    return 0;
}

static int cmd_list(PSERVER_DATA srv)
{
    PAPP_LIST apps = NULL, a;
    int rc;

    if (require_paired(srv) != 0)
        return 1;
    rc = gs_applist(srv, &apps);
    if (rc != GS_OK) {
        fprintf(stderr, "applist: %s\n", gs_error ? gs_error : "failed");
        return 1;
    }
    for (a = apps; a != NULL; a = a->next) {
        printf("%d\t%s%s\n", a->id, a->name,
               a->id == srv->currentGame ? "\t(running)" : "");
        ipc_send("app %d %d %s", a->id, a->id == srv->currentGame, a->name);
    }
    free_apps(apps);
    return 0;
}

static int cmd_pair(PSERVER_DATA srv, const char *pin_arg)
{
    char pin[5];
    unsigned char r[4];
    int fd, i;

    if (srv->paired) {
        printf("already paired\n");
        return 0;
    }
    if (pin_arg != NULL && strlen(pin_arg) == 4 &&
        strspn(pin_arg, "0123456789") == 4) {
        memcpy(pin, pin_arg, 5);
    } else {
        fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0 || read(fd, r, sizeof(r)) != (ssize_t)sizeof(r)) {
            perror("urandom");
            return 1;
        }
        close(fd);
        for (i = 0; i < 4; i++)
            pin[i] = (char)('0' + r[i] % 10);
        pin[4] = '\0';
    }
    printf("PIN: %s (enter it on the host)\n", pin);
    fflush(stdout);
    ipc_send("pin %s", pin);
    if (gs_pair(srv, pin) != GS_OK) {
        fprintf(stderr, "pairing failed: %s\n", gs_error ? gs_error : "?");
        ipc_send("error pair-failed");
        return 1;
    }
    printf("paired\n");
    ipc_send("paired");
    return 0;
}

static int resolve_app(PSERVER_DATA srv, const char *app)
{
    PAPP_LIST apps = NULL, a;
    int id = -1;

    if (app[0] != '\0' && strspn(app, "0123456789") == strlen(app))
        return atoi(app);
    if (gs_applist(srv, &apps) != GS_OK) {
        fprintf(stderr, "applist: %s\n", gs_error ? gs_error : "failed");
        return -1;
    }
    for (a = apps; a != NULL; a = a->next)
        if (strcasecmp(a->name, app) == 0)
            id = a->id;
    free_apps(apps);
    if (id < 0)
        fprintf(stderr, "no app named \"%s\" (see list)\n", app);
    return id;
}

/* ---- streaming -------------------------------------------------------- */

static void cl_stage_starting(int stage)
{
    if (O.verbose)
        printf("stage: %s\n", LiGetStageName(stage));
    ipc_send("stage %s", LiGetStageName(stage));
}

static void cl_stage_failed(int stage, int err)
{
    fprintf(stderr, "stage %s failed: %d\n", LiGetStageName(stage), err);
    ipc_send("error stage-failed %s %d", LiGetStageName(stage), err);
}

static void cl_started(void)
{
    printf("connected\n");
    fflush(stdout);
    ipc_send("started");
}

static void cl_terminated(int err)
{
    atomic_store(&g_term_code, err);
    atomic_store(&g_terminated, 1);
    wake();
}

static void cl_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void cl_status(int status)
{
    printf("network: %s\n", status == CONN_STATUS_POOR ? "poor" : "okay");
    ipc_send("network %s", status == CONN_STATUS_POOR ? "poor" : "okay");
}

static CONNECTION_LISTENER_CALLBACKS listener = {
    .stageStarting = cl_stage_starting,
    .stageFailed = cl_stage_failed,
    .connectionStarted = cl_started,
    .connectionTerminated = cl_terminated,
    .logMessage = cl_log,
    .connectionStatusUpdate = cl_status,
};

/* Shown to the user in the UI's "Disconnected" dialog. */
static const char *term_reason(int code)
{
    switch (code) {
    case ML_ERROR_GRACEFUL_TERMINATION: return "The PC ended the stream.";
    case ML_ERROR_NO_VIDEO_TRAFFIC:
        return "No video came from the PC. Make sure that its firewall does "
               "not block the stream.";
    case ML_ERROR_NO_VIDEO_FRAME:
        return "The network is too slow for this bitrate. Set a lower "
               "bitrate in Settings.";
    case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
        return "Video capture on the PC stopped with an error.";
    case ML_ERROR_PROTECTED_CONTENT:
        return "The PC cannot capture protected content.";
    case ML_ERROR_FRAME_CONVERSION:
        return "Frame conversion on the PC stopped with an error.";
    default: return "This device lost the connection to the PC.";
    }
}

/*
 * Per-thread CPU share since each thread started, printed before the
 * stream threads are joined (names come from moonlight-common-c).
 */
static void print_thread_cpu(double secs)
{
    char path[300], buf[512], line[1024];
    long hz = sysconf(_SC_CLK_TCK);
    size_t used = 0;
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;

    if (d == NULL || secs <= 0 || hz <= 0) {
        if (d != NULL)
            closedir(d);
        return;
    }
    line[0] = 0;
    while ((e = readdir(d)) != NULL) {
        unsigned long ut, st;
        char *name, *end;
        FILE *f;

        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
        f = fopen(path, "re");
        if (f == NULL)
            continue;
        if (fgets(buf, sizeof(buf), f) == NULL) {
            fclose(f);
            continue;
        }
        fclose(f);
        name = strchr(buf, '(');
        end = strrchr(buf, ')');
        if (name == NULL || end == NULL ||
            sscanf(end + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                            "%lu %lu", &ut, &st) != 2)
            continue;
        *end = 0;
        if (ut + st == 0)
            continue;
        used += (size_t)snprintf(line + used, sizeof(line) - used,
                                 " %s=%.1f%%", name + 1,
                                 (double)(ut + st) * 100.0 / (double)hz /
                                 secs);
        if (used >= sizeof(line))
            break;
    }
    closedir(d);
    printf("threads:%s\n", line);
}

static double cpu_seconds(void)
{
    char buf[512], *p;
    unsigned long ut = 0, st = 0;
    int fd, i;
    ssize_t n;

    fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    p = strrchr(buf, ')');
    /* Fields after the name: state is 3; utime 14 and stime 15. */
    for (i = 2; p != NULL && i < 14; i++)
        p = strchr(p + 1, ' ');
    if (p == NULL || sscanf(p, " %lu %lu", &ut, &st) != 2)
        return 0;
    return (double)(ut + st) / (double)sysconf(_SC_CLK_TCK);
}

struct totals {
    unsigned long frames, shown, errors, need_idr, skipped, recreated;
    unsigned long hidden, gaps50, gaps100;
    double gap_max_ms;
    unsigned long audio_lost, audio_dropped, underruns;
    double total_ms_sum, total_max_ms;
    uint64_t bytes;
};

static void report(struct totals *t, double secs, double cpu, int elapsed_s)
{
    struct video_stats v;
    struct audio_stats a;
    uint32_t rtt = 0, rttv = 0;

    video_cedar_take_stats(&v);
    audio_alsa_take_stats(&a);
    LiGetEstimatedRttInfo(&rtt, &rttv);
    t->frames += v.frames;
    t->shown += v.shown;
    t->errors += v.errors;
    t->need_idr += v.need_idr;
    t->skipped += v.skipped;
    t->recreated += v.recreated;
    t->hidden += v.hidden;
    t->gaps50 += v.gaps50;
    t->gaps100 += v.gaps100;
    if (v.gap_max_ms > t->gap_max_ms)
        t->gap_max_ms = v.gap_max_ms;
    t->audio_lost += a.lost;
    t->audio_dropped += a.dropped;
    t->underruns += a.underruns;
    t->total_ms_sum += v.total_ms * (double)v.shown;
    if (v.total_max_ms > t->total_max_ms)
        t->total_max_ms = v.total_max_ms;
    t->bytes += v.bytes;
    if (secs <= 0)
        secs = 1;
    fdguard_check();

    if (O.stats)
        printf("stats t=%d fps=%.0f/%.0f skip=%lu late=%lu err=%lu idr=%lu "
               "gap=%.0f/%lu/%lu lat=%.1f max=%.1f asm=%.1f q=%.2f dec=%.2f rot=%.2f "
               "commit=%.2f host=%.1f rtt=%u/%u kbps=%.0f vq=%d "
               "audio=%lu lost=%lu drop=%lu xrun=%lu adelay=%.1f "
               "cpu=%.1f%%\n",
               elapsed_s, v.frames / secs, v.shown / secs, v.skipped, v.late,
               v.errors, v.idr, v.gap_max_ms, v.gaps50, v.gaps100, v.total_ms, v.total_max_ms, v.assemble_ms,
               v.queue_ms, v.decode_ms, v.rotate_ms, v.commit_ms, v.host_ms,
               rtt, rttv, (double)v.bytes * 8.0 / 1000.0 / secs,
               LiGetPendingVideoFrames(), a.packets, a.lost, a.dropped,
               a.underruns, a.delay_ms, cpu * 100.0 / secs);
    fflush(stdout);
    ipc_send("stats fps=%.0f lat=%.1f dec=%.2f rtt=%u kbps=%.0f "
             "lost=%lu drop=%lu cpu=%.1f",
             v.shown / secs, v.total_ms, v.decode_ms, rtt,
             (double)v.bytes * 8.0 / 1000.0 / secs, a.lost, a.dropped,
             cpu * 100.0 / secs);
    ipc_send("hb %lu", t->shown);
}

static void set_paused(int paused)
{
    video_cedar_set_visible(!paused);
    input_set_forwarding(!paused);
    if (!paused)
        LiRequestIdrFrame();    /* repaint a static host screen at once */
}

enum stop_reason { STOP_NONE, STOP_SIGNAL, STOP_TERMINATED, STOP_COMBO,
                   STOP_UI, STOP_DURATION, STOP_IPC_LOST };

/* Reads UI commands; sets *quit_app for "quit-app". */
static enum stop_reason ipc_poll(char *buf, size_t *len, int *quit_app)
{
    ssize_t n = read(O.ipc_fd, buf + *len, 255 - *len);
    char *line, *nl;

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
        return STOP_IPC_LOST;
    if (n < 0)
        return STOP_NONE;
    *len += (size_t)n;
    buf[*len] = '\0';
    line = buf;
    while ((nl = strchr(line, '\n')) != NULL) {
        *nl = '\0';
        if (strcmp(line, "pause") == 0) {
            set_paused(1);
        } else if (strcmp(line, "resume") == 0) {
            set_paused(0);
        } else if (strcmp(line, "guide") == 0) {
            input_send_guide();
        } else if (strcmp(line, "mouse on") == 0) {
            input_set_mouse(1);
        } else if (strcmp(line, "mouse off") == 0) {
            input_set_mouse(0);
        } else if (strcmp(line, "stats on") == 0) {
            O.stats = 1;
        } else if (strcmp(line, "stats off") == 0) {
            O.stats = 0;
        } else if (strcmp(line, "stop") == 0) {
            return STOP_UI;
        } else if (strcmp(line, "quit-app") == 0) {
            *quit_app = 1;
            return STOP_UI;
        }
        line = nl + 1;
    }
    *len = strlen(line);
    memmove(buf, line, *len + 1);
    if (*len >= 255)
        *len = 0;
    return STOP_NONE;
}

static int cmd_stream(PSERVER_DATA srv, const char *app)
{
    STREAM_CONFIGURATION sc;
    struct input_options io = { .swap_abxy = O.swap_abxy,
                                .mouse = O.mouse };
    struct itimerspec its = { { 1, 0 }, { 1, 0 } };
    struct totals tot;
    enum stop_reason why = STOP_NONE;
    char ipcbuf[256];
    size_t ipclen = 0;
    int app_id, rc, tfd, paused = 0, quit_app = O.quit_after, elapsed = 0;
    int nval_logged = 0, late_logged = 0, wake_fd = g_evpipe[0];
    uint64_t t_start, t_last;
    double cpu_last, cpu0;

    if (require_paired(srv) != 0)
        return 1;
    app_id = resolve_app(srv, app);
    if (app_id < 0)
        return 1;
    if (srv->currentGame != 0 && srv->currentGame != app_id) {
        fprintf(stderr, "another app (%d) is running on the host; quit it "
                        "first\n", srv->currentGame);
        ipc_send("error other-app-running %d", srv->currentGame);
        return 1;
    }

    LiInitializeStreamConfiguration(&sc);
    sc.width = O.width;
    sc.height = O.height;
    sc.fps = O.fps;
    sc.bitrate = O.bitrate;
    sc.packetSize = 1392;
    sc.streamingRemotely = STREAM_CFG_AUTO;
    sc.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    if (strcmp(O.codec, "h264") == 0)
        sc.supportedVideoFormats = VIDEO_FORMAT_H264;
    else if (strcmp(O.codec, "h265") == 0 || strcmp(O.codec, "hevc") == 0)
        sc.supportedVideoFormats = VIDEO_FORMAT_H265;
    else
        sc.supportedVideoFormats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265;
    sc.colorSpace = COLORSPACE_REC_601;
    sc.colorRange = COLOR_RANGE_LIMITED;
    sc.encryptionFlags = ENCFLG_ALL;
    audio_alsa_set_latency(O.audio_latency);
    if (O.audio_device != NULL)
        audio_alsa_set_device(O.audio_device);

    printf("stream: app %d, %dx%d@%d, %d kbps, codec %s\n", app_id, sc.width,
           sc.height, sc.fps, sc.bitrate, O.codec);
    printf("fdguard: %s\n", g_fdguard ? "interposed" : "NOT interposed");
    fflush(stdout);
    rc = gs_start_app(srv, &sc, app_id, O.sops != 0, false, 0x1);
    if (rc != GS_OK) {
        if (rc == GS_NOT_SUPPORTED_MODE)
            fprintf(stderr, "host rejects %dx%d@%d\n", sc.width, sc.height,
                    sc.fps);
        else
            fprintf(stderr, "launch failed (%d): %s\n", rc,
                    gs_error ? gs_error : "?");
        ipc_send("error launch-failed %d", rc);
        return 1;
    }

    if (O.verbose)
        video_cedar_set_trace(40);
    if (!O.keep_wifi_ps && wifi_ps_suspend() < 0)
        fprintf(stderr, "wifi: could not turn power save off\n");
    atomic_store(&g_phase, PH_CONNECTING);
    rc = LiStartConnection(&srv->serverInfo, &sc, &listener,
                           &video_cedar_callbacks, &audio_alsa_callbacks,
                           NULL, 0, NULL, 0);
    if (rc != 0) {
        atomic_store(&g_phase, PH_STOPPING);
        fprintf(stderr, "connection failed (%d)%s\n", rc,
                atomic_load(&g_stop_sig) ? ": interrupted" : "");
        ipc_send("error connect-failed %d", rc);
        wifi_ps_restore();
        if (quit_app)
            gs_quit_app(srv);
        return 1;
    }
    atomic_store(&g_phase, PH_STREAMING);
    if (atomic_load(&g_stop_sig))
        why = STOP_SIGNAL;

    if (!O.no_input && input_start(&io, g_evpipe[1]) != 0)
        fprintf(stderr, "input: start failed\n");

    memset(&tot, 0, sizeof(tot));
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tfd < 0 || timerfd_settime(tfd, 0, &its, NULL) != 0)
        fprintf(stderr, "timer: %s\n", strerror(errno));
    fdguard_add(tfd, "timer");
    t_start = t_last = now_ns();
    cpu0 = cpu_last = cpu_seconds();

    while (why == STOP_NONE) {
        struct pollfd p[3];
        int np = 2, tick;
        unsigned ev;
        char drain[64];
        uint64_t t;

        memset(p, 0, sizeof(p));
        p[0].fd = wake_fd;           /* poll skips a negative fd */
        p[0].events = POLLIN;
        p[1].fd = tfd;
        p[1].events = POLLIN;
        if (O.ipc_fd >= 0) {
            p[2].fd = O.ipc_fd;
            p[2].events = POLLIN;
            np = 3;
        }
        /* The timeout keeps the heartbeat going if the timer fails. */
        if (poll(p, (nfds_t)np, tfd >= 0 ? 1000 : 500) < 0 && errno != EINTR)
            break;
        if (!nval_logged && ((p[0].revents | p[1].revents |
                              (np == 3 ? p[2].revents : 0)) & POLLNVAL)) {
            fprintf(stderr, "main loop: poll reports a closed fd "
                            "(%d:%x %d:%x %d:%x)\n", p[0].fd, p[0].revents,
                    p[1].fd, p[1].revents, np == 3 ? p[2].fd : -1,
                    np == 3 ? p[2].revents : 0);
            fdguard_check();
            nval_logged = 1;
        }
        /* A guarded fd that was closed or replaced behind fdguard's back:
         * stop using it, rather than spin on POLLNVAL or read another
         * component's data. */
        if (tfd >= 0 && (p[1].revents & (POLLIN | POLLNVAL)) &&
            !fdguard_ok(tfd)) {
            fprintf(stderr, "timer: fd %d lost; ticking from the poll "
                            "timeout\n", tfd);
            fdguard_check();
            fdguard_forget(tfd);
            tfd = -1;
            p[1].revents = 0;
        }
        if (wake_fd >= 0 && (p[0].revents & (POLLIN | POLLNVAL)) &&
            !fdguard_ok(wake_fd)) {
            fprintf(stderr, "wake pipe: fd %d lost\n", wake_fd);
            fdguard_check();
            fdguard_forget(wake_fd);
            wake_fd = -1;
            p[0].revents = 0;
        }
        if (np == 3 && (p[2].revents & (POLLIN | POLLHUP | POLLERR |
                                        POLLNVAL)) &&
            !fdguard_ok(O.ipc_fd)) {
            fprintf(stderr, "ipc: socket fd %d lost\n", O.ipc_fd);
            fdguard_check();
            fdguard_forget(O.ipc_fd);
            pthread_mutex_lock(&g_ipc_lock);
            O.ipc_fd = -1;
            pthread_mutex_unlock(&g_ipc_lock);
            why = STOP_IPC_LOST;
            break;
        }
        if (p[0].revents & POLLIN)
            while (read(wake_fd, drain, sizeof(drain)) > 0)
                ;
        if (atomic_load(&g_stop_sig)) {
            why = STOP_SIGNAL;
            break;
        }
        if (atomic_load(&g_terminated)) {
            why = STOP_TERMINATED;
            break;
        }
        ev = input_take_events();
        if (ev & INPUT_EV_EXIT) {
            why = STOP_COMBO;
            break;
        }
        if (ev & INPUT_EV_MENU) {
            if (O.ipc_fd >= 0) {
                paused = 1;
                set_paused(1);
                ipc_send("menu");
            } else {
                /* CLI: MENU toggles a pause (layer hidden, pad released). */
                paused = !paused;
                set_paused(paused);
                printf("menu: %s\n", paused ? "paused" : "resumed");
            }
        }
        if (np == 3 && (p[2].revents & (POLLIN | POLLHUP | POLLERR |
                                        POLLNVAL))) {
            why = ipc_poll(ipcbuf, &ipclen, &quit_app);
            if (why != STOP_NONE)
                break;
        }
        t = now_ns();
        tick = (p[1].revents & POLLIN) != 0;
        if (!tick && t - t_last >= 1500000000ull && !late_logged) {
            fprintf(stderr, "timer: no tick for %.1f s\n",
                    (double)(t - t_last) / 1e9);
            fdguard_check();
            late_logged = 1;
        }
        if (tick || t - t_last >= 1500000000ull) {
            uint64_t expirations;
            double c = cpu_seconds();

            if (tick && read(tfd, &expirations, sizeof(expirations)) < 0)
                expirations = 0;
            elapsed = (int)((t - t_start) / 1000000000ull);
            report(&tot, (double)(t - t_last) / 1e9, c - cpu_last, elapsed);
            t_last = t;
            cpu_last = c;
            if (O.duration > 0 && elapsed >= O.duration)
                why = STOP_DURATION;
        }
    }

    atomic_store(&g_phase, PH_STOPPING);
    switch (why) {
    case STOP_TERMINATED:
        rc = atomic_load(&g_term_code);
        printf("stopped: error %d: %s\n", rc, term_reason(rc));
        ipc_send("term %d %s", rc, term_reason(rc));
        break;
    case STOP_COMBO: printf("stopped: exit combo\n"); break;
    case STOP_SIGNAL: printf("stopped: signal %d\n",
                             atomic_load(&g_stop_sig)); break;
    case STOP_UI: printf("stopped: by UI\n"); break;
    case STOP_DURATION: printf("stopped: duration reached\n"); break;
    case STOP_IPC_LOST: printf("stopped: UI went away\n"); break;
    default: break;
    }
    fflush(stdout);
    if (!O.no_input) {
        input_set_forwarding(0);
        input_stop();
    }
    {
        double secs = (double)(now_ns() - t_start) / 1e9, dec, alsa;

        print_thread_cpu(secs);
        audio_alsa_cpu(&dec, &alsa);
        printf("audio cpu: opus %.1f%%, alsa %.1f%% of one core\n",
               secs > 0 ? dec * 100.0 / secs : 0,
               secs > 0 ? alsa * 100.0 / secs : 0);
        fflush(stdout);
    }
    LiStopConnection();
    wifi_ps_restore();
    if (tfd >= 0)
        fdguard_close(tfd);
    {
        double secs = (double)(now_ns() - t_start) / 1e9;
        double cpu = cpu_seconds() - cpu0;

        printf("summary: %.0f s, %lu frames, %lu shown (%.1f fps), "
               "lat mean %.1f max %.1f ms, %lu skipped, %lu errors, "
               "%lu idr requests, %lu rebuilds, %.0f kbps, audio %lu lost "
               "%lu dropped %lu xruns, cpu %.1f%% of one core\n",
               secs, tot.frames, tot.shown, secs > 0 ? tot.shown / secs : 0,
               tot.shown ? tot.total_ms_sum / (double)tot.shown : 0,
               tot.total_max_ms, tot.skipped, tot.errors, tot.need_idr,
               tot.recreated,
               secs > 0 ? (double)tot.bytes * 8.0 / 1000.0 / secs : 0,
               tot.audio_lost, tot.audio_dropped, tot.underruns,
               secs > 0 ? cpu * 100.0 / secs : 0);
        printf("gaps: max %.0f ms, %lu over 50 ms, %lu over 100 ms; "
               "%lu frames decoded while hidden\n", tot.gap_max_ms,
               tot.gaps50, tot.gaps100, tot.hidden);
        {
            struct cedar_sync_stats ss;

            cedar_sync_stats(&ss);
            printf("semwait: total calls=%lu blocked=%lu timeouts=%lu\n",
                   ss.calls, ss.blocked, ss.timeouts);
        }
        printf("fdguard: %lu stray close() calls refused\n",
               fdguard_refused());
    }
    if (quit_app) {
        printf("quitting app on host\n");
        if (gs_quit_app(srv) != GS_OK)
            fprintf(stderr, "quit: %s\n", gs_error ? gs_error : "failed");
    }
    fflush(stdout);
    ipc_send("ended");
    return why == STOP_TERMINATED && rc != ML_ERROR_GRACEFUL_TERMINATION ?
           2 : 0;
}

/* ---- main ------------------------------------------------------------- */

static void usage(FILE *f)
{
    fprintf(f,
"moonlight-tsp-stream " ENGINE_VERSION "\n"
"usage: moonlight-tsp-stream [options] COMMAND [ARG]\n"
"\n"
"commands:\n"
"  info                 host status, codecs and modes\n"
"  list                 apps on the host (id, name)\n"
"  pair [PIN]           pair; enter the printed PIN on the host\n"
"  unpair\n"
"  stream APP           stream an app (name or id)\n"
"  quit                 quit the app running on the host\n"
"  reset-display        remove a leftover video layer and restore Wi-Fi\n"
"                       power save after a killed engine\n"
"  input-test           print pad events (Ctrl-C ends)\n"
"  update-check         ask GitHub for a newer release\n"
"  update-fetch VER     download release VER into the stage folder\n"
"\n"
"options:\n"
"  --host ADDR          host address (all host commands)\n"
"  --port N             host HTTP port (47989)\n"
"  --keydir DIR         pairing identity (" DEFAULT_KEYDIR ")\n"
"  --no-import          do not copy the stock client's identity\n"
"  --width N --height N --fps N   (1280x720@60)\n"
"  --bitrate KBPS       (10000)\n"
"  --codec auto|h264|h265\n"
"  --audio-latency MS   (20)\n"
"  --audio-device NAME  ALSA PCM instead of the low-latency path\n"
"  --swap-abxy          exchange A/B and X/Y\n"
"  --mouse              start in mouse mode (the pad drives the mouse)\n"
"  --no-input           view only\n"
"  --sops               let the host optimise game settings\n"
"  --quit-after         quit the host app when the stream ends\n"
"  --keep-wifi-ps       leave Wi-Fi power save on during the stream\n"
"  --duration SEC       end the stream after SEC seconds\n"
"  --stats              print statistics every second\n"
"  --ipc-fd N           UI control socket\n"
"  --update-url URL     releases page (" DEFAULT_UPDATE_URL ")\n"
"  --ca-file FILE       root certificates (" DEFAULT_CA_FILE ")\n"
"  --stage DIR          update stage folder, named " UPDATE_STAGE_NAME "\n"
"                       (" DEFAULT_STAGE ")\n"
"  --unzip PROGRAM      (" DEFAULT_UNZIP ")\n"
"  --version            print the version\n"
"  -v                   verbose\n");
}

int main(int argc, char **argv)
{
    static const struct option lopts[] = {
        { "host", required_argument, NULL, 'H' },
        { "port", required_argument, NULL, 'P' },
        { "keydir", required_argument, NULL, 'k' },
        { "no-import", no_argument, NULL, 'N' },
        { "width", required_argument, NULL, 'w' },
        { "height", required_argument, NULL, 'h' },
        { "fps", required_argument, NULL, 'f' },
        { "bitrate", required_argument, NULL, 'b' },
        { "codec", required_argument, NULL, 'c' },
        { "audio-latency", required_argument, NULL, 'l' },
        { "audio-device", required_argument, NULL, 'a' },
        { "swap-abxy", no_argument, NULL, 's' },
        { "mouse", no_argument, NULL, 'M' },
        { "no-input", no_argument, NULL, 'n' },
        { "sops", no_argument, NULL, 'S' },
        { "quit-after", no_argument, NULL, 'q' },
        { "duration", required_argument, NULL, 'd' },
        { "stats", no_argument, NULL, 't' },
        { "ipc-fd", required_argument, NULL, 'I' },
        { "reset-display", no_argument, NULL, 'R' },
        { "keep-wifi-ps", no_argument, NULL, 'W' },
        { "help", no_argument, NULL, 'Z' },
        { "update-url", required_argument, NULL, 'U' },
        { "ca-file", required_argument, NULL, 'C' },
        { "stage", required_argument, NULL, 'G' },
        { "unzip", required_argument, NULL, 'X' },
        { "version", no_argument, NULL, 'V' },
        { NULL, 0, NULL, 0 },
    };
    SERVER_DATA srv;
    const char *cmd, *arg;
    int c, rc, is_update;

    setvbuf(stdout, NULL, _IOLBF, 0);
    while ((c = getopt_long(argc, argv, "v", lopts, NULL)) != -1) {
        switch (c) {
        case 'H': O.host = optarg; break;
        case 'P': O.port = (unsigned)atoi(optarg); break;
        case 'k': O.keydir = optarg; break;
        case 'N': O.no_import = 1; break;
        case 'w': O.width = atoi(optarg); break;
        case 'h': O.height = atoi(optarg); break;
        case 'f': O.fps = atoi(optarg); break;
        case 'b': O.bitrate = atoi(optarg); break;
        case 'c': O.codec = optarg; break;
        case 'l': O.audio_latency = atoi(optarg); break;
        case 'a': O.audio_device = optarg; break;
        case 's': O.swap_abxy = 1; break;
        case 'M': O.mouse = 1; break;
        case 'n': O.no_input = 1; break;
        case 'S': O.sops = 1; break;
        case 'q': O.quit_after = 1; break;
        case 'd': O.duration = atoi(optarg); break;
        case 't': O.stats = 1; break;
        case 'I': O.ipc_fd = atoi(optarg); break;
        case 'W': O.keep_wifi_ps = 1; break;
        case 'R':
            wifi_ps_restore();
            return video_cedar_reset_display() == 0 ? 0 : 1;
        case 'v': O.verbose++; break;
        case 'Z': usage(stdout); return 0;
        case 'U': O.update_url = optarg; break;
        case 'C': O.ca_file = optarg; break;
        case 'G': O.stage = optarg; break;
        case 'X': O.unzip = optarg; break;
        case 'V': printf("moonlight-tsp-stream " ENGINE_VERSION "\n"); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind >= argc) {
        usage(stderr);
        return 2;
    }
    cmd = argv[optind];
    arg = optind + 1 < argc ? argv[optind + 1] : NULL;

    if (strcmp(cmd, "reset-display") == 0) {
        wifi_ps_restore();
        return video_cedar_reset_display() == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "input-test") == 0)
        return input_dump();
    is_update = strcmp(cmd, "update-check") == 0 ||
                strcmp(cmd, "update-fetch") == 0;
    if (strcmp(cmd, "info") != 0 && strcmp(cmd, "list") != 0 &&
        strcmp(cmd, "pair") != 0 && strcmp(cmd, "unpair") != 0 &&
        strcmp(cmd, "stream") != 0 && strcmp(cmd, "quit") != 0 &&
        !is_update) {
        fprintf(stderr, "unknown command %s\n", cmd);
        usage(stderr);
        return 2;
    }
    if (O.host == NULL && !is_update) {
        fprintf(stderr, "%s needs --host\n", cmd);
        return 2;
    }
    if (strcmp(cmd, "stream") == 0 && arg == NULL) {
        fprintf(stderr, "stream needs an app name or id\n");
        return 2;
    }
    if (strcmp(cmd, "update-fetch") == 0 && arg == NULL) {
        fprintf(stderr, "update-fetch needs a version\n");
        return 2;
    }
    if (O.ipc_fd >= 0) {
        if (fcntl(O.ipc_fd, F_SETFL,
                  fcntl(O.ipc_fd, F_GETFL) | O_NONBLOCK) != 0) {
            perror("ipc-fd");
            return 2;
        }
        fcntl(O.ipc_fd, F_SETFD, FD_CLOEXEC);
        /* The engine must not outlive the UI that drives it. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() == 1)
            return 1;
    }
    if (pipe2(g_evpipe, O_CLOEXEC | O_NONBLOCK) != 0 ||
        setup_signals() != 0) {
        perror("setup");
        return 1;
    }
    g_fdguard = fdguard_init();
    fdguard_add(STDOUT_FILENO, "stdout");
    fdguard_add(STDERR_FILENO, "stderr");
    fdguard_add(O.ipc_fd, "ipc socket");
    fdguard_add(g_evpipe[0], "wake pipe");
    fdguard_add(g_evpipe[1], "wake pipe");

    if (is_update) {
        struct update_opts uo = {
            .base_url = O.update_url,
            .ca_file = O.ca_file,
            .stage = O.stage,
            .unzip = O.unzip,
            .version = ENGINE_VERSION,
            .send = ipc_send,
        };

        printf("moonlight-tsp-stream " ENGINE_VERSION " %s%s%s\n", cmd,
               arg != NULL ? " " : "", arg != NULL ? arg : "");
        return strcmp(cmd, "update-check") == 0 ? update_check(&uo) :
               update_fetch(&uo, arg);
    }
    if (connect_host(&srv) != 0)
        return 1;
    if (strcmp(cmd, "info") == 0) {
        rc = cmd_info(&srv);
    } else if (strcmp(cmd, "list") == 0) {
        rc = cmd_list(&srv);
    } else if (strcmp(cmd, "pair") == 0) {
        rc = cmd_pair(&srv, arg);
    } else if (strcmp(cmd, "unpair") == 0) {
        rc = gs_unpair(&srv) == GS_OK ? 0 : 1;
        printf(rc == 0 ? "unpaired\n" : "unpair failed\n");
    } else if (strcmp(cmd, "quit") == 0) {
        rc = require_paired(&srv) != 0 || gs_quit_app(&srv) != GS_OK;
        printf(rc == 0 ? "quit sent\n" : "quit failed\n");
        ipc_send(rc == 0 ? "quit-done" : "error quit-failed");
    } else {
        rc = cmd_stream(&srv, arg);
    }
    return rc;
}
