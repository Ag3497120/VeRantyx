// Command Clips: a running program's windows as text, and actions on them as messages, without looking at the screen
// (docs: VXRuntime/CLIPS.md). VXRT_CONTROL=<path> opens a Unix socket there (mode 0600, removed at exit); each line
// sent is one request, each answered with one line of JSON:
//   state                      the windows: class, title, client size, the text drawn in each and where; the active
//                              window, the focus; whether the program is idle
//   click <hwnd> <x> <y>       a left click at client coordinates (hwnd 0: the active window, else the first visible one)
//   click-text <hwnd> <text>   a click on the middle of the first drawn text that contains <text> (hwnd 0: any window)
//   key <hwnd> <vk>            a key pressed and let go (a virtual-key code, decimal or 0x..; no WM_CHAR: see type)
//   type <hwnd> <text>         the characters of <text> (UTF-8) as WM_CHAR
//   close <hwnd>               WM_CLOSE
//   wait-idle [ms]             answers once the program is idle (user32.c, vx_ui_idle: the actions so far handled and
//                              painted), or after ms (default 2000) with "idle": false
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "ui.h"

static char g_path[104];

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static void sleep_ms(int ms) { struct timespec t = { 0, ms * 1000000L }; nanosleep(&t, NULL); }

static void reply(int fd, const char *s) {
    size_t n = strlen(s);
    while (n) { ssize_t k = write(fd, s, n); if (k <= 0) return; s += k; n -= (size_t)k; }
    if (write(fd, "\n", 1) < 0) return;
}
static void reply_ok(int fd, HANDLE h) { char b[96]; snprintf(b, sizeof b, "{\"ok\":true,\"hwnd\":%lld}", (long long)(intptr_t)h); reply(fd, b); }
static void reply_err(int fd, const char *what) {
    char b[256]; snprintf(b, sizeof b, "{\"ok\":false,\"error\":\"%s\"}", what); reply(fd, b);
}
// The next word of *s (spaces skipped); the rest stays in *s.
static char *word(char **s) {
    while (**s == ' ') (*s)++;
    if (!**s) return NULL;
    char *w = *s;
    while (**s && **s != ' ') (*s)++;
    if (**s) *(*s)++ = 0;
    return w;
}
static HANDLE window_arg(char **s) {
    char *w = word(s);
    return vx_ui_window(w ? (HANDLE)(intptr_t)strtoll(w, NULL, 0) : NULL);
}

static void request(int fd, char *line) {
    char *s = line, *cmd = word(&s);
    if (!cmd) { reply_err(fd, "empty request"); return; }
    if (!strcmp(cmd, "state")) { char *j = vx_ui_state(); reply(fd, j); free(j); return; }
    if (!strcmp(cmd, "wait-idle")) {
        char *a = word(&s);
        int ms = a ? atoi(a) : 2000, stable = 0;
        double t0 = now_ms();
        for (;;) {
            stable = vx_ui_idle() ? stable + 1 : 0;                       // (twice in a row: a message posted between
            if (stable >= 2) break;                                       //  two threads' checks is not missed)
            if (now_ms() - t0 >= ms) break;
            sleep_ms(1);
        }
        char b[96]; snprintf(b, sizeof b, "{\"ok\":true,\"idle\":%s,\"ms\":%.1f}", stable >= 2 ? "true" : "false", now_ms() - t0);
        reply(fd, b);
        return;
    }
    if (!strcmp(cmd, "click-text")) {
        char *w = word(&s);
        while (*s == ' ') s++;
        HANDLE in; POINT at;
        if (!w || !*s) { reply_err(fd, "click-text <hwnd> <text>"); return; }
        if (!vx_ui_find_text((HANDLE)(intptr_t)strtoll(w, NULL, 0), s, &in, &at)) { reply_err(fd, "no drawn text contains it"); return; }
        vx_ui_click(in, (int)at.x, (int)at.y);
        char b[128]; snprintf(b, sizeof b, "{\"ok\":true,\"hwnd\":%lld,\"at\":[%d,%d]}", (long long)(intptr_t)in, (int)at.x, (int)at.y);
        reply(fd, b);
        return;
    }
    HANDLE h = window_arg(&s);
    if (!h) { reply_err(fd, "no such window"); return; }
    if (!strcmp(cmd, "click")) {
        char *x = word(&s), *y = word(&s);
        if (!x || !y) { reply_err(fd, "click <hwnd> <x> <y>"); return; }
        vx_ui_click(h, atoi(x), atoi(y));
    } else if (!strcmp(cmd, "key")) {
        char *k = word(&s);
        if (!k) { reply_err(fd, "key <hwnd> <vk>"); return; }
        vx_ui_key(h, (int)strtol(k, NULL, 0));
    } else if (!strcmp(cmd, "type")) {
        while (*s == ' ') s++;
        vx_ui_type(h, s);
    } else if (!strcmp(cmd, "close")) vx_window_post(h, WM_CLOSE, 0, 0);
    else { reply_err(fd, "unknown request (state, click, click-text, key, type, close, wait-idle)"); return; }
    reply_ok(fd, h);
}

static void *connection(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[8192]; size_t n = 0;
    for (;;) {
        ssize_t k = read(fd, buf + n, sizeof buf - 1 - n);
        if (k <= 0) break;
        n += (size_t)k;
        char *nl;
        while ((nl = memchr(buf, '\n', n))) {
            *nl = 0;
            if (nl > buf && nl[-1] == '\r') nl[-1] = 0;
            request(fd, buf);
            n -= (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, n);
        }
        if (n == sizeof buf - 1) n = 0;                                    // (a line too long: dropped)
    }
    close(fd);
    return NULL;
}
static void *serve(void *arg) {
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        pthread_t t;
        if (pthread_create(&t, NULL, connection, (void *)(intptr_t)fd) == 0) pthread_detach(t); else close(fd);
    }
    return NULL;
}
static void remove_socket(void) { if (g_path[0]) unlink(g_path); }

void vx_control_start(const char *path) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof a.sun_path) { vx_log("control: the socket path is too long: %s", path); return; }
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0) return;
    unlink(path);
    mode_t old = umask(077);                                              // (only this user may connect)
    int ok = bind(ls, (struct sockaddr *)&a, sizeof a) == 0 && listen(ls, 4) == 0;
    umask(old);
    if (!ok) { vx_log("control: cannot listen at %s: %s", path, strerror(errno)); close(ls); return; }
    snprintf(g_path, sizeof g_path, "%s", path);
    atexit(remove_socket);
    pthread_t t;
    if (pthread_create(&t, NULL, serve, (void *)(intptr_t)ls) == 0) pthread_detach(t);
    vx_log("control: Command Clips at %s", path);
}
