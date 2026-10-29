// visionOS (VISIONOS_PORT.md): what the files the app leaves out would have given the rest of the runtime.
//   menus.m, tray.m      the notification area and popup menus: the stand-ins in user32.c / desktop.c stay
//   clipboard.m          the Windows clipboard: desktop.c's stand-ins stay (the text one is in xros_ui.m)
//   opengl.c             no OpenGL on visionOS: opengl32.dll is not there, a program importing it does not load
//   directinput.m        no IOKit HID there: dinput8.dll keeps no devices (GameController would be the way)
//   vcpu.c, guest.c      no Hypervisor.framework there: no vCPU
//   vm_hooks.c           (the vCPU's interposed allocator): the host's malloc
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vx.h"
#include "../translator/vcpu.h"

void vx_tray_init(void) { }
void vx_tray_window_destroyed(HANDLE hwnd) { (void)hwnd; }
char *vx_tray_state(void) { return strdup("{\"ok\":true,\"icons\":[]}"); }
int vx_tray_click(int index, const char *how, char *why, size_t cap) { snprintf(why, cap, "no notification area on visionOS"); return 0; }
int vx_tray_snapshot(int index, const char *png, char *why, size_t cap) { snprintf(why, cap, "no notification area on visionOS"); return 0; }
char *vx_menu_state(void) { return strdup("{\"ok\":true,\"open\":false}"); }
int vx_menu_pick(const char *text, int wait_ms, char *why, size_t cap) { snprintf(why, cap, "no popup menus on visionOS"); return 0; }
void vx_clipboard_init(void) { }
void vx_opengl_init(void) { }
int vx_gl_describe_pixel_format(void *pfd, uint32_t size) { (void)pfd; (void)size; return 0; }
int vx_gl_swap(void) { return 0; }
void vx_directinput_init(void) { }

// The vCPU (translator/vcpu.c) and its guest code (translator/guest.c): never started (xe_new_vcpu answers NULL).
static __thread int t_depth;
int xv_depth(void) { return t_depth; }
void xv_set_depth(int depth) { t_depth = depth; }
int xv_interrupt(void) { return 0; }
void xv_unmap(uint64_t va, uint64_t len) { (void)va; (void)len; }
void xv_set_low(uint64_t base) { (void)base; }
void xv_set_interrupt_hook(void (*hook)(x86_ctx *c, const xv_regs *r)) { (void)hook; }
int xv_guest_defer(x86_ctx *c, const xv_defer *d) { (void)c; (void)d; return 1; }
#define G(name) int name(x86_ctx *c, const void *d) { (void)c; (void)d; return 1; }   // (1: the host's function instead)
G(xv_g_tls_get) G(xv_g_tls_set) G(xv_g_get_last_error) G(xv_g_set_last_error) G(xv_g_qpc) G(xv_g_qpf) G(xv_g_tick64) G(xv_g_tick)
G(xv_g_memcpy) G(xv_g_memmove) G(xv_g_memset) G(xv_g_memcmp) G(xv_g_strlen) G(xv_g_wcslen) G(xv_g_malloc) G(xv_g_calloc)
G(xv_g_free) G(xv_g_realloc) G(xv_g_msize) G(xv_g_heap_alloc) G(xv_g_heap_free) G(xv_g_heap_size) G(xv_g_heap_realloc)
G(xv_g_cs_enter) G(xv_g_cs_try) G(xv_g_cs_leave) G(xv_g_srw_acquire_exclusive) G(xv_g_srw_acquire_shared)
G(xv_g_srw_try_exclusive) G(xv_g_srw_try_shared) G(xv_g_srw_release_exclusive) G(xv_g_srw_release_shared)
G(xv_g_const) G(xv_g_local_alloc) G(xv_g_local_free)
#undef G

// src/vm_hooks.c's allocator: the host's malloc (no guest to share small blocks with).
void vxvm_set_hook(void (*hook)(uint64_t addr, uint64_t len)) { (void)hook; }
void *vxvm_heap(void) { return NULL; }
int vxvm_owns(const void *p) { (void)p; return 0; }
void *vxvm_alloc(size_t n) { return malloc(n ? n : 1); }
void *vxvm_zalloc(size_t n) { return calloc(1, n ? n : 1); }

#if VX_XROS_SIM
// The visionOS simulator has no MetalFX (src/upscale_core.m is left out there): no scaler.
#include "upscale_core.h"
int vxu_supported(void *device, int mode) { (void)device; (void)mode; return 0; }
int vxu_encode(void *command_buffer, int mode, const vxu_frame *f, char *why, size_t why_len, int *note) {
    (void)command_buffer; (void)mode; (void)f; (void)note;
    snprintf(why, why_len, "no MetalFX in the visionOS simulator");
    return 0;
}
#endif
