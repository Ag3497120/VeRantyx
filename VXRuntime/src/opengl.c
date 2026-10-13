// opengl32.dll: OpenGL for Windows programs, on macOS's own OpenGL (OpenGL.framework). Every GL function the SDK
// declares is a Windows-convention function calling the framework's (gl_thunks.h, generated); WGL contexts are CGL
// contexts: the legacy 2.1 profile, or the 4.1 core profile when a program asks for 3.0 or later without the
// compatibility profile (macOS has no 3.x+ compatibility profile). Contexts of a profile share their objects with
// the first of them, so wglShareLists always succeeds. Contexts have no drawable: VR programs draw into their own
// framebuffer objects (SwapBuffers flushes).
#define GL_SILENCE_DEPRECATION
#include "vx.h"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gltypes.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "gl_thunks.h"

#define NFUNCS (sizeof g_gl_functions / sizeof g_gl_functions[0])
typedef struct { CGLContextObj cgl; int core; } wgl_context;
static __thread wgl_context *t_current;
static __thread HANDLE t_dc;
static CGLContextObj g_share[2];                  // the first context of each profile (kept)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static wgl_context *create_context(int core, wgl_context *share) {
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)(core ? kCGLOGLPVersion_GL4_Core : kCGLOGLPVersion_Legacy),
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24, kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
        kCGLPFADepthSize, (CGLPixelFormatAttribute)24, kCGLPFAStencilSize, (CGLPixelFormatAttribute)8,
        kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers, (CGLPixelFormatAttribute)0 };
    CGLPixelFormatObj pf = NULL;
    GLint n = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &n) != kCGLNoError || !pf) { vx_log("OpenGL: no %s pixel format", core ? "core" : "legacy"); return NULL; }
    pthread_mutex_lock(&g_lock);
    CGLContextObj ctx = NULL;
    CGLError e = CGLCreateContext(pf, share && share->core == core ? share->cgl : g_share[core], &ctx);
    if (e == kCGLNoError && !g_share[core]) g_share[core] = CGLRetainContext(ctx);
    pthread_mutex_unlock(&g_lock);
    CGLReleasePixelFormat(pf);
    if (e != kCGLNoError) { vx_log("OpenGL: CGLCreateContext failed: %d", (int)e); return NULL; }
    wgl_context *w = calloc(1, sizeof *w);
    w->cgl = ctx; w->core = core;
    return w;
}

static WINAPI HANDLE wglCreateContext(HANDLE dc) { (void)dc; return create_context(0, NULL); }
static WINAPI BOOL wglDeleteContext(HANDLE h) {
    wgl_context *w = h;
    if (!w) return 0;
    if (t_current == w) { CGLSetCurrentContext(NULL); t_current = NULL; }
    CGLReleaseContext(w->cgl);
    free(w);
    return 1;
}
static WINAPI BOOL wglMakeCurrent(HANDLE dc, HANDLE h) {
    wgl_context *w = h;
    if (CGLSetCurrentContext(w ? w->cgl : NULL) != kCGLNoError) return 0;
    t_current = w; t_dc = w ? dc : NULL;
    return 1;
}
static WINAPI HANDLE wglGetCurrentContext(void) { return t_current; }
static WINAPI HANDLE wglGetCurrentDC(void) { return t_dc; }
static WINAPI BOOL wglShareLists(HANDLE a, HANDLE b) { (void)a; (void)b; return 1; }   // (they share already)

#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 2
static WINAPI HANDLE wglCreateContextAttribsARB(HANDLE dc, HANDLE share, const int *attribs) {
    (void)dc;
    int major = 1, profile = 1;
    for (const int *a = attribs; a && a[0]; a += 2) {
        if (a[0] == WGL_CONTEXT_MAJOR_VERSION_ARB) major = a[1];
        if (a[0] == WGL_CONTEXT_PROFILE_MASK_ARB) profile = a[1];
    }
    int core = major >= 3 && !(profile & WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB);
    vx_log("OpenGL: context for version %d (%s profile asked): macOS %s", major, profile & 2 ? "compatibility" : "core", core ? "4.1 core" : "2.1 legacy");
    return create_context(core, share);
}
static const char g_wgl_extensions[] = "WGL_ARB_extensions_string WGL_EXT_extensions_string WGL_ARB_create_context WGL_ARB_create_context_profile "
                                       "WGL_ARB_pixel_format WGL_EXT_swap_control WGL_ARB_framebuffer_sRGB";
static WINAPI const char *wglGetExtensionsStringARB(HANDLE dc) { (void)dc; return g_wgl_extensions; }
static WINAPI const char *wglGetExtensionsStringEXT(void) { return g_wgl_extensions; }
static WINAPI BOOL wglSwapIntervalEXT(int n) { (void)n; return 1; }
static WINAPI int wglGetSwapIntervalEXT(void) { return 1; }
static WINAPI BOOL wglChoosePixelFormatARB(HANDLE dc, const int *ia, const float *fa, uint32_t max, int *formats, uint32_t *n) {
    (void)dc; (void)ia; (void)fa;
    if (max && formats) formats[0] = 1;
    if (n) *n = 1;
    return 1;
}
static int pixel_attrib(int a) {                    // the one pixel format: RGBA8, depth 24, stencil 8, double-buffered
    switch (a) {
    case 0x2000: return 1;                          // WGL_NUMBER_PIXEL_FORMATS_ARB
    case 0x2001: case 0x2010: case 0x2011: case 0x20A9: return 1;   // draw to window, OpenGL, double buffer, sRGB capable
    case 0x2003: return 0x2027;                     // WGL_ACCELERATION_ARB: full
    case 0x2013: return 0x202B;                     // WGL_PIXEL_TYPE_ARB: RGBA
    case 0x2014: return 32;                         // color bits
    case 0x2015: case 0x2017: case 0x2019: case 0x201B: return 8;   // red, green, blue, alpha bits
    case 0x2022: return 24;                         // depth bits
    case 0x2023: return 8;                          // stencil bits
    default: return 0;
    }
}
static WINAPI BOOL wglGetPixelFormatAttribivARB(HANDLE dc, int format, int layer, uint32_t n, const int *attribs, int *values) {
    (void)dc; (void)format; (void)layer;
    for (uint32_t i = 0; i < n; i++) values[i] = pixel_attrib(attribs[i]);
    return 1;
}

static const vx_export g_wgl[] = {
    VX_EXPORT(wglCreateContextAttribsARB), VX_EXPORT(wglGetExtensionsStringARB), VX_EXPORT(wglGetExtensionsStringEXT),
    VX_EXPORT(wglSwapIntervalEXT), VX_EXPORT(wglGetSwapIntervalEXT), VX_EXPORT(wglChoosePixelFormatARB),
    VX_EXPORT(wglGetPixelFormatAttribivARB),
};
static void *gl_function(const char *name) {        // g_gl_functions is sorted by name
    size_t lo = 0, hi = NFUNCS;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(g_gl_functions[mid].name, name);
        if (!c) return *g_gl_functions[mid].real ? g_gl_functions[mid].thunk : NULL;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
static WINAPI void *wglGetProcAddress(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_wgl / sizeof g_wgl[0]; i++) if (!strcmp(g_wgl[i].name, name)) return g_wgl[i].fn;
    void *f = gl_function(name);
    if (!f && vx_trace) vx_log("OpenGL: wglGetProcAddress(%s): not available", name);
    return f;
}

// gdi32's pixel-format calls and SwapBuffers (gdi.c) come here.
int vx_gl_describe_pixel_format(void *pfd, uint32_t size) {
    if (pfd && size >= 40) {                        // PIXELFORMATDESCRIPTOR
        uint8_t *p = pfd;
        memset(p, 0, 40);
        *(uint16_t *)p = 40; *(uint16_t *)(p + 2) = 1;
        *(uint32_t *)(p + 4) = 0x4 | 0x20 | 0x1;     // PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER
        p[8] = 0;                                    // PFD_TYPE_RGBA
        p[9] = 32; p[10] = 8; p[11] = 16; p[12] = 8; p[13] = 8; p[14] = 8; p[15] = 0; p[16] = 8; p[17] = 24;
        p[23] = 24; p[24] = 8;                       // depth, stencil
    }
    return 1;                                        // (one format)
}
int vx_gl_swap(void) {
    if (!t_current) return 0;
    if (r_glFlush) r_glFlush();
    return 1;
}

static vx_export *g_exports;
void vx_opengl_init(void) {
    if (getenv("VXRT_OPENGL") && !strcmp(getenv("VXRT_OPENGL"), "0")) return;   // VXRT_OPENGL=0: none (desktop.c's stand-ins)
    void *gl = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY | RTLD_LOCAL);
    size_t n = 0;
    g_exports = calloc(NFUNCS + 16, sizeof *g_exports);
    g_exports[n++] = (vx_export)VX_EXPORT(wglCreateContext);
    g_exports[n++] = (vx_export)VX_EXPORT(wglDeleteContext);
    g_exports[n++] = (vx_export)VX_EXPORT(wglMakeCurrent);
    g_exports[n++] = (vx_export)VX_EXPORT(wglGetCurrentContext);
    g_exports[n++] = (vx_export)VX_EXPORT(wglGetCurrentDC);
    g_exports[n++] = (vx_export)VX_EXPORT(wglShareLists);
    g_exports[n++] = (vx_export)VX_EXPORT(wglGetProcAddress);
    for (size_t i = 0; i < NFUNCS; i++) {
        *g_gl_functions[i].real = gl ? dlsym(gl, g_gl_functions[i].name) : NULL;
        if (*g_gl_functions[i].real) g_exports[n++] = (vx_export){ g_gl_functions[i].name, g_gl_functions[i].thunk };
    }
    vx_register_module("opengl32.dll", g_exports, n);
}
