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
// --single-process, which no Steam client option sets (2026-10-24: -cef-single-process and -cef-in-process-gpu add
// nothing to the webhelper's command line; the client's own -cef-disable-gpu and -cef-disable-sandbox do their part).
// Chromium's renderer, GPU and utility processes talk Mojo over handles the browser process hands them, which vxrt
// cannot pass between processes yet.
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

// argv[0] is vxrt, argv[1] the program; on a match *argv becomes a new array with the arguments after argv[1].
void vx_extra_args(int *argc, char ***argv) {
    const char *spec = getenv("VXRT_EXTRA_ARGS");
    if (!spec || !*spec || *argc < 2) return;
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
    if (!n) { free(added); return; }
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
    free(shown); free(added);
}
