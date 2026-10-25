// Command Clips: a running program's windows as text, and actions on them as messages, without looking at the screen
// (docs: VXRuntime/CLIPS.md). VXRT_CONTROL=<path> opens a Unix socket there (mode 0600, removed at exit); each line
// sent is one request, each answered with one line of JSON:
//   state                      the windows: class, title, client size and origin on the screen, the text drawn in each
//                              and where; the active window, the focus; whether the program is idle
//   windows                    the same without the text (what a screenshot-only agent may know: tools/vxrt_screen.py)
//   snapshot <hwnd> <path>     the window's client area as a PNG file (what GDI drew into it)
//   snapshot-view <hwnd> <path>  what its Mac window shows of it (the committed picture, its child windows, alpha),
//                              at the window's backing scale, as a PNG file; answers the scale and the size in pixels
//   click <hwnd> <x> <y>       a left click at client coordinates (hwnd 0: the active window, else the first visible one)
//   click-text <hwnd> <text>   a click on the middle of the first drawn text that contains <text> (hwnd 0: any window)
//   key <hwnd> <vk>[+<vk>...]  keys pressed in order and let go in reverse (virtual-key codes, decimal or 0x..: 0x11+0x56
//                              is Ctrl+V), to the focus window when it is hwnd or inside it; no WM_CHAR: see type
//   type <hwnd> <text>         the characters of <text> (UTF-8) as WM_CHAR, to the focus window when it is inside hwnd
//   pointer <hwnd> <x> <y>     the pointer moved to client x, y, no button (WM_MOUSEMOVE; GetCursorPos for a second)
//   wheel <hwnd> <x> <y> <delta>  the mouse wheel turned (120 a notch, positive away from the user) with the pointer at
//                              client x, y: WM_MOUSEWHEEL to the focus window, as Windows sends it
//   ime <hwnd> <text>          <text> composed in the input method, then committed: WM_IME_STARTCOMPOSITION,
//                              WM_IME_COMPOSITION (the composition, then the result), WM_IME_ENDCOMPOSITION (ime.c)
//   clipboard                  the clipboard's text; clipboard-set <text>: the clipboard holds <text> (as another
//                              program's copy would leave it)
//   close <hwnd>               WM_CLOSE
//   wait-idle [ms]             answers once the program is idle (user32.c, vx_ui_idle: the actions so far handled and
//                              painted), or after ms (default 2000) with "idle": false
//   tray                       the notification-area icons (tray.m): owner window, id, callback message, version, tip,
//                              picture size, where in the menu bar
//   tray-click <n> [how]       the n-th of them clicked: left (default), right, double; or chosen with the keyboard: key,
//                              menu-key. A click goes through the status item's button as a real one does
//   tray-snapshot <n> <path>   the n-th one's picture as the menu bar has it (twice its size), as a PNG file
//   menu                       the popup menu open now (TrackPopupMenu, menus.m) and its items
//   menu-pick <text>           its first enabled item whose text contains <text> (or #<id>) chosen, as a click would
//                              (waiting up to 2 s for a menu to open)
//   menu-cancel                the open menu closed without a choice
// VXRT_CONTROL_LOG=<file>: each request appended with the milliseconds since the socket opened (how an agent worked).
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
static FILE *g_log;
static double g_t0;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

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
    if (g_log) { pthread_mutex_lock(&g_log_lock); fprintf(g_log, "%.1f %s\n", now_ms() - g_t0, line); fflush(g_log); pthread_mutex_unlock(&g_log_lock); }
    char *s = line, *cmd = word(&s);
    if (!cmd) { reply_err(fd, "empty request"); return; }
    if (!strcmp(cmd, "state") || !strcmp(cmd, "windows")) { char *j = vx_ui_state(cmd[0] == 's'); reply(fd, j); free(j); return; }
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
    if (!strcmp(cmd, "tray")) { char *j = vx_tray_state(); reply(fd, j); free(j); return; }
    if (!strcmp(cmd, "menu")) { char *j = vx_menu_state(); reply(fd, j); free(j); return; }
    if (!strcmp(cmd, "tray-click")) {
        char why[160] = "tray-click <index> [left|right|double|key|menu-key]", *n = word(&s), *how = word(&s);
        if (n && vx_tray_click(atoi(n), how ? how : "left", why, sizeof why)) reply(fd, "{\"ok\":true}"); else reply_err(fd, why);
        return;
    }
    if (!strcmp(cmd, "tray-snapshot")) {
        char why[160] = "tray-snapshot <index> <path>", *n = word(&s);
        while (*s == ' ') s++;
        if (n && *s && vx_tray_snapshot(atoi(n), s, why, sizeof why)) reply(fd, "{\"ok\":true}"); else reply_err(fd, why);
        return;
    }
    if (!strcmp(cmd, "menu-pick") || !strcmp(cmd, "menu-cancel")) {
        while (*s == ' ') s++;
        char why[160] = "menu-pick <text>";
        int pick = cmd[5] == 'p';
        if ((!pick || *s) && vx_menu_pick(pick ? s : NULL, 2000, why, sizeof why)) reply(fd, "{\"ok\":true}"); else reply_err(fd, why);
        return;
    }
    if (!strcmp(cmd, "clipboard")) {
        char text[4096], b[4096 * 6 + 64];
        if (!vx_ui_clipboard_get(text, sizeof text)) { reply(fd, "{\"ok\":true,\"text\":null}"); return; }
        size_t k = (size_t)snprintf(b, sizeof b, "{\"ok\":true,\"text\":\"");
        for (const unsigned char *c = (const unsigned char *)text; *c && k + 8 < sizeof b; c++) {
            if (*c == '"' || *c == '\\') { b[k++] = '\\'; b[k++] = (char)*c; }
            else if (*c < 0x20) k += (size_t)snprintf(b + k, sizeof b - k, "\\u%04x", *c);
            else b[k++] = (char)*c;
        }
        snprintf(b + k, sizeof b - k, "\"}");
        reply(fd, b);
        return;
    }
    if (!strcmp(cmd, "clipboard-set")) {
        while (*s == ' ') s++;
        if (vx_ui_clipboard_set(s)) reply(fd, "{\"ok\":true}"); else reply_err(fd, "clipboard-set <text>: the clipboard could not be set");
        return;
    }
    HANDLE h = window_arg(&s);
    if (!h) { reply_err(fd, "no such window"); return; }
    if (!strcmp(cmd, "click")) {
        char *x = word(&s), *y = word(&s), *hold = word(&s);
        if (!x || !y) { reply_err(fd, "click <hwnd> <x> <y> [hold ms]"); return; }
        vx_ui_click_hold(h, atoi(x), atoi(y), hold ? atoi(hold) : 50);
    } else if (!strcmp(cmd, "key")) {
        char *k = word(&s);
        if (!k) { reply_err(fd, "key <hwnd> <vk>[+<vk>...]"); return; }
        int vks[8], n = 0;
        for (char *p = k; *p && n < 8; ) {
            char *end;
            long v = strtol(p, &end, 0);
            if (end == p || v <= 0 || v > 0xfe) { reply_err(fd, "key <hwnd> <vk>[+<vk>...]: a virtual-key code is 1..0xfe"); return; }
            vks[n++] = (int)v;
            p = *end == '+' ? end + 1 : end;
            if (*end && *end != '+') { reply_err(fd, "key <hwnd> <vk>[+<vk>...]"); return; }
        }
        vx_ui_keys(h, vks, n);
    } else if (!strcmp(cmd, "pointer")) {
        char *x = word(&s), *y = word(&s);
        if (!x || !y) { reply_err(fd, "pointer <hwnd> <x> <y>"); return; }
        vx_ui_pointer(h, atoi(x), atoi(y));
    } else if (!strcmp(cmd, "wheel")) {
        char *x = word(&s), *y = word(&s), *d = word(&s);
        if (!x || !y || !d) { reply_err(fd, "wheel <hwnd> <x> <y> <delta>"); return; }
        vx_ui_wheel(h, atoi(x), atoi(y), atoi(d));
    } else if (!strcmp(cmd, "ime")) {
        while (*s == ' ') s++;
        if (!*s || !vx_ui_ime(h, s, s)) { reply_err(fd, "ime <hwnd> <text>: no window to compose in"); return; }
    } else if (!strcmp(cmd, "type")) {
        while (*s == ' ') s++;
        vx_ui_type(h, s);
    } else if (!strcmp(cmd, "snapshot")) {
        while (*s == ' ') s++;
        if (!*s || !vx_ui_snapshot(h, s)) { reply_err(fd, "snapshot <hwnd> <path>: not written"); return; }
    } else if (!strcmp(cmd, "snapshot-view")) {
        while (*s == ' ') s++;
        char info[200] = "", b[320];
        if (!*s || !vx_ui_snapshot_view(h, s, info, sizeof info)) { reply_err(fd, "snapshot-view <hwnd> <path>: not written (no Mac window shows it)"); return; }
        snprintf(b, sizeof b, "{\"ok\":true,\"hwnd\":%lld,%s}", (long long)(intptr_t)h, info);
        reply(fd, b);
        return;
    } else if (!strcmp(cmd, "close")) vx_window_post(h, WM_CLOSE, 0, 0);
    else { reply_err(fd, "unknown request (state, windows, snapshot, snapshot-view, click, click-text, key, type, wheel, ime, clipboard, clipboard-set, close, wait-idle, tray, tray-click, tray-snapshot, menu, menu-pick, menu-cancel)"); return; }
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
    g_t0 = now_ms();
    if (getenv("VXRT_CONTROL_LOG")) g_log = fopen(getenv("VXRT_CONTROL_LOG"), "a");
    pthread_t t;
    if (pthread_create(&t, NULL, serve, (void *)(intptr_t)ls) == 0) pthread_detach(t);
    vx_log("control: Command Clips at %s", path);
    // the socket is this program's: a program it starts (a launcher's game) must not take the path over and
    // remove it when it exits (g_path keeps the copy the socket needs)
    // VXRT_CONTROL_CHILD=<path>: the program it starts gets that socket instead (a launcher's game, to look at it)
    // (VXRT_CONTROL_CHILD_FOR=<program.exe>: only that one of them takes it, a watchdog started first passes it on)
    const char *child = getenv("VXRT_CONTROL_CHILD"), *child_for = getenv("VXRT_CONTROL_CHILD_FOR");
    if (child && *child) {
        setenv("VXRT_CONTROL", child, 1); unsetenv("VXRT_CONTROL_CHILD");
        if (child_for && *child_for) { setenv("VXRT_CONTROL_FOR", child_for, 1); unsetenv("VXRT_CONTROL_CHILD_FOR"); }
    } else unsetenv("VXRT_CONTROL");
    unsetenv("VXRT_CONTROL_LOG");
}
