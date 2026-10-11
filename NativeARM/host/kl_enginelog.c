// The engine's console output. Source 2 logs through tier0's LoggingSystem;
// on this host nothing shows it, and the engine ends with _exit() when VR or
// Vulkan setup fails, taking the reason with it. These interposers print every
// message to stderr and then hand it to the real tier0 routine, so the
// engine's own listeners (console.log) still see it.
//
// The variadic entry points take Linux AAPCS64 varargs (in registers), so they
// arrive through thunks in kl_glibc_va.S that build a kl_va; the message is
// formatted here and forwarded as ("%s", text) through a plain two-argument
// call, which both ABIs place in x0/x1.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "klepton.h"
#include "kl_va.h"
#include "kl_enginelog.h"

// Passed through register-exact: the third parameter's type (a Color struct or
// something wider) is not known for certain, and a wrong C type would
// truncate it. The message is whichever of the last two words is a pointer.
typedef uint64_t (*log_direct_fn)(uint64_t a, uint64_t b, uint64_t c, uint64_t d);
typedef void (*fmt1_fn)(const char *fmt, const char *arg);
static log_direct_fn g_real_log_direct;
static fmt1_fn g_real_msg, g_real_warning;
static void (*g_real_debug_print)(const char *);
static int g_enabled = 1;
static unsigned g_lines;

static void emit(const char *kind, const char *text) {
    if (!g_enabled) return;
    size_t n = strlen(text);
    fprintf(stderr, "[engine%s] %s%s", kind, text, n && text[n - 1] == '\n' ? "" : "\n");
    g_lines++;
}

#define FORMAT(fmt, va, out)                                                   \
    char _m[1024] __attribute__((aligned(16)));                                \
    char out[4096];                                                            \
    if (kl_va_marshal((fmt), (va), _m, sizeof _m, KL_VA_PRINTF) == (size_t)-1) \
        snprintf(out, sizeof out, "%s", (fmt));                                \
    else vsnprintf(out, sizeof out, (fmt), (va_list)_m)

extern void klg_LoggingSystem_Log(void), klg_LoggingSystem_LogColor(void), klg_Msg(void), klg_Warning(void);

int klgh_LoggingSystem_Log(int channel, int severity, const char *fmt, kl_va *va) {
    FORMAT(fmt, va, text);
    char kind[32]; snprintf(kind, sizeof kind, severity >= 3 ? " ch%d ERROR" : severity == 2 ? " ch%d warn" : " ch%d", channel);
    emit(kind, text);
    return 0;   // LoggingResponse_Continue; the engine's own listeners are not involved
}
int klgh_LoggingSystem_LogColor(int channel, int severity, uint32_t color, const char *fmt, kl_va *va) {
    FORMAT(fmt, va, text);
    char kind[32]; snprintf(kind, sizeof kind, severity >= 3 ? " ch%d ERROR" : severity == 2 ? " ch%d warn" : " ch%d", channel);
    emit(kind, text);
    return 0;
}
int klgh_Msg(const char *fmt, kl_va *va) {
    FORMAT(fmt, va, text);
    emit("", text);
    if (g_real_msg) g_real_msg("%s", text);
    return 0;
}
int klgh_Warning(const char *fmt, kl_va *va) {
    FORMAT(fmt, va, text);
    emit(" warn", text);
    if (g_real_warning) g_real_warning("%s", text);
    return 0;
}
static uint64_t klg_LoggingSystem_LogDirect(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    const char *msg = d > 0x10000 ? (const char *)(uintptr_t)d : c > 0x10000 ? (const char *)(uintptr_t)c : "";
    char kind[32]; snprintf(kind, sizeof kind, " ch%d direct", (int)a);
    emit(kind, msg);
    return g_real_log_direct ? g_real_log_direct(a, b, c, d) : 0;
}
static void klg_Plat_DebugPrint(const char *msg) {
    emit(" dbg", msg ? msg : "");
    if (g_real_debug_print) g_real_debug_print(msg);
}

// The fatal-error path collects its explanation into the minidump comment
// (tier0's Plat_AppendMiniDumpErrorString, called through the PLT) and then
// crashes on purpose; with no minidump writer that text is the only account
// of why, so it is printed. The format argument is used as plain text.
static void klg_Plat_AppendMiniDumpErrorString(const char *fmt, const char *a1, const char *a2) {
    fprintf(stderr, "[engine minidump] %s", fmt ? fmt : "(null)");
    if (fmt && strstr(fmt, "%s") && a1) fprintf(stderr, " | %s", a1);
    fprintf(stderr, "\n");
}
static void describe_ptr(const char *what, const void *p) {
    size_t off = 0; const char *img = kl_addr_image(p, &off);
    fprintf(stderr, "    %s = %p (%s+%#zx)\n", what, p, img ? img : "host?", off);
}
static int klg_Plat_MessageBox(const char *title, const char *text) {
    fprintf(stderr, "[engine messagebox] %s: %s\n", title ? title : "", text ? text : "");
    if (text && strstr(text, "Mongoose")) {   // which object the client's watchdog asked
        kl_image *c = kl_find_image("libclient.so");
        if (c) {
            void *obj = *(void **)((uint8_t *)kl_base(c) + 0x26b96e8);
            describe_ptr("watchdog object", obj);
            if (obj) {
                void **vt = *(void ***)obj;
                describe_ptr("  vtable", vt);
                describe_ptr("  slot 0x70", vt[0x70 / 8]);
                describe_ptr("  slot 0xd0", vt[0xd0 / 8]);
            }
        }
    }
    return 0;
}

void kl_enginelog_install(void) {
    const char *e = getenv("ALYX_ENGINE_LOG");
    if (e && !strcmp(e, "0")) g_enabled = 0;
    kl_interpose("LoggingSystem_Log", (void *)klg_LoggingSystem_Log);
    kl_interpose("_Z17LoggingSystem_Logi17LoggingSeverity_t5ColorPKcz", (void *)klg_LoggingSystem_LogColor);
    kl_interpose("LoggingSystem_LogDirect", (void *)klg_LoggingSystem_LogDirect);
    kl_interpose("Msg", (void *)klg_Msg);
    kl_interpose("Warning", (void *)klg_Warning);
    kl_interpose("Plat_DebugPrint", (void *)klg_Plat_DebugPrint);
    kl_interpose("Plat_AppendMiniDumpErrorString", (void *)klg_Plat_AppendMiniDumpErrorString);
    kl_interpose("Plat_MessageBox", (void *)klg_Plat_MessageBox);
}

void kl_enginelog_bind(kl_image *tier0) {
    if (!tier0) return;
    g_real_log_direct = (log_direct_fn)kl_sym(tier0, "LoggingSystem_LogDirect");
    g_real_msg = (fmt1_fn)kl_sym(tier0, "Msg");
    g_real_warning = (fmt1_fn)kl_sym(tier0, "Warning");
    g_real_debug_print = (void (*)(const char *))kl_sym(tier0, "Plat_DebugPrint");
    fprintf(stderr, "  [engine] console output interposed (LogDirect %p, Msg %p)\n", (void *)g_real_log_direct, (void *)g_real_msg);
}

unsigned kl_enginelog_lines(void) { return g_lines; }
