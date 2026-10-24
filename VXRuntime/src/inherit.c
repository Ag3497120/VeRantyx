// Handle inheritance for the handles that are descriptors (anonymous pipes and files: 0x10000 + descriptor). A child
// process here is another vxrt started with posix_spawn, so an inheritable handle is a descriptor without FD_CLOEXEC:
// the child finds it open under the same handle value, as a child on Windows finds an inherited handle. (Every other
// kind of handle reaches another process through DuplicateHandle and the handle servers, handlexfer.c.)
//
// - CreatePipe makes both ends inheritable when its SECURITY_ATTRIBUTES ask for it (bInheritHandle), and not otherwise.
// - SetHandleInformation with HANDLE_FLAG_INHERIT in the mask makes a handle inheritable or not; GetHandleInformation
//   says which it is (Microsoft's "Handle Inheritance" and the two functions' pages).
//
// Google Earth VR starts its crash handler this way once version.dll gives it its own version: CreatePipe with
// inheritable ends, the read end made non-inheritable with SetHandleInformation, the handler started (with a handle list
// in a STARTUPINFOEX) with the write end's value on its command line. The handler writes its pipe's name there and Earth
// waits for it. With every pipe end close-on-exec (CreatePipe ignored the attributes, SetHandleInformation did nothing)
// the handler's WriteFile found no such descriptor and Earth waited for ever, before it reached SteamVR.
//
// Not done: CreateProcess's bInheritHandles = FALSE and a PROC_THREAD_ATTRIBUTE_HANDLE_LIST do not narrow what a child
// inherits (every inheritable descriptor goes, as descriptors made by DuplicateHandle already did);
// HANDLE_FLAG_PROTECT_FROM_CLOSE is accepted and not kept. The standard descriptors 0-2 stay as they are: they are the
// runtime's own output too, and a child's standard handles are set when it starts (STARTF_USESTDHANDLES).
#include <fcntl.h>
#include <unistd.h>
#include "vx.h"

#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define HANDLE_FLAG_INHERIT 0x1

int vx_handle_fd(HANDLE h);                                          // kernel32.c

// SECURITY_ATTRIBUTES (x64): DWORD nLength, a pointer to the security descriptor at 8, BOOL bInheritHandle at 16.
static int wants_inherit(const void *sa) {
    if (!sa) return 0;
    return *(const DWORD *)sa >= 20 && *(const int32_t *)((const uint8_t *)sa + 16) != 0;
}
static void set_inherit(int fd, int inherit) {
    int f = fcntl(fd, F_GETFD);
    if (f >= 0) fcntl(fd, F_SETFD, inherit ? (f & ~FD_CLOEXEC) : (f | FD_CLOEXEC));
}

// Anonymous pipes: a Unix pipe; the handles are file handles (0x10000 + descriptor, as files.c makes them).
static WINAPI BOOL CreatePipe(HANDLE *rd, HANDLE *wr, void *sa, DWORD size) {
    (void)size;
    int fds[2];
    if (pipe(fds)) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    int inherit = wants_inherit(sa);
    set_inherit(fds[0], inherit); set_inherit(fds[1], inherit);
    *rd = (HANDLE)(intptr_t)(0x10000 + fds[0]); *wr = (HANDLE)(intptr_t)(0x10000 + fds[1]);
    return 1;
}

static WINAPI BOOL SetHandleInformation(HANDLE h, DWORD mask, DWORD flags) {
    int fd = vx_handle_fd(h);
    if (fd < 0) return 1;                                            // (not a descriptor: nothing is inherited here)
    if (fcntl(fd, F_GETFD) < 0) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    if ((mask & HANDLE_FLAG_INHERIT) && fd > 2) set_inherit(fd, (flags & HANDLE_FLAG_INHERIT) != 0);
    return 1;
}
static WINAPI BOOL GetHandleInformation(HANDLE h, DWORD *flags) {
    int fd = vx_handle_fd(h);
    DWORD out = 0;
    if (fd >= 0) {
        int f = fcntl(fd, F_GETFD);
        if (f < 0) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
        if (!(f & FD_CLOEXEC)) out |= HANDLE_FLAG_INHERIT;
    }
    if (flags) *flags = out;
    return 1;
}

static const vx_export kernel32[] = {
    VX_EXPORT(CreatePipe), VX_EXPORT(SetHandleInformation), VX_EXPORT(GetHandleInformation),
};
void vx_inherit_init(void) { vx_register_module("kernel32.dll", kernel32, sizeof kernel32 / sizeof kernel32[0]); }

#ifdef __aarch64__
#include "thunks/inherit.c.inc"
#endif
