// Experimental (not part of make check). Build: x86_64-w64-mingw32-gcc -O1 -o steamwebhelper.exe tools/steam_fake_webhelper.c, then
// put it in the overlay at bin/cef/cef.win64/steamwebhelper.exe so the Windows Steam client starts it instead of Chromium.
// A stand-in for steamwebhelper.exe, so the Steam client gets past "connect to the HTML process" without
// Chromium. Steam's CSharedMemStream: a mapping "<name>_mem-IPCWrapper" holding a 16-byte header
// {read, write, size, used} and a ring, a mutex "<name>_mutex-IPCWrapper", events "_written" and "_avail".
// The helper makes the master stream; Steam writes {cmd 1, 1, its pid, char reply_stream[64]} there and waits
// on its reply stream for {cmd, id, char name[64]}; it then makes "<name>_client"/"<name>_server" itself.
// This helper answers, then drains whatever Steam writes into those streams.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef struct { volatile LONG rd, wr, size, used; unsigned char data[]; } hdr;
typedef struct { HANDLE mx, written, avail, map; hdr *h; } stream;
static int open_stream(stream *s, const char *base, LONG cap, int create) {
    char n[256];
    snprintf(n, sizeof n, "%s_mutex-IPCWrapper", base); s->mx = create ? CreateMutexA(NULL, FALSE, n) : OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, n);
    snprintf(n, sizeof n, "%s_written-IPCWrapper", base); s->written = create ? CreateEventA(NULL, FALSE, FALSE, n) : OpenEventA(EVENT_ALL_ACCESS, FALSE, n);
    snprintf(n, sizeof n, "%s_avail-IPCWrapper", base); s->avail = create ? CreateEventA(NULL, FALSE, FALSE, n) : OpenEventA(EVENT_ALL_ACCESS, FALSE, n);
    snprintf(n, sizeof n, "%s_mem-IPCWrapper", base);
    s->map = create ? CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 16 + cap, n) : OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, n);
    if (!s->mx || !s->written || !s->avail || !s->map) return 0;
    s->h = MapViewOfFile(s->map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (create) { s->h->size = cap; s->h->rd = s->h->wr = s->h->used = 0; }
    return s->h != NULL;
}
// Takes up to n bytes (all there is when buf is NULL); returns how many.
static LONG get(stream *s, void *buf, LONG n) {
    WaitForSingleObject(s->mx, INFINITE);
    LONG cap = s->h->size, used = s->h->used, k = buf ? (used < n ? used : n) : used;
    for (LONG i = 0; buf && i < k; i++) ((unsigned char *)buf)[i] = s->h->data[(s->h->rd + i) % cap];
    s->h->rd = (s->h->rd + k) % cap; s->h->used -= k;
    ReleaseMutex(s->mx);
    if (k) SetEvent(s->avail);
    return k;
}
static void put(stream *s, const void *buf, LONG n) {
    WaitForSingleObject(s->mx, INFINITE);
    LONG cap = s->h->size;
    for (LONG i = 0; i < n; i++) s->h->data[(s->h->wr + i) % cap] = ((const unsigned char *)buf)[i];
    s->h->wr = (s->h->wr + n) % cap; s->h->used += n;
    ReleaseMutex(s->mx);
    SetEvent(s->written);
}
int main(int argc, char **argv) {
    unsigned spid = 0;
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], "-steampid=", 10)) spid = (unsigned)atoi(argv[i] + 10);
    char base[128];
    snprintf(base, sizeof base, "SteamChrome_MasterStream_spid%u", spid);
    stream master, conn[2]; int connected = 0;
    if (!open_stream(&master, base, 8192, 1)) { fprintf(stderr, "fakewh: no master stream\n"); return 1; }
    fprintf(stderr, "fakewh: master stream %s ready\n", base);
    unsigned long drained = 0;
    for (;;) {
        WaitForSingleObject(master.written, 50);
        struct { DWORD cmd, ver, pid; char reply[64]; } req;
        while (master.h->used >= (LONG)sizeof req && get(&master, &req, sizeof req) == sizeof req) {
            req.reply[63] = 0;
            fprintf(stderr, "fakewh: connect from pid %lu, reply stream %s\n", (unsigned long)req.pid, req.reply);
            stream r;
            if (!open_stream(&r, req.reply, 0, 0)) { fprintf(stderr, "fakewh: cannot open %s\n", req.reply); continue; }
            struct { DWORD cmd, id; char name[64]; } ans = { 1, GetCurrentProcessId(), "" };
            snprintf(ans.name, sizeof ans.name, "SteamChrome_FakeHelper_%lu", (unsigned long)GetCurrentProcessId());
            put(&r, &ans, sizeof ans);
            fprintf(stderr, "fakewh: answered with %s\n", ans.name);
            for (int t = 0; t < 100 && !connected; t++) {               // Steam makes the two streams after reading the answer
                char n[96];
                snprintf(n, sizeof n, "%s_server", ans.name);
                stream a, b;
                if (open_stream(&a, n, 0, 0)) {
                    snprintf(n, sizeof n, "%s_client", ans.name);
                    if (open_stream(&b, n, 0, 0)) { conn[0] = a; conn[1] = b; connected = 1; fprintf(stderr, "fakewh: connected to %s_client/_server\n", ans.name); break; }
                }
                Sleep(50);
            }
        }
        for (int i = 0; connected && i < 2; i++) {
            LONG k = get(&conn[i], NULL, 0);
            if (k && drained < 64 * 1024) fprintf(stderr, "fakewh: drained %ld bytes from %s\n", k, i ? "_client" : "_server");
            drained += k;
        }
    }
}
