#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "ae.h"
#include "night.h"
#include "isp.h"
#include "control.h"

#define CONTROL_PORT 8091

static volatile int    g_control_running;
static pthread_t       g_control_tid;
static pthread_t       g_log_tid;
static int             g_server_fd = -1;

/* Single-client model — this is a debug/tuning tool, not a multi-user
 * service. A new connection replaces any existing one. */
static pthread_mutex_t g_client_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_client_fd = -1;

static int             g_orig_stdout = -1; /* real stdout fd (serial console) */
static int             g_log_pipe_r  = -1;

/* isp.saturation/contrast/brightness/sharpness — the isp_set_*() functions
 * (isp.c) are one-way (GET-modify-SET against the hardware, no getter for
 * "what did we last ask for"), so the logical -50..50 value the user set is
 * tracked here, same ownership pattern as ae_tuning_t/night_tuning_t but
 * without a matching isp_tuning_t struct. 0 = neutral/unchanged from stock
 * default (matches isp_set_* treating the value as a delta off the
 * hardware/cached baseline captured at first call). */
static int g_isp_saturation = 0;
static int g_isp_contrast   = 0;
static int g_isp_brightness = 0;
static int g_isp_sharpness  = 0;

/* -------------------------------------------------------------------------
 * stdout tee — captures every printf in the whole binary (not just ae.c/
 * night.c) without touching a single existing call site. control_start()
 * redirects STDOUT_FILENO into a pipe; this thread reads it, writes the
 * exact same bytes back to the real stdout (serial console unaffected),
 * and additionally forwards complete lines to the connected control client
 * prefixed "LOG ".
 * ------------------------------------------------------------------------- */
static void *log_pump(void *arg)
{
    (void)arg;
    char   raw[512];
    char   line[1024];
    size_t line_len = 0;

    for (;;) {
        ssize_t n = read(g_log_pipe_r, raw, sizeof(raw));
        if (n <= 0) break;  /* pipe write end closed = process exiting */

        write(g_orig_stdout, raw, n);  /* unchanged passthrough to serial console */

        for (ssize_t i = 0; i < n; i++) {
            char c = raw[i];
            if (c == '\n') {
                line[line_len] = '\0';
                pthread_mutex_lock(&g_client_lock);
                if (g_client_fd >= 0)
                    dprintf(g_client_fd, "LOG %s\n", line);
                pthread_mutex_unlock(&g_client_lock);
                line_len = 0;
            } else if (line_len < sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Command protocol
 * ------------------------------------------------------------------------- */
/* Locked against g_client_lock — log_pump() also writes to this fd from a
 * different thread (async LOG lines); without a shared lock the two could
 * interleave mid-line on the socket. */
static void reply(int fd, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    if (len < sizeof(buf) - 1) { buf[len] = '\n'; len++; }
    pthread_mutex_lock(&g_client_lock);
    write(fd, buf, len);
    pthread_mutex_unlock(&g_client_lock);
}

static const char *night_mode_name(night_override_t m)
{
    switch (m) {
    case NIGHT_MODE_FORCE_DAY:   return "day";
    case NIGHT_MODE_FORCE_NIGHT: return "night";
    default:                     return "auto";
    }
}

static int night_mode_parse(const char *s, night_override_t *out)
{
    if (!strcmp(s, "auto"))  { *out = NIGHT_MODE_AUTO;        return 0; }
    if (!strcmp(s, "day"))   { *out = NIGHT_MODE_FORCE_DAY;   return 0; }
    if (!strcmp(s, "night")) { *out = NIGHT_MODE_FORCE_NIGHT; return 0; }
    return -1;
}

static void do_list(int fd)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);

    reply(fd, "ae.stable_range=%d",       a.stable_range);
    reply(fd, "ae.hold_range=%d",         a.hold_range);
    reply(fd, "ae.speed=%d",              a.speed);
    reply(fd, "ae.exp_max=%d",            a.exp_max);
    reply(fd, "ae.enabled=%d",            a.enabled);
    reply(fd, "night.mode=%s",            night_mode_name(n.override));
    reply(fd, "night.state=%s",           night_is_night() ? "night" : "day");
    reply(fd, "night.trigger_hw_exp=%u",  n.trigger_hw_exp);
    reply(fd, "night.day_hw_exp=%u",      n.day_hw_exp);
    reply(fd, "night.confirm_samples=%d", n.confirm_samples);
    reply(fd, "night.lock_ms=%d",         n.lock_ms);
    reply(fd, "isp.saturation=%d",        g_isp_saturation);
    reply(fd, "isp.contrast=%d",          g_isp_contrast);
    reply(fd, "isp.brightness=%d",        g_isp_brightness);
    reply(fd, "isp.sharpness=%d",         g_isp_sharpness);
    /* VENC QP is stock-matched ground truth but
     * not live-settable yet — no confirmed ioctl to update rc params on an
     * already-created encoder channel without a DEACTIVATE/CREATE_ENC
     * cycle. Reported for visibility only. */
    reply(fd, "venc.minqp=28 (read-only)");
    reply(fd, "venc.maxqp=43 (read-only)");
    reply(fd, ".");
}

static void do_get(int fd, const char *param)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);

    if      (!strcmp(param, "ae.stable_range"))       reply(fd, "ae.stable_range=%d", a.stable_range);
    else if (!strcmp(param, "ae.hold_range"))         reply(fd, "ae.hold_range=%d", a.hold_range);
    else if (!strcmp(param, "ae.speed"))              reply(fd, "ae.speed=%d", a.speed);
    else if (!strcmp(param, "ae.exp_max"))            reply(fd, "ae.exp_max=%d", a.exp_max);
    else if (!strcmp(param, "ae.enabled"))            reply(fd, "ae.enabled=%d", a.enabled);
    else if (!strcmp(param, "night.mode"))            reply(fd, "night.mode=%s", night_mode_name(n.override));
    else if (!strcmp(param, "night.state"))           reply(fd, "night.state=%s", night_is_night() ? "night" : "day");
    else if (!strcmp(param, "night.trigger_hw_exp"))  reply(fd, "night.trigger_hw_exp=%u", n.trigger_hw_exp);
    else if (!strcmp(param, "night.day_hw_exp"))      reply(fd, "night.day_hw_exp=%u", n.day_hw_exp);
    else if (!strcmp(param, "night.confirm_samples")) reply(fd, "night.confirm_samples=%d", n.confirm_samples);
    else if (!strcmp(param, "night.lock_ms"))         reply(fd, "night.lock_ms=%d", n.lock_ms);
    else if (!strcmp(param, "isp.saturation"))        reply(fd, "isp.saturation=%d", g_isp_saturation);
    else if (!strcmp(param, "isp.contrast"))          reply(fd, "isp.contrast=%d", g_isp_contrast);
    else if (!strcmp(param, "isp.brightness"))        reply(fd, "isp.brightness=%d", g_isp_brightness);
    else if (!strcmp(param, "isp.sharpness"))         reply(fd, "isp.sharpness=%d", g_isp_sharpness);
    else if (!strcmp(param, "venc.minqp"))            reply(fd, "venc.minqp=28 (read-only)");
    else if (!strcmp(param, "venc.maxqp"))            reply(fd, "venc.maxqp=43 (read-only)");
    else                                              reply(fd, "ERR unknown param %s", param);
}

static void do_set(int fd, const char *param, const char *value)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);

    if (!strcmp(param, "ae.stable_range")) {
        a.stable_range = atoi(value);
        ae_set_tuning(&a);
        reply(fd, "OK ae.stable_range=%d", a.stable_range);
    } else if (!strcmp(param, "ae.hold_range")) {
        a.hold_range = atoi(value);
        ae_set_tuning(&a);
        reply(fd, "OK ae.hold_range=%d", a.hold_range);
    } else if (!strcmp(param, "ae.speed")) {
        a.speed = atoi(value);
        ae_set_tuning(&a);
        reply(fd, "OK ae.speed=%d", a.speed);
    } else if (!strcmp(param, "ae.exp_max")) {
        a.exp_max = atoi(value);
        ae_set_tuning(&a);
        reply(fd, "OK ae.exp_max=%d", a.exp_max);
    } else if (!strcmp(param, "ae.enabled")) {
        a.enabled = atoi(value) ? 1 : 0;
        ae_set_tuning(&a);
        reply(fd, "OK ae.enabled=%d", a.enabled);
    } else if (!strcmp(param, "night.mode")) {
        night_override_t m;
        if (night_mode_parse(value, &m) != 0) {
            reply(fd, "ERR invalid night.mode value %s (want auto|day|night)", value);
            return;
        }
        n.override = m;
        night_set_tuning(&n);
        reply(fd, "OK night.mode=%s", night_mode_name(m));
    } else if (!strcmp(param, "night.trigger_hw_exp")) {
        n.trigger_hw_exp = (uint32_t)strtoul(value, NULL, 10);
        night_set_tuning(&n);
        reply(fd, "OK night.trigger_hw_exp=%u", n.trigger_hw_exp);
    } else if (!strcmp(param, "night.day_hw_exp")) {
        n.day_hw_exp = (uint32_t)strtoul(value, NULL, 10);
        night_set_tuning(&n);
        reply(fd, "OK night.day_hw_exp=%u", n.day_hw_exp);
    } else if (!strcmp(param, "night.confirm_samples")) {
        n.confirm_samples = atoi(value);
        night_set_tuning(&n);
        reply(fd, "OK night.confirm_samples=%d", n.confirm_samples);
    } else if (!strcmp(param, "night.lock_ms")) {
        n.lock_ms = atoi(value);
        night_set_tuning(&n);
        reply(fd, "OK night.lock_ms=%d", n.lock_ms);
    } else if (!strcmp(param, "isp.saturation")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(fd, "ERR isp.saturation range [-50,50]"); return; }
        if (isp_set_saturation(v) != 0) { reply(fd, "ERR isp_set_saturation failed"); return; }
        g_isp_saturation = v;
        reply(fd, "OK isp.saturation=%d", v);
    } else if (!strcmp(param, "isp.contrast")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(fd, "ERR isp.contrast range [-50,50]"); return; }
        if (isp_set_contrast(v) != 0) { reply(fd, "ERR isp_set_contrast failed"); return; }
        g_isp_contrast = v;
        reply(fd, "OK isp.contrast=%d", v);
    } else if (!strcmp(param, "isp.brightness")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(fd, "ERR isp.brightness range [-50,50]"); return; }
        if (isp_set_brightness(v) != 0) { reply(fd, "ERR isp_set_brightness failed"); return; }
        g_isp_brightness = v;
        reply(fd, "OK isp.brightness=%d", v);
    } else if (!strcmp(param, "isp.sharpness")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(fd, "ERR isp.sharpness range [-50,50]"); return; }
        if (isp_set_sharpness(v) != 0) { reply(fd, "ERR isp_set_sharpness failed"); return; }
        g_isp_sharpness = v;
        reply(fd, "OK isp.sharpness=%d", v);
    } else if (!strcmp(param, "venc.minqp") || !strcmp(param, "venc.maxqp")) {
        reply(fd, "ERR %s is read-only in this version "
                  "(requires an encoder restart, not yet implemented)", param);
    } else {
        reply(fd, "ERR unknown param %s", param);
    }
}

/* Blocking byte-at-a-time line reader — traffic on this protocol is tiny
 * (a human moving sliders), so simplicity wins over buffered-recv efficiency. */
static int read_line(int fd, char *buf, size_t cap)
{
    size_t i = 0;
    for (;;) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;  /* EOF or error */
        if (c == '\r') continue;
        if (c == '\n') { buf[i] = '\0'; return (int)i; }
        if (i < cap - 1) buf[i++] = c;
    }
}

static void handle_control_client(int fd)
{
    char line[256];
    for (;;) {
        int n = read_line(fd, line, sizeof(line));
        if (n < 0) return;
        if (n == 0) continue;

        char *cmd = strtok(line, " ");
        if (!cmd) continue;

        if (!strcasecmp(cmd, "LIST")) {
            do_list(fd);
        } else if (!strcasecmp(cmd, "GET")) {
            char *param = strtok(NULL, " ");
            if (!param) { reply(fd, "ERR GET requires a param name"); continue; }
            do_get(fd, param);
        } else if (!strcasecmp(cmd, "SET")) {
            char *param = strtok(NULL, " ");
            char *value = strtok(NULL, " ");
            if (!param || !value) { reply(fd, "ERR SET requires param and value"); continue; }
            do_set(fd, param, value);
        } else {
            reply(fd, "ERR unknown command %s", cmd);
        }
    }
}

static void *control_accept_loop(void *arg)
{
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("[control] socket"); return NULL; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(CONTROL_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[control] bind"); close(server_fd); return NULL;
    }
    if (listen(server_fd, 1) < 0) {
        perror("[control] listen"); close(server_fd); return NULL;
    }

    g_server_fd = server_fd;
    printf("[control] tuning server listening on port %d\n", CONTROL_PORT);

    while (g_control_running) {
        struct sockaddr_in client_addr;
        socklen_t alen = sizeof(client_addr);
        int fd = accept(server_fd, (struct sockaddr *)&client_addr, &alen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;  /* server_fd closed by control_stop() */
        }

        pthread_mutex_lock(&g_client_lock);
        if (g_client_fd >= 0) close(g_client_fd);  /* single-client: replace previous */
        g_client_fd = fd;
        pthread_mutex_unlock(&g_client_lock);

        printf("[control] tuning client connected\n");
        handle_control_client(fd);

        pthread_mutex_lock(&g_client_lock);
        if (g_client_fd == fd) { close(fd); g_client_fd = -1; }
        pthread_mutex_unlock(&g_client_lock);
        printf("[control] tuning client disconnected\n");
    }

    return NULL;
}

int control_start(void)
{
    /* Tee stdout BEFORE any other init runs, so the full boot log (ISP,
     * VI, VENC) reaches the tuning tab's debug view, not just ae/night's
     * own logging. */
    g_orig_stdout = dup(STDOUT_FILENO);
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("[control] pipe"); return -1; }
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) { perror("[control] dup2"); return -1; }
    close(pipefd[1]);
    g_log_pipe_r = pipefd[0];
    setvbuf(stdout, NULL, _IOLBF, 0);  /* line-buffered so printf flushes promptly through the pipe */

    if (pthread_create(&g_log_tid, NULL, log_pump, NULL) != 0) {
        perror("[control] log_pump pthread_create");
        return -1;
    }

    g_control_running = 1;
    if (pthread_create(&g_control_tid, NULL, control_accept_loop, NULL) != 0) {
        perror("[control] pthread_create");
        g_control_running = 0;
        return -1;
    }
    return 0;
}

void control_stop(void)
{
    g_control_running = 0;
    if (g_server_fd >= 0) {
        shutdown(g_server_fd, SHUT_RDWR);
        close(g_server_fd);
        g_server_fd = -1;
    }
    pthread_mutex_lock(&g_client_lock);
    if (g_client_fd >= 0) {
        /* shutdown() before close() — the accept thread may be blocked in
         * recv() inside handle_control_client(); shutdown() forces that
         * call to return cleanly instead of racing a bare close(). */
        shutdown(g_client_fd, SHUT_RDWR);
        close(g_client_fd);
        g_client_fd = -1;
    }
    pthread_mutex_unlock(&g_client_lock);
    pthread_join(g_control_tid, NULL);
    /* log_pump isn't joined — it blocks on read() of the tee'd stdout pipe
     * and only returns when that pipe closes at process exit, which is
     * always imminent by the time control_stop() runs. Acceptable for a
     * debug tool's shutdown path. */
}
