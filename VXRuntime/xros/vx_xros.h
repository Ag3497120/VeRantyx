// The visionOS build (VISIONOS_PORT.md) includes this before every source file (clang -include). It stands in for the
// macOS display, cursor and event calls the runtime makes, which visionOS does not have: the program's "display" is the
// app's window (its size and refresh set by src/xros_ui.m), there is no cursor to move or hide, and the pointer is
// where the last touch or pinch was.
#pragma once
#define VX_XROS 1
#ifndef __ASSEMBLER__
#include <stdint.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

extern int vx_xros_display_w, vx_xros_display_h, vx_xros_display_hz;    // (src/xros_ui.m)
extern double vx_xros_pointer_x, vx_xros_pointer_y, vx_xros_last_input;  // (pointer in display pixels; CFAbsoluteTime)

typedef uint32_t CGDirectDisplayID;
typedef struct vx_xros_display_mode *CGDisplayModeRef;   // never made: CGDisplayCopyDisplayMode answers NULL
typedef struct vx_xros_event *CGEventRef;
#define kCGDirectMainDisplay ((CGDirectDisplayID)1)
#define kCGEventSourceStateCombinedSessionState 0
#define kCGAnyInputEventType ((uint32_t)~0u)

static inline CGDirectDisplayID CGMainDisplayID(void) { return kCGDirectMainDisplay; }
static inline CGRect CGDisplayBounds(CGDirectDisplayID d) { (void)d; return CGRectMake(0, 0, vx_xros_display_w, vx_xros_display_h); }
static inline size_t CGDisplayPixelsWide(CGDirectDisplayID d) { (void)d; return (size_t)vx_xros_display_w; }
static inline size_t CGDisplayPixelsHigh(CGDirectDisplayID d) { (void)d; return (size_t)vx_xros_display_h; }
static inline CFArrayRef CGDisplayCopyAllDisplayModes(CGDirectDisplayID d, CFDictionaryRef o) { (void)d; (void)o; return NULL; }
static inline CGDisplayModeRef CGDisplayCopyDisplayMode(CGDirectDisplayID d) { (void)d; return NULL; }
// (callers that do not check for NULL get the window's size and rate)
static inline size_t CGDisplayModeGetWidth(CGDisplayModeRef m) { (void)m; return (size_t)vx_xros_display_w; }
static inline size_t CGDisplayModeGetHeight(CGDisplayModeRef m) { (void)m; return (size_t)vx_xros_display_h; }
static inline double CGDisplayModeGetRefreshRate(CGDisplayModeRef m) { (void)m; return vx_xros_display_hz; }
static inline void CGDisplayModeRelease(CGDisplayModeRef m) { (void)m; }
static inline CGError CGDisplayHideCursor(CGDirectDisplayID d) { (void)d; return kCGErrorSuccess; }
static inline CGError CGDisplayShowCursor(CGDirectDisplayID d) { (void)d; return kCGErrorSuccess; }
static inline CGError CGWarpMouseCursorPosition(CGPoint p) { vx_xros_pointer_x = p.x; vx_xros_pointer_y = p.y; return kCGErrorSuccess; }
static inline CGError CGAssociateMouseAndMouseCursorPosition(int on) { (void)on; return kCGErrorSuccess; }
static inline CGEventRef CGEventCreate(void *source) { (void)source; return (CGEventRef)CFDataCreate(NULL, NULL, 0); }   // (CFRelease'd)
static inline CGPoint CGEventGetLocation(CGEventRef e) { (void)e; return CGPointMake(vx_xros_pointer_x, vx_xros_pointer_y); }
static inline double CGEventSourceSecondsSinceLastEventType(int state, uint32_t type) {
    (void)state; (void)type;
    return vx_xros_last_input > 0 ? CFAbsoluteTimeGetCurrent() - vx_xros_last_input : 0;
}
// AudioToolbox: the output unit is RemoteIO there (src/audio.m, src/xaudio2.m)
#define kAudioUnitSubType_DefaultOutput kAudioUnitSubType_RemoteIO
#endif
