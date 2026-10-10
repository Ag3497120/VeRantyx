// Alyx's scripting VM runs on a NATIVE LuaJIT (GC64, built for Darwin) instead of
// the one embedded in libvscript.so.
//
// The embedded LuaJIT is a non-GC64 build: 32-bit GC references, so its heap
// must live below 2 GB (Valve's allocator, lj_alloc_f_VALVE_OVERRIDEN, asks
// Plat_VirtualAlloc for exactly that). A Darwin arm64 process has a 4 GB
// __PAGEZERO that the kernel will not let a binary shrink (a 64 KB one is
// SIGKILLed at exec), so that VM cannot start here at all ("VM Did not
// start!"). libvscript exports the whole Lua C API and Valve's binding code
// calls it through its PLT, so every lua_* / luaL_* / luaJIT_* / luaopen_*
// name is bound to this build instead (the loader's preload hook, i.e.
// LD_PRELOAD semantics). LuaJIT is built with LUAJIT_NO_UNWIND so a Lua error
// never asks Darwin's unwinder to walk Linux-compiled guest frames.
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../src/LuaJIT/src/lua.h"
#include "../../../src/LuaJIT/src/lauxlib.h"
#include "../../../src/LuaJIT/src/luajit.h"
#include "../../../src/LuaJIT/src/lualib.h"
#include "kl_va.h"

// Valve passes its low-memory allocator; LuaJIT's own is correct here.
static lua_State *kl_lua_newstate(lua_Alloc f, void *ud) {
    (void)f; (void)ud;
    lua_State *L = luaL_newstate();
    if (getenv("KL_LUA_TRACE")) fprintf(stderr, "[lua] lua_newstate -> native GC64 LuaJIT state %p\n", (void *)L);
    // No JIT where the platform forbids it (visionOS), or on request.
    if (L && getenv("KL_LUA_NOJIT")) luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
    return L;
}

// Variadic entry points: the guest passes AAPCS64 varargs (kl_glibc_va.S).
// LuaJIT's formatter accepts %s %d %i %f %p %c %%, so each argument is pulled
// in that vocabulary and formatted here.
const char *klgh_lua_pushfstring(lua_State *L, const char *fmt, kl_va *va) {
    luaL_Buffer b; luaL_buffinit(L, &b);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { luaL_addchar(&b, *p); continue; }
        char c = *++p, tmp[64];
        switch (c) {
        case 's': { const char *s = (const char *)kl_va_gp(va); luaL_addstring(&b, s ? s : "(null)"); break; }
        case 'd': case 'i': snprintf(tmp, sizeof tmp, "%d", (int)kl_va_gp(va)); luaL_addstring(&b, tmp); break;
        case 'f': snprintf(tmp, sizeof tmp, "%.14g", kl_va_fp(va)); luaL_addstring(&b, tmp); break;
        case 'p': snprintf(tmp, sizeof tmp, "%p", (void *)kl_va_gp(va)); luaL_addstring(&b, tmp); break;
        case 'c': luaL_addchar(&b, (char)kl_va_gp(va)); break;
        case '%': luaL_addchar(&b, '%'); break;
        case 0: p--; break;
        default: luaL_addchar(&b, '%'); luaL_addchar(&b, c);
        }
    }
    luaL_pushresult(&b);
    return lua_tostring(L, -1);
}
int klgh_luaL_error(lua_State *L, const char *fmt, kl_va *va) {
    luaL_where(L, 1);
    klgh_lua_pushfstring(L, fmt, va);
    lua_concat(L, 2);
    return lua_error(L);
}
extern void klg_lua_pushfstring(void), klg_luaL_error(void);

static int is_lua_name(const char *n) {
    return !strncmp(n, "lua_", 4) || !strncmp(n, "luaL_", 5) || !strncmp(n, "luaJIT_", 7) || !strncmp(n, "luaopen_", 8);
}

// ---- V8 without a JIT ------------------------------------------------------
// Panorama's V8 (11.9) compiles JavaScript to machine code at run time, which
// visionOS does not allow a third-party app to execute. libpanorama sets its V8
// flags through v8::V8::SetFlagsFromString("--stack-size=384") before
// V8::Initialize; this wrapper, bound ahead of libv8's own definition, adds
// --jitless (interpreter only, no executable memory, no WebAssembly) right after
// Valve's flags. On by default; KL_V8_JIT=1 leaves V8's JIT alone (Mac A/B).
#include "../klepton.h"
#define V8_SET_FLAGS "_ZN2v82V818SetFlagsFromStringEPKcm"
typedef void (*v8_set_flags_fn)(const char *, size_t);
static void kl_v8_set_flags(const char *flags, size_t len) {
    static v8_set_flags_fn real;
    if (!real) {
        kl_image *v8 = kl_find_image("libv8.so");
        if (v8) real = (v8_set_flags_fn)kl_sym(v8, V8_SET_FLAGS);
    }
    if (!real) { fprintf(stderr, "[v8] SetFlagsFromString: libv8 not found; flags '%.*s' dropped\n", (int)len, flags); return; }
    real(flags, len);
    if (!kl_env_on("KL_V8_JIT", 0)) {
        static const char jl[] = "--jitless";
        real(jl, sizeof jl - 1);
        static int said;
        if (!said++) fprintf(stderr, "[v8] '%.*s' + --jitless\n", (int)len, flags);
    }
}

// KL_LUA_INJECT="<chunk-name substring>=<file>": that chunk is loaded from
// the file instead. A test hook (a retail Source 2 build reads scripts only
// from its VPKs): e.g. game/gameinit.lua, which ships empty, can then print
// the player's origin for a scripted-input run. KL_LUA_TRACE lists chunks.
static int kl_lua_inject(lua_State *L, const char *buff, size_t sz, const char *name, const char *mode) {
    const char *spec = getenv("KL_LUA_INJECT");
    if (getenv("KL_LUA_TRACE")) fprintf(stderr, "[lua] load chunk %s (%zu bytes)\n", name ? name : "?", sz);
    const char *eq = spec ? strchr(spec, '=') : NULL;
    if (eq && name && eq > spec) {
        char want[256];
        size_t n = (size_t)(eq - spec) < sizeof want - 1 ? (size_t)(eq - spec) : sizeof want - 1;
        memcpy(want, spec, n); want[n] = 0;
        if (strstr(name, want)) {
            FILE *f = fopen(eq + 1, "rb");
            if (f) {
                // the whole file, whatever its size (a fixed 64 KB buffer cut longer scripts off mid-string)
                fseek(f, 0, SEEK_END);
                long fl = ftell(f);
                fseek(f, 0, SEEK_SET);
                char *body = fl > 0 ? malloc((size_t)fl) : NULL;
                size_t len = body ? fread(body, 1, (size_t)fl, f) : 0;
                fclose(f);
                fprintf(stderr, "[lua] chunk %s replaced by %s (%zu bytes)\n", name, eq + 1, len);
                int r = luaL_loadbufferx(L, body ? body : "", len, name, mode);
                free(body);  // the chunk is compiled: Lua keeps no reference to the source buffer
                return r;
            }
        }
    }
    return luaL_loadbufferx(L, buff, sz, name, mode);
}
static int kl_lua_loadbufferx(lua_State *L, const char *b, size_t sz, const char *name, const char *mode) {
    return kl_lua_inject(L, b, sz, name, mode);
}
static int kl_lua_loadbuffer(lua_State *L, const char *b, size_t sz, const char *name) {
    return kl_lua_inject(L, b, sz, name, NULL);
}

void *kl_lua_preload(const char *name) {
    if (!strcmp(name, V8_SET_FLAGS)) return (void *)kl_v8_set_flags;
    if (!is_lua_name(name)) return NULL;
    if (getenv("KL_LUA_EMBEDDED")) return NULL;          // A/B: the guest's own LuaJIT
    if (!strcmp(name, "lua_newstate")) return (void *)kl_lua_newstate;
    if (getenv("KL_LUA_INJECT") || getenv("KL_LUA_TRACE")) {
        if (!strcmp(name, "luaL_loadbufferx")) return (void *)kl_lua_loadbufferx;
        if (!strcmp(name, "luaL_loadbuffer")) return (void *)kl_lua_loadbuffer;
    }
    if (!strcmp(name, "lua_pushfstring")) return (void *)klg_lua_pushfstring;
    if (!strcmp(name, "luaL_error")) return (void *)klg_luaL_error;
    if (!strcmp(name, "lua_pushvfstring") || !strcmp(name, "luaL_verror")) {
        fprintf(stderr, "[lua] %s (va_list) not bridged\n", name);
        return NULL;
    }
    // By address, from a table compiled against LuaJIT's headers: the app links
    // LuaJIT statically without exporting it, so dlsym(RTLD_DEFAULT) found
    // nothing there and the guest silently kept its embedded non-GC64 LuaJIT —
    // which crashes in lua_pcall on the headset.
    static const struct { const char *n; void *p; } tab[] = {
#define KL_LUA_FN(fn) { #fn, (void *)fn },
#include "kl_lua_table.h"
#undef KL_LUA_FN
    };
    void *p = NULL;
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0] && !p; i++)
        if (!strcmp(tab[i].n, name)) p = tab[i].p;
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    if (!p && getenv("KL_LUA_TRACE")) fprintf(stderr, "[lua] native LuaJIT has no %s\n", name);
    return p;
}
