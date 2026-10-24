// wevtapi.dll, the Windows Event Log query API, on a Mac: there is no Windows event log here and nothing ever writes
// one, so the standard channels exist and hold no events, and other channels do not exist. Chromium (the Windows
// Steam client's steamwebhelper) delay-loads the DLL and stops at a CHECK when it cannot be loaded; it reads the log
// on its second start. From Microsoft's documentation of each function:
//   EvtQuery            the channels Application, System, Setup and ForwardedEvents answer an empty result set;
//                       Security needs the auditing privilege a program here does not have (ERROR_ACCESS_DENIED);
//                       any other channel: ERROR_EVT_CHANNEL_NOT_FOUND (also when a structured XML query names it,
//                       unless EvtQueryTolerateQueryErrors). Log files (EvtQueryFilePath) cannot be read here.
//   EvtNext             no more items, ever
//   EvtCreateRenderContext / EvtRender / EvtClose   contexts are made; with no events there is nothing to render.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "vx.h"

enum { ERROR_ACCESS_DENIED = 5, ERROR_INVALID_HANDLE = 6, ERROR_NOT_SUPPORTED = 50, ERROR_INVALID_PARAMETER = 87,
       ERROR_NO_MORE_ITEMS = 259, ERROR_EVT_CHANNEL_NOT_FOUND = 15007 };
enum { EvtQueryChannelPath = 1, EvtQueryFilePath = 2, EvtQueryForwardDirection = 0x100, EvtQueryReverseDirection = 0x200,
       EvtQueryTolerateQueryErrors = 0x1000 };
enum { EvtRenderContextValues = 0, EvtRenderContextSystem = 1, EvtRenderContextUser = 2 };

// Handles: the objects made here, so that one the program did not get from here is refused, not read.
enum { EVT_QUERY = 1, EVT_RENDER_CONTEXT };
typedef struct { int kind; } evt_object;
static evt_object **g_objects; static int g_count, g_cap;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static evt_object *make(int kind) {
    evt_object *o = malloc(sizeof *o);
    o->kind = kind;
    pthread_mutex_lock(&g_lock);
    if (g_count == g_cap) { g_cap = g_cap ? g_cap * 2 : 16; g_objects = realloc(g_objects, (size_t)g_cap * sizeof *g_objects); }
    g_objects[g_count++] = o;
    pthread_mutex_unlock(&g_lock);
    return o;
}
static int live(const void *h, int kind) {                       // kind 0: any
    pthread_mutex_lock(&g_lock);
    int found = 0;
    for (int i = 0; i < g_count && !found; i++) found = g_objects[i] == h && (!kind || g_objects[i]->kind == kind);
    pthread_mutex_unlock(&g_lock);
    return found;
}

// A channel's standing here: 1 exists (and is empty), 0 does not exist, -1 exists but may not be read.
static int channel(const WCHAR *name, size_t n) {
    static const char *const empty[] = { "Application", "System", "Setup", "ForwardedEvents" };
    char a[64];
    if (n >= sizeof a) return 0;
    for (size_t i = 0; i < n; i++) { if (name[i] > 0x7e) return 0; a[i] = (char)name[i]; }
    a[n] = 0;
    for (size_t i = 0; i < sizeof empty / sizeof *empty; i++) if (!strcasecmp(a, empty[i])) return 1;
    return strcasecmp(a, "Security") ? 0 : -1;
}
static size_t wlen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
// A structured query (<QueryList><Query Path="..."><Select Path="...">...): every channel it names, by its Path
// attributes; the first one that does not exist or may not be read decides.
static int structured(const WCHAR *xml) {
    for (const WCHAR *p = xml; *p; p++) {
        if (!(p[0] == 'P' && p[1] == 'a' && p[2] == 't' && p[3] == 'h')) continue;
        const WCHAR *q = p + 4;
        while (*q == ' ') q++;
        if (*q++ != '=') continue;
        while (*q == ' ') q++;
        WCHAR quote = *q;
        if (quote != '"' && quote != '\'') continue;
        const WCHAR *start = ++q;
        while (*q && *q != quote) q++;
        int c = channel(start, (size_t)(q - start));
        if (c != 1) return c;
        p = q;
    }
    return 1;
}

static WINAPI void *EvtQuery(void *session, const WCHAR *path, const WCHAR *query, DWORD flags) {
    if (session) { vx_set_last_error(ERROR_INVALID_HANDLE); return NULL; }      // (no EvtOpenSession here)
    DWORD kind = flags & (EvtQueryChannelPath | EvtQueryFilePath);
    if (kind != EvtQueryChannelPath && kind != EvtQueryFilePath) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    if (kind == EvtQueryFilePath) { vx_set_last_error(ERROR_NOT_SUPPORTED); return NULL; }
    int c;
    if (path) c = channel(path, wlen(path));
    else if (query) c = structured(query);
    else { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    if (c == 0 && !path && (flags & EvtQueryTolerateQueryErrors)) c = 1;
    if (vx_trace) {
        char *p = vx_utf8_dup(path ? path : query);
        vx_log("EvtQuery(%s): %s", p, c == 1 ? "no events (there is no Windows event log here)" : c ? "access denied" : "no such channel");
        free(p);
    }
    if (c != 1) { vx_set_last_error(c ? ERROR_ACCESS_DENIED : ERROR_EVT_CHANNEL_NOT_FOUND); return NULL; }
    return make(EVT_QUERY);
}
static WINAPI BOOL EvtNext(void *results, DWORD size, void **events, DWORD timeout, DWORD flags, DWORD *returned) {
    (void)timeout;
    if (returned) *returned = 0;
    if (!live(results, EVT_QUERY)) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if (!size || !events || !returned || flags) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    vx_set_last_error(ERROR_NO_MORE_ITEMS);
    return 0;
}
static WINAPI void *EvtCreateRenderContext(DWORD count, const WCHAR **paths, DWORD flags) {
    int values = flags == EvtRenderContextValues;
    if (flags > EvtRenderContextUser || (values ? !count || !paths : count || paths)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    return make(EVT_RENDER_CONTEXT);
}
// What is rendered is an event or a bookmark, and there are none here.
static WINAPI BOOL EvtRender(void *context, void *fragment, DWORD flags, DWORD size, void *buffer, DWORD *used, DWORD *count) {
    (void)context; (void)fragment; (void)flags; (void)size; (void)buffer;
    if (used) *used = 0;
    if (count) *count = 0;
    vx_set_last_error(ERROR_INVALID_HANDLE);
    return 0;
}
static WINAPI BOOL EvtClose(void *h) {
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < g_count && g_objects[i] != h) i++;
    int found = i < g_count;
    if (found) { free(g_objects[i]); g_objects[i] = g_objects[--g_count]; }
    pthread_mutex_unlock(&g_lock);
    if (!found) vx_set_last_error(ERROR_INVALID_HANDLE);
    return found;
}

static const vx_export wevtapi[] = {
    VX_EXPORT(EvtQuery), VX_EXPORT(EvtNext), VX_EXPORT(EvtCreateRenderContext), VX_EXPORT(EvtRender), VX_EXPORT(EvtClose),
};

void vx_wevtapi_init(void) {
    vx_register_module("wevtapi.dll", wevtapi, sizeof wevtapi / sizeof wevtapi[0]);
}

#ifdef __aarch64__
#include "thunks/wevtapi.c.inc"
#endif
