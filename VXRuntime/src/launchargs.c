// VXRT_EXTRA_ARGS: arguments a launcher adds to one program's command line, so that a program another program starts
// (steam.exe starts steamwebhelper.exe) can be given an option without changing either program's files.
//
//   VXRT_EXTRA_ARGS='<program.exe>=<arguments>[;<program.exe>=<arguments>...]'
//
// When the program this vxrt process runs has that file name (in any folder; case ignored), the arguments go right
// after the program's name, before the ones it was started with: Chromium reads switches only before a "--", and
// the Wine-era steamwebhelper shim put its switches there too. Arguments are separated by spaces; double quotes keep
// spaces in one. The setting reaches child processes as every VXRT_ setting does (kernel32.c's CreateProcess), so
// it applies however deep in the process tree the program starts. Each process it changes says so in the log, with
// what it added.
//
// The Windows Steam launcher (scripts/launch_vxrt_windows_steam.sh) uses it for steamwebhelper.exe's
// --single-process and --disable-software-rasterizer, which no Steam client option sets (2026-10-24:
// -cef-single-process and -cef-in-process-gpu add nothing to the webhelper's command line; the client's own
// -cef-disable-gpu and -cef-disable-sandbox do their part). Chromium's renderer, GPU and utility processes talk Mojo
// over handles the browser process hands them, which vxrt cannot pass between processes yet; without a software
// rasterizer to fall back to, Chromium composites in software through GDI (2026-10-25: the sign-in window shows).
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "vx.h"

// Splits s (n bytes) at spaces and tabs, double quotes grouping; appends the pieces to out from index *count.
static void split_into(const char *s, size_t n, char **out, int *count) {
    size_t i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= n) break;
        char *arg = malloc(n - i + 1);
        size_t k = 0;
        int quoted = 0;
        for (; i < n && (quoted || (s[i] != ' ' && s[i] != '\t')); i++) {
            if (s[i] == '"') { quoted = !quoted; continue; }
            arg[k++] = s[i];
        }
        arg[k] = 0;
        out[(*count)++] = arg;
    }
}

// argv[0] is vxrt, argv[1] the program; on a match *argv becomes a new array with the arguments after argv[1], and the
// result is those arguments as command-line text (for a command line given word for word: loader.c), else NULL.
char *vx_extra_args(int *argc, char ***argv) {
    const char *spec = getenv("VXRT_EXTRA_ARGS");
    if (!spec || !*spec || *argc < 2) return NULL;
    const char *program = (*argv)[1], *base = program;
    for (const char *p = program; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    size_t room = strlen(spec) / 2 + 2;                     // (no more pieces than that)
    char **added = calloc(room, sizeof *added);
    int n = 0;
    for (const char *entry = spec; *entry; ) {
        const char *end = strchr(entry, ';');
        size_t len = end ? (size_t)(end - entry) : strlen(entry);
        const char *eq = memchr(entry, '=', len);
        if (eq && (size_t)(eq - entry) == strlen(base) && !strncasecmp(entry, base, strlen(base)))
            split_into(eq + 1, len - (size_t)(eq + 1 - entry), added, &n);
        entry += len + (end ? 1 : 0);
    }
    if (!n) { free(added); return NULL; }
    char **out = calloc((size_t)*argc + (size_t)n + 1, sizeof *out);
    out[0] = (*argv)[0]; out[1] = (*argv)[1];
    size_t shown_len = 1;
    for (int i = 0; i < n; i++) { out[2 + i] = added[i]; shown_len += strlen(added[i]) + 3; }
    for (int i = 2; i < *argc; i++) out[n + i] = (*argv)[i];
    char *shown = calloc(1, shown_len);
    for (int i = 0; i < n; i++) {
        if (i) strcat(shown, " ");
        int quote = strchr(added[i], ' ') != NULL;
        if (quote) strcat(shown, "\"");
        strcat(shown, added[i]);
        if (quote) strcat(shown, "\"");
    }
    *argc += n;
    *argv = out;
    vx_proc.argc = *argc - 1; vx_proc.argv = out + 1;      // (vx_log names a child from it; loader.c sets the same)
    vx_log("VXRT_EXTRA_ARGS: %s runs with %s added after its name", base, shown);
    free(added);
    return shown;
}

// <VXRT_ROOT>/program_env.txt: environment settings for one program, applied when that program starts, however it
// was started. A game that a running launcher (Windows Steam) starts gets that launcher's environment, which was fixed
// when the launcher started; measuring a game (VXRT_STATS, VXRT_PROFILE, MTL_HUD_ENABLED, ...) then needs no restart of
// the launcher and changes nothing in the game's or the launcher's files. One line per program:
//
//   # comment
//   NMS.exe VXRT_STATS=1 VXRT_PROFILE=/tmp/nms.prof
//
// The program by file name (any folder, case ignored); the settings separated by spaces, double quotes keeping spaces
// in one value. They replace what the environment had. A child of that program inherits them as any setting. Each
// process changed says so in the log. The root as files.c finds it: VXRT_ROOT, else ~/.vxrt.
#include <stdio.h>
void vx_program_env(const char *program) {
    char path[4096];
    const char *root = getenv("VXRT_ROOT");
    if (root && *root) snprintf(path, sizeof path, "%s/program_env.txt", root);
    else snprintf(path, sizeof path, "%s/.vxrt/program_env.txt", getenv("HOME") ? getenv("HOME") : "/tmp");
    FILE *f = fopen(path, "r");
    if (!f) return;
    const char *base = program;
    for (const char *p = program; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        size_t n = strcspn(line, "\r\n");
        line[n] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char *name_end = p;
        while (*name_end && *name_end != ' ' && *name_end != '\t') name_end++;
        if ((size_t)(name_end - p) != strlen(base) || strncasecmp(p, base, strlen(base))) continue;
        char *pieces[sizeof line / 2 + 1];
        int count = 0;
        split_into(name_end, strlen(name_end), pieces, &count);   // (at most one piece per two bytes: fits)
        for (int i = 0; i < count; i++) {
            char *eq = strchr(pieces[i], '=');
            if (eq && eq != pieces[i]) {
                *eq = 0;
                setenv(pieces[i], eq + 1, 1);
                vx_log("program_env.txt: %s runs with %s=%s", base, pieces[i], eq + 1);
            }
            free(pieces[i]);
        }
    }
    fclose(f);
}
