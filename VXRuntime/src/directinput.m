// DirectInput 8 joystick support backed by macOS HID. The first slice exposes attached HID joysticks,
// gamepads and multi-axis controllers, then maps their axes, buttons and POV hats into the caller's data format.
#import <IOKit/hid/IOHIDManager.h>
#import <IOKit/hid/IOHIDKeys.h>
#include "vx.h"
#include "directinput_buffer.h"
#include "directinput_format.h"
#include <stdlib.h>
#include <string.h>

typedef int32_t HRESULT;
typedef int32_t BOOL32;
typedef uint32_t ULONG;
typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
static int guid_equal(const GUID *a, const GUID *b);

enum {
    VX_DI_S_OK = 0, VX_DI_S_FALSE = 1,
    VX_DI_E_NOINTERFACE = (int32_t)0x80004002u,
    VX_DI_E_POINTER = (int32_t)0x80004003u,
    VX_DI_E_OUTOFMEMORY = (int32_t)0x8007000eu,
    VX_DI_E_INVALIDARG = (int32_t)0x80070057u,
    VX_DI_E_NOTIMPL = (int32_t)0x80004001u,
    DIERR_NOTFOUND = (int32_t)0x80070002u,
    DIERR_NOTACQUIRED = (int32_t)0x8007000cu,
    DIERR_INPUTLOST = (int32_t)0x8007001eu,
    DIERR_ACQUIRED = (int32_t)0x800700aau,
    DIERR_OTHERAPPHASPRIO = (int32_t)0x80070005u,
    DIERR_NOTBUFFERED = (int32_t)0x80040207u,
    DIERR_NOAGGREGATION = (int32_t)0x80040110u,
    DIERR_UNSUPPORTED = VX_DI_E_NOTIMPL,
    DI_BUFFEROVERFLOW = VX_DI_S_FALSE,
    DI_POLLEDDEVICE = 2,
    DIDEVTYPE_JOYSTICK = 4,
    DIDEVTYPE_HID = 0x00010000,
    DI8DEVCLASS_ALL = 0,
    DI8DEVCLASS_GAMECTRL = 4,
    DI8DEVTYPE_JOYSTICK = 0x14,
    DI8DEVTYPE_GAMEPAD = 0x15,
    DI8DEVTYPE_FLIGHT = 0x17,
    DIEDFL_FORCEFEEDBACK = 0x00000100,
    DIDFT_ABSAXIS = 0x00000002,
    DIDFT_PSHBUTTON = 0x00000004,
    DIDFT_TGLBUTTON = 0x00000008,
    DIDFT_BUTTON = 0x0000000c,
    DIDFT_POV = 0x00000010,
    DIDFT_OPTIONAL = 0x80000000u,
    DIPH_DEVICE = 0,
    DIPH_BYOFFSET = 1,
    DIPH_BYID = 2,
    DIPROP_BUFFERSIZE_ID = 1,
    DIPROP_RANGE_ID = 4,
    DIGDD_PEEK = 1,
    DI_MAX_BUFFER_SIZE = 65536,
};

typedef struct { DWORD dwSize; GUID guidInstance, guidProduct; DWORD dwDevType; WCHAR instance_name[260], product_name[260]; GUID guidFFDriver; uint16_t usagePage, usage; } dideviceinstancew;
typedef struct { DWORD dwSize; DWORD dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs; DWORD dwFFSamplePeriod, dwFFMinTimeResolution, dwFirmwareRevision, dwHardwareRevision, dwFFDriverVersion; } didevcaps;
typedef struct { DWORD dwSize; GUID guidType; DWORD dwOfs, dwType, dwFlags; WCHAR name[260]; DWORD ffMaxForce, ffForceResolution; uint16_t collection, designator, usagePage, usage; DWORD dimension; uint16_t exponent, reportId; } dideviceobjectinstancew;
// the ANSI interfaces (IDirectInput8A, IDirectInputDevice8A: what a program built without UNICODE asks for) differ only
// in these two structures' names being char arrays
typedef struct { DWORD dwSize; GUID guidInstance, guidProduct; DWORD dwDevType; char instance_name[260], product_name[260]; GUID guidFFDriver; uint16_t usagePage, usage; } dideviceinstancea;
typedef struct { DWORD dwSize; GUID guidType; DWORD dwOfs, dwType, dwFlags; char name[260]; DWORD ffMaxForce, ffForceResolution; uint16_t collection, designator, usagePage, usage; DWORD dimension; uint16_t exponent, reportId; } dideviceobjectinstancea;
typedef struct { DWORD dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs; const void *objects; } didataformat;
typedef struct { const GUID *guid; DWORD offset, type, flags; } diobjectdataformat;
typedef struct { DWORD dwSize, dwHeaderSize, dwObj, dwHow; } dipropheader;
typedef struct { dipropheader header; DWORD data; } dipropdword;
typedef struct { dipropheader header; LONG min, max; } diproprange;
typedef vx_di_buffer_event diobjectdata;
typedef struct { DWORD offset, type, flags; GUID guid; BOOL32 has_guid; uint32_t source_index, width, category; } diobject;

typedef struct {
    IOHIDElementRef element;
    uint32_t page, usage;
    LONG logical_min, logical_max;
    WCHAR name[260];
} hid_object;

typedef struct {
    IOHIDDeviceRef hid;
    GUID instance, product;
    DWORD type;
    uint16_t primary_page, primary_usage;
    WCHAR name[260];
} hid_device;

typedef struct di_input di_input;
typedef struct di_device di_device;
struct di_input { const void **vtable; uint32_t refs; DWORD version; BOOL32 ansi; };
struct di_device {
    const void **vtable;
    uint32_t refs;
    hid_device source;
    hid_object *objects;
    size_t object_count;
    diobject *format;
    DWORD format_count, format_size;
    LONG range_min[8], range_max[8];
    uint32_t *last_values;
    uint8_t *last_valid;
    DWORD buffer_size;
    HANDLE event;
    vx_di_buffer event_buffer;
    BOOL32 opened, acquired, format_set;
    BOOL32 ansi;                                       // created through IDirectInput8A: char names in instances and objects
};

static const GUID IID_IUnknown = { 0, 0, 0, { 0xc0,0,0,0,0,0,0,0x46 } };
static const GUID IID_IDirectInput8W = { 0xbf798031,0x483a,0x4da2,{0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00} };
static const GUID IID_IDirectInput7W = { 0x9a4cb685,0x236d,0x11d3,{0x8e,0x9d,0,0xc0,0x4f,0x68,0x44,0xae} };
static const GUID IID_IDirectInput2W = { 0x5944e663,0xaa8a,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID IID_IDirectInputW = { 0x89521361,0xaa8a,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID IID_IDirectInput8A = { 0xbf798030,0x483a,0x4da2,{0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00} };
static const GUID IID_IDirectInput7A = { 0x9a4cb684,0x236d,0x11d3,{0x8e,0x9d,0,0xc0,0x4f,0x68,0x44,0xae} };
static const GUID IID_IDirectInput2A = { 0x5944e662,0xaa8a,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID IID_IDirectInputA = { 0x89521360,0xaa8a,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID IID_IDirectInputDevice8W = { 0x54d41081,0xdc15,0x4833,{0xa4,0x1b,0x74,0x8f,0x73,0xa3,0x81,0x79} };
static const GUID IID_IDirectInputDevice7W = { 0x57d7c6bd,0x2356,0x11d3,{0x8e,0x9d,0,0xc0,0x4f,0x68,0x44,0xae} };
static const GUID IID_IDirectInputDevice2W = { 0x5944e683,0xc92e,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID IID_IDirectInputDeviceW = { 0x5944e681,0xc92e,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID IID_IDirectInputDevice8A = { 0x54d41080,0xdc15,0x4833,{0xa4,0x1b,0x74,0x8f,0x73,0xa3,0x81,0x79} };
static const GUID IID_IDirectInputDevice7A = { 0x57d7c6bc,0x2356,0x11d3,{0x8e,0x9d,0,0xc0,0x4f,0x68,0x44,0xae} };
static const GUID IID_IDirectInputDevice2A = { 0x5944e682,0xc92e,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID IID_IDirectInputDeviceA = { 0x5944e680,0xc92e,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static BOOL32 input_iid_ansi(const GUID *iid) {
    return guid_equal(iid, &IID_IDirectInput8A) || guid_equal(iid, &IID_IDirectInput7A) || guid_equal(iid, &IID_IDirectInput2A) || guid_equal(iid, &IID_IDirectInputA);
}
static BOOL32 device_iid_ansi(const GUID *iid) {
    return guid_equal(iid, &IID_IDirectInputDevice8A) || guid_equal(iid, &IID_IDirectInputDevice7A) || guid_equal(iid, &IID_IDirectInputDevice2A) || guid_equal(iid, &IID_IDirectInputDeviceA);
}
static const GUID GUID_Joystick = { 0x6f1d2b70,0xd5a0,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID GUID_XAxis = { 0xa36d02e0,0xc9f3,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID GUID_YAxis = { 0xa36d02e1,0xc9f3,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID GUID_ZAxis = { 0xa36d02e2,0xc9f3,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID GUID_RzAxis = { 0xa36d02e3,0xc9f3,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID GUID_Slider = { 0xa36d02e4,0xc9f3,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID GUID_RxAxis = { 0xa36d02f4,0xc9f3,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID GUID_RyAxis = { 0xa36d02f5,0xc9f3,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID GUID_Button = { 0xa36d02f0,0xc9f3,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };
static const GUID GUID_POV = { 0xa36d02f2,0xc9f3,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0,0} };

static int guid_equal(const GUID *a, const GUID *b) { return a && b && !memcmp(a, b, sizeof *a); }
static uint32_t property_id(const GUID *property) {
    uintptr_t value = (uintptr_t)property;
    if (value <= UINT16_MAX) return (uint32_t)value;
    return property ? property->Data1 : 0;
}
static size_t wide_len(const WCHAR *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static uint32_t cf_u32(CFTypeRef value) {
    int32_t n = 0;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &n);
    return (uint32_t)n;
}
static void cf_string_to_wide(CFStringRef value, WCHAR *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return;
    CFIndex n = CFStringGetLength(value), take = n < (CFIndex)(cap - 1) ? n : (CFIndex)(cap - 1);
    if (take > 0) CFStringGetCharacters(value, CFRangeMake(0, take), (UniChar *)out);
    out[take] = 0;
}
static GUID device_guid(uint32_t vid, uint32_t pid, uint32_t location, uint32_t page, uint32_t usage) {
    GUID g = { (vid << 16) | (pid & 0xffff), (uint16_t)location, (uint16_t)usage,
               { (uint8_t)page, (uint8_t)(page >> 8), (uint8_t)(location >> 16), (uint8_t)(location >> 24),
                 (uint8_t)(vid >> 8), (uint8_t)vid, (uint8_t)(pid >> 8), (uint8_t)pid } };
    return g;
}

static BOOL32 make_hid_info(IOHIDDeviceRef hid, hid_device *out) {
    if (!hid || !out) return 0;
    memset(out, 0, sizeof *out);
    uint32_t page = cf_u32(IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDPrimaryUsagePageKey)));
    uint32_t usage = cf_u32(IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDPrimaryUsageKey)));
    if (page != 1 || (usage != 4 && usage != 5 && usage != 8)) return 0;
    uint32_t vid = cf_u32(IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDVendorIDKey)));
    uint32_t pid = cf_u32(IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductIDKey)));
    uint32_t location = cf_u32(IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDLocationIDKey)));
    out->hid = (IOHIDDeviceRef)CFRetain(hid);
    out->instance = device_guid(vid, pid, location, page, usage);
    out->product = device_guid(vid, pid, 0, page, usage);
    out->type = DIDEVTYPE_HID | DIDEVTYPE_JOYSTICK;
    out->primary_page = (uint16_t)page;
    out->primary_usage = (uint16_t)usage;
    cf_string_to_wide(IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDProductKey)), out->name, 260);
    if (!out->name[0]) {
        static const WCHAR fallback[] = { 'H','I','D',' ','J','o','y','s','t','i','c','k',0 };
        memcpy(out->name, fallback, sizeof fallback);
    }
    return 1;
}

static hid_device *snapshot_devices(size_t *count) {
    if (count) *count = 0;
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!manager) return NULL;
    IOHIDManagerSetDeviceMatching(manager, NULL);
    (void)IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);
    if (!devices) return NULL;
    CFIndex n = CFSetGetCount(devices);
    IOHIDDeviceRef *values = n > 0 ? calloc((size_t)n, sizeof *values) : NULL;
    if (n > 0 && !values) { CFRelease(devices); return NULL; }
    if (n > 0) CFSetGetValues(devices, (const void **)values);
    hid_device *result = n > 0 ? calloc((size_t)n, sizeof *result) : NULL;
    if (n > 0 && !result) { free(values); CFRelease(devices); return NULL; }
    size_t used = 0;
    for (CFIndex i = 0; i < n; i++) {
        hid_device info;
        if (make_hid_info(values[i], &info)) result[used++] = info;
    }
    free(values);
    CFRelease(devices);
    if (!used) { free(result); return NULL; }
    if (count) *count = used;
    return result;
}
static void release_snapshot(hid_device *devices, size_t count) {
    if (!devices) return;
    for (size_t i = 0; i < count; i++) if (devices[i].hid) CFRelease(devices[i].hid);
    free(devices);
}
static void fill_instance(const hid_device *device, dideviceinstancew *out) {
    memset(out, 0, sizeof *out);
    out->dwSize = sizeof *out;
    out->guidInstance = device->instance;
    out->guidProduct = device->product;
    out->dwDevType = device->type;
    memcpy(out->instance_name, device->name, sizeof out->instance_name);
    memcpy(out->product_name, device->name, sizeof out->product_name);
    out->usagePage = device->primary_page;
    out->usage = device->primary_usage;
}

static void narrow_name(const WCHAR *wide, char *out, size_t cap) {
    size_t i = 0;
    for (; i + 1 < cap && wide[i]; i++) out[i] = wide[i] < 128 ? (char)wide[i] : '?';
    out[i] = 0;
}
static void instance_to_ansi(const dideviceinstancew *w, dideviceinstancea *a) {
    memset(a, 0, sizeof *a);
    a->dwSize = sizeof *a; a->guidInstance = w->guidInstance; a->guidProduct = w->guidProduct; a->dwDevType = w->dwDevType;
    narrow_name(w->instance_name, a->instance_name, sizeof a->instance_name);
    narrow_name(w->product_name, a->product_name, sizeof a->product_name);
    a->guidFFDriver = w->guidFFDriver; a->usagePage = w->usagePage; a->usage = w->usage;
}
static void object_to_ansi(const dideviceobjectinstancew *w, dideviceobjectinstancea *a) {
    memset(a, 0, sizeof *a);
    a->dwSize = sizeof *a; a->guidType = w->guidType; a->dwOfs = w->dwOfs; a->dwType = w->dwType; a->dwFlags = w->dwFlags;
    narrow_name(w->name, a->name, sizeof a->name);
    a->ffMaxForce = w->ffMaxForce; a->ffForceResolution = w->ffForceResolution; a->collection = w->collection; a->designator = w->designator;
    a->usagePage = w->usagePage; a->usage = w->usage; a->dimension = w->dimension; a->exponent = w->exponent; a->reportId = w->reportId;
}
typedef BOOL32 (WINAPI *enum_devices_cb)(const dideviceinstancew *, void *);
typedef BOOL32 (WINAPI *enum_objects_cb)(const dideviceobjectinstancew *, void *);
static HRESULT WINAPI di_input_qi(di_input *, const GUID *, void **);
static ULONG WINAPI di_input_addref(di_input *);
static ULONG WINAPI di_input_release(di_input *);
static HRESULT WINAPI di_input_create_device(di_input *, const GUID *, di_device **, void *);
static HRESULT WINAPI di_input_enum_devices(di_input *, DWORD, enum_devices_cb, void *, DWORD);
static HRESULT WINAPI di_input_get_device_status(di_input *, const GUID *);
static HRESULT WINAPI di_input_run_control_panel(di_input *, void *, DWORD);
static HRESULT WINAPI di_input_initialize(di_input *, void *, DWORD);
static HRESULT WINAPI di_input_find_device(di_input *, const GUID *, const WCHAR *, GUID *);
static HRESULT WINAPI di_input_unimplemented(void);
static const void *g_input_vtable[11] = {
    di_input_qi, di_input_addref, di_input_release, di_input_create_device, di_input_enum_devices,
    di_input_get_device_status, di_input_run_control_panel, di_input_initialize, di_input_find_device,
    di_input_unimplemented, di_input_unimplemented
};

static HRESULT WINAPI di_dev_qi(di_device *, const GUID *, void **);
static ULONG WINAPI di_dev_addref(di_device *);
static ULONG WINAPI di_dev_release(di_device *);
static HRESULT WINAPI di_dev_caps(di_device *, didevcaps *);
static HRESULT WINAPI di_dev_enum_objects(di_device *, enum_objects_cb, void *, DWORD);
static HRESULT WINAPI di_dev_get_property(di_device *, const GUID *, dipropheader *);
static HRESULT WINAPI di_dev_set_property(di_device *, const GUID *, const dipropheader *);
static HRESULT WINAPI di_dev_acquire(di_device *);
static HRESULT WINAPI di_dev_unacquire(di_device *);
static HRESULT WINAPI di_dev_get_state(di_device *, DWORD, void *);
static HRESULT WINAPI di_dev_get_data(di_device *, DWORD, void *, DWORD *, DWORD);
static HRESULT WINAPI di_dev_set_format(di_device *, const didataformat *);
static HRESULT WINAPI di_dev_set_event(di_device *, HANDLE);
static HRESULT WINAPI di_dev_set_coop(di_device *, void *, DWORD);
static HRESULT WINAPI di_dev_get_object_info(di_device *, dideviceobjectinstancew *, DWORD, DWORD);
static HRESULT WINAPI di_dev_get_info(di_device *, dideviceinstancew *);
static HRESULT WINAPI di_dev_run_control_panel(di_device *, void *, DWORD);
static HRESULT WINAPI di_dev_initialize(di_device *, void *, DWORD, const GUID *);
static HRESULT WINAPI di_dev_poll(di_device *);
static HRESULT WINAPI di_dev_unimplemented(void);
static const void *g_device_vtable[32] = {
    di_dev_qi, di_dev_addref, di_dev_release, di_dev_caps, di_dev_enum_objects, di_dev_get_property,
    di_dev_set_property, di_dev_acquire, di_dev_unacquire, di_dev_get_state, di_dev_get_data,
    di_dev_set_format, di_dev_set_event, di_dev_set_coop, di_dev_get_object_info, di_dev_get_info,
    di_dev_run_control_panel, di_dev_initialize,
    di_dev_unimplemented, di_dev_unimplemented, di_dev_unimplemented, di_dev_unimplemented,
    di_dev_unimplemented, di_dev_unimplemented, di_dev_unimplemented, di_dev_poll,
    di_dev_unimplemented, di_dev_unimplemented, di_dev_unimplemented, di_dev_unimplemented,
    di_dev_unimplemented, di_dev_unimplemented
};

static HRESULT WINAPI di_input_qi(di_input *self, const GUID *iid, void **out) {
    if (!out) return VX_DI_E_POINTER;
    *out = NULL;
    if (!iid) return VX_DI_E_INVALIDARG;
    if (guid_equal(iid, &IID_IUnknown) || guid_equal(iid, &IID_IDirectInput8W) ||
        guid_equal(iid, &IID_IDirectInput7W) || guid_equal(iid, &IID_IDirectInput2W) ||
        guid_equal(iid, &IID_IDirectInputW) || input_iid_ansi(iid)) {
        *out = self; di_input_addref(self); return VX_DI_S_OK;
    }
    return VX_DI_E_NOINTERFACE;
}
static ULONG WINAPI di_input_addref(di_input *self) { return __atomic_add_fetch(&self->refs, 1, __ATOMIC_RELAXED); }
static ULONG WINAPI di_input_release(di_input *self) {
    ULONG refs = __atomic_sub_fetch(&self->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) free(self);
    return refs;
}
static HRESULT WINAPI di_input_create_device(di_input *self, const GUID *guid, di_device **out, void *outer) {
    if (!guid || !out) return VX_DI_E_POINTER;
    *out = NULL;
    if (outer) return DIERR_NOAGGREGATION;
    size_t count = 0;
    hid_device *devices = snapshot_devices(&count);
    hid_device *found = NULL;
    for (size_t i = 0; i < count; i++) if (guid_equal(guid, &devices[i].instance)) { found = &devices[i]; break; }
    if (!found) { release_snapshot(devices, count); return DIERR_NOTFOUND; }
    di_device *device = calloc(1, sizeof *device);
    if (!device) { release_snapshot(devices, count); return VX_DI_E_OUTOFMEMORY; }
    device->vtable = g_device_vtable;
    device->refs = 1;
    device->ansi = self->ansi;
    device->source = *found;
    found->hid = NULL;
    vx_di_buffer_init(&device->event_buffer);
    device->range_min[0] = device->range_min[1] = device->range_min[2] = device->range_min[3] = 0;
    device->range_min[4] = device->range_min[5] = device->range_min[6] = device->range_min[7] = 0;
    for (int i = 0; i < 8; i++) device->range_max[i] = 65535;
    release_snapshot(devices, count);
    *out = device;
    return VX_DI_S_OK;
}
static BOOL32 acceptable_type(DWORD requested, DWORD actual) {
    if (!requested || requested == DI8DEVCLASS_ALL || requested == DI8DEVCLASS_GAMECTRL) return 1;
    return requested == DIDEVTYPE_JOYSTICK || requested == DI8DEVTYPE_JOYSTICK ||
           requested == DI8DEVTYPE_GAMEPAD || requested == DI8DEVTYPE_FLIGHT || requested == actual;
}
static HRESULT WINAPI di_input_enum_devices(di_input *self, DWORD type, enum_devices_cb callback, void *context, DWORD flags) {
    (void)self;
    if (!callback) return VX_DI_E_INVALIDARG;
    if (flags & DIEDFL_FORCEFEEDBACK) return VX_DI_S_OK;
    size_t count = 0;
    hid_device *devices = snapshot_devices(&count);
    for (size_t i = 0; i < count; i++) {
        if (!acceptable_type(type, devices[i].type)) continue;
        dideviceinstancew instance;
        fill_instance(&devices[i], &instance);
        if (self->ansi) {
            dideviceinstancea ansi;
            instance_to_ansi(&instance, &ansi);
            if (!((BOOL32 (WINAPI *)(const dideviceinstancea *, void *))callback)(&ansi, context)) break;
        } else if (!callback(&instance, context)) break;
    }
    release_snapshot(devices, count);
    return VX_DI_S_OK;
}
static HRESULT WINAPI di_input_get_device_status(di_input *self, const GUID *guid) {
    (void)self;
    if (!guid) return VX_DI_E_INVALIDARG;
    size_t count = 0;
    hid_device *devices = snapshot_devices(&count);
    HRESULT result = DIERR_NOTFOUND;
    for (size_t i = 0; i < count; i++) if (guid_equal(guid, &devices[i].instance)) { result = VX_DI_S_OK; break; }
    release_snapshot(devices, count);
    return result;
}
static HRESULT WINAPI di_input_run_control_panel(di_input *self, void *window, DWORD flags) {
    (void)self; (void)window; (void)flags; return DIERR_UNSUPPORTED;
}
static HRESULT WINAPI di_input_initialize(di_input *self, void *instance, DWORD version) {
    (void)self; (void)instance;
    return version >= 0x0700 && version <= 0x0800 ? VX_DI_S_OK : VX_DI_E_INVALIDARG;
}
static HRESULT WINAPI di_input_find_device(di_input *self, const GUID *class_guid, const WCHAR *name, GUID *out) {
    (void)self; (void)class_guid; (void)name; (void)out; return DIERR_NOTFOUND;
}
static HRESULT WINAPI di_input_unimplemented(void) { return DIERR_UNSUPPORTED; }

static HRESULT WINAPI di_dev_qi(di_device *self, const GUID *iid, void **out) {
    if (!out) return VX_DI_E_POINTER;
    *out = NULL;
    if (!iid) return VX_DI_E_INVALIDARG;
    if (guid_equal(iid, &IID_IUnknown) || guid_equal(iid, &IID_IDirectInputDevice8W) ||
        guid_equal(iid, &IID_IDirectInputDevice7W) || guid_equal(iid, &IID_IDirectInputDevice2W) ||
        guid_equal(iid, &IID_IDirectInputDeviceW) || device_iid_ansi(iid)) {
        *out = self; di_dev_addref(self); return VX_DI_S_OK;
    }
    return VX_DI_E_NOINTERFACE;
}
static ULONG WINAPI di_dev_addref(di_device *self) { return __atomic_add_fetch(&self->refs, 1, __ATOMIC_RELAXED); }
static ULONG WINAPI di_dev_release(di_device *self) {
    ULONG refs = __atomic_sub_fetch(&self->refs, 1, __ATOMIC_ACQ_REL);
    if (!refs) {
        if (self->opened) IOHIDDeviceClose(self->source.hid, kIOHIDOptionsTypeNone);
        if (self->source.hid) CFRelease(self->source.hid);
        for (size_t i = 0; i < self->object_count; i++) if (self->objects[i].element) CFRelease(self->objects[i].element);
        vx_di_buffer_destroy(&self->event_buffer);
        free(self->objects); free(self->format); free(self->last_values); free(self->last_valid); free(self);
    }
    return refs;
}
static BOOL32 load_objects(di_device *self) {
    if (self->objects) return 1;
    CFArrayRef elements = IOHIDDeviceCopyMatchingElements(self->source.hid, NULL, kIOHIDOptionsTypeNone);
    if (!elements) return 0;
    CFIndex count = CFArrayGetCount(elements);
    hid_object *objects = count ? calloc((size_t)count, sizeof *objects) : NULL;
    if (count && !objects) { CFRelease(elements); return 0; }
    size_t used = 0;
    for (CFIndex i = 0; i < count; i++) {
        IOHIDElementRef e = (IOHIDElementRef)CFArrayGetValueAtIndex(elements, i);
        IOHIDElementType type = IOHIDElementGetType(e);
        uint32_t page = IOHIDElementGetUsagePage(e), usage = IOHIDElementGetUsage(e);
        BOOL32 axis = page == 1 && usage >= 0x30 && usage <= 0x38;
        BOOL32 pov = page == 1 && usage == 0x39;
        BOOL32 button = page == 9 && usage > 0;
        if (!axis && !pov && !button) continue;
        if (type != kIOHIDElementTypeInput_Misc && type != kIOHIDElementTypeInput_Button &&
            type != kIOHIDElementTypeInput_Axis) continue;
        hid_object *obj = &objects[used++];
        obj->element = (IOHIDElementRef)CFRetain(e);
        obj->page = page; obj->usage = usage;
        obj->logical_min = (LONG)IOHIDElementGetLogicalMin(e);
        obj->logical_max = (LONG)IOHIDElementGetLogicalMax(e);
        cf_string_to_wide(IOHIDElementGetName(e), obj->name, 260);
    }
    CFRelease(elements);
    self->objects = objects;
    self->object_count = used;
    return 1;
}
static uint32_t count_objects(di_device *self, int category) {
    if (!load_objects(self)) return 0;
    uint32_t count = 0;
    for (size_t i = 0; i < self->object_count; i++) {
        hid_object *o = &self->objects[i];
        if ((category == 0 && o->page == 1 && o->usage >= 0x30 && o->usage <= 0x38) ||
            (category == 1 && o->page == 9) || (category == 2 && o->page == 1 && o->usage == 0x39)) count++;
    }
    return count;
}
static HRESULT WINAPI di_dev_caps(di_device *self, didevcaps *caps) {
    if (!caps || caps->dwSize < sizeof *caps) return VX_DI_E_INVALIDARG;
    DWORD size = caps->dwSize;
    memset(caps, 0, sizeof *caps);
    caps->dwSize = size;
    caps->dwFlags = 1 /* DIDC_ATTACHED */ | 2 /* DIDC_POLLEDDEVICE */;
    caps->dwDevType = self->source.type;
    caps->dwAxes = count_objects(self, 0);
    caps->dwButtons = count_objects(self, 1);
    caps->dwPOVs = count_objects(self, 2);
    return VX_DI_S_OK;
}
static GUID object_guid(const hid_object *object, uint32_t instance) {
    if (object->page == 9) return GUID_Button;
    if (object->page == 1 && object->usage == 0x39) return GUID_POV;
    switch (object->usage) {
    case 0x30: return GUID_XAxis; case 0x31: return GUID_YAxis; case 0x32: return GUID_ZAxis;
    case 0x33: return GUID_RxAxis; case 0x34: return GUID_RyAxis; case 0x35: return GUID_RzAxis;
    case 0x36: case 0x37: case 0x38: (void)instance; return GUID_Slider;
    default: return GUID_Joystick;
    }
}
static uint32_t object_instance(const hid_object *object, uint32_t button, uint32_t pov, uint32_t axis) {
    if (object->page == 9) return button;
    if (object->page == 1 && object->usage == 0x39) return pov;
    switch (object->usage) {
    case 0x30: return 0; case 0x31: return 1; case 0x32: return 2;
    case 0x33: return 3; case 0x34: return 4; case 0x35: return 5;
    case 0x36: return 6; case 0x37: return 7;
    default: return axis;
    }
}
static DWORD standard_object_offset(const hid_object *object, uint32_t instance) {
    if (object->page == 9) return 48 + instance;
    if (object->page == 1 && object->usage == 0x39) return 32 + instance * 4;
    switch (object->usage) {
    case 0x30: return 0; case 0x31: return 4; case 0x32: return 8;
    case 0x33: return 12; case 0x34: return 16; case 0x35: return 20;
    case 0x36: return 24; case 0x37: return 28;
    default: return UINT32_MAX;
    }
}
static BOOL32 object_matches_flags(DWORD object_type, DWORD flags) {
    DWORD want = flags & 0xffu;
    return !want || (want & 0x03u && object_type & 0x03u) || (want & 0x0cu && object_type & 0x0cu) ||
           (want & 0x10u && object_type & 0x10u);
}
static LONG axis_usage_for(const GUID *guid, uint32_t instance);
static BOOL32 axis_range_index(di_device *self, DWORD how, DWORD object, DWORD *index) {
    uint32_t usage = UINT32_MAX;
    if (how == DIPH_DEVICE) { *index = 0; return 1; }
    if (how == DIPH_BYOFFSET) {
        for (DWORD i = 0; i < self->format_count; i++) if (self->format[i].offset == object && self->format[i].category == 1) {
            uint32_t source = self->format[i].source_index;
            if (source < self->object_count) usage = self->objects[source].usage;
            else if (self->format[i].has_guid) usage = (uint32_t)axis_usage_for(&self->format[i].guid, 0);
            else usage = 0x30 + ((self->format[i].type >> 8) & 0xffu);
            break;
        }
    } else if (how == DIPH_BYID && (object & 0xffu) == DIDFT_ABSAXIS) {
        uint32_t instance = (object >> 8) & 0xffffu;
        if (instance < 8) usage = 0x30 + instance;
    } else if (how == 3 /* DIPH_BYUSAGE */ && (object >> 16) == 1) {
        usage = object & 0xffffu;
    }
    if (usage < 0x30 || usage > 0x37) return 0;
    *index = usage - 0x30;
    return 1;
}
static HRESULT WINAPI di_dev_enum_objects(di_device *self, enum_objects_cb callback, void *context, DWORD flags) {
    if (!callback) return VX_DI_E_INVALIDARG;
    if (!load_objects(self)) return DIERR_INPUTLOST;
    uint32_t axes = 0, buttons = 0, povs = 0;
    for (size_t i = 0; i < self->object_count; i++) {
        hid_object *src = &self->objects[i];
        dideviceobjectinstancew object = { 0 };
        object.dwSize = sizeof object;
        uint32_t instance = 0, kind = 0;
        if (src->page == 9) { instance = object_instance(src, buttons, povs, axes); buttons++; kind = DIDFT_PSHBUTTON; }
        else if (src->page == 1 && src->usage == 0x39) { instance = object_instance(src, buttons, povs, axes); povs++; kind = DIDFT_POV; }
        else { instance = object_instance(src, buttons, povs, axes); axes++; kind = DIDFT_ABSAXIS; }
        if (!object_matches_flags(kind, flags)) continue;
        object.guidType = object_guid(src, instance);
        object.dwOfs = standard_object_offset(src, instance);
        object.dwType = kind | (instance << 8);
        memcpy(object.name, src->name, sizeof object.name);
        if (!object.name[0]) {
            static const WCHAR axis_name[] = { 'A','x','i','s',0 };
            static const WCHAR button_name[] = { 'B','u','t','t','o','n',0 };
            static const WCHAR pov_name[] = { 'P','O','V',0 };
            const WCHAR *name = src->page == 9 ? button_name : src->usage == 0x39 ? pov_name : axis_name;
            memcpy(object.name, name, (wide_len(name) + 1) * sizeof(WCHAR));
        }
        object.usagePage = (uint16_t)src->page;
        object.usage = (uint16_t)src->usage;
        if (self->ansi) {
            dideviceobjectinstancea ansi;
            object_to_ansi(&object, &ansi);
            if (!((BOOL32 (WINAPI *)(const dideviceobjectinstancea *, void *))callback)(&ansi, context)) break;
        } else if (!callback(&object, context)) break;
    }
    return VX_DI_S_OK;
}
static HRESULT WINAPI di_dev_get_property(di_device *self, const GUID *property, dipropheader *header) {
    if (!property || !header) return VX_DI_E_INVALIDARG;
    uint32_t id = property_id(property);
    if (header->dwSize < sizeof *header || header->dwHeaderSize != sizeof *header) return VX_DI_E_INVALIDARG;
    if (id == DIPROP_BUFFERSIZE_ID && header->dwSize >= sizeof(dipropdword)) {
        if (header->dwHow != DIPH_DEVICE) return VX_DI_E_INVALIDARG;
        ((dipropdword *)header)->data = self->buffer_size;
        return VX_DI_S_OK;
    }
    if (id == DIPROP_RANGE_ID && header->dwSize >= sizeof(diproprange)) {
        DWORD axis;
        if (!axis_range_index(self, header->dwHow, header->dwObj, &axis)) return VX_DI_E_INVALIDARG;
        diproprange *range = (diproprange *)header;
        range->min = self->range_min[axis]; range->max = self->range_max[axis];
        return VX_DI_S_OK;
    }
    return DIERR_UNSUPPORTED;
}
static HRESULT WINAPI di_dev_set_property(di_device *self, const GUID *property, const dipropheader *header) {
    if (!property || !header) return VX_DI_E_INVALIDARG;
    if (self->acquired) return DIERR_ACQUIRED;
    uint32_t id = property_id(property);
    if (header->dwSize < sizeof *header || header->dwHeaderSize != sizeof *header) return VX_DI_E_INVALIDARG;
    if (id == DIPROP_BUFFERSIZE_ID && header->dwSize >= sizeof(dipropdword)) {
        if (header->dwHow != DIPH_DEVICE) return VX_DI_E_INVALIDARG;
        DWORD size = ((const dipropdword *)header)->data;
        if (size > DI_MAX_BUFFER_SIZE) size = DI_MAX_BUFFER_SIZE;
        if (!vx_di_buffer_set_size(&self->event_buffer, size)) return VX_DI_E_OUTOFMEMORY;
        self->buffer_size = size;
        return VX_DI_S_OK;
    }
    if (id == DIPROP_RANGE_ID && header->dwSize >= sizeof(diproprange)) {
        const diproprange *range = (const diproprange *)header;
        if (range->min >= range->max) return VX_DI_E_INVALIDARG;
        if (header->dwHow == DIPH_DEVICE) {
            for (int i = 0; i < 8; i++) { self->range_min[i] = range->min; self->range_max[i] = range->max; }
        } else {
            DWORD axis;
            if (!axis_range_index(self, header->dwHow, header->dwObj, &axis)) return VX_DI_E_INVALIDARG;
            self->range_min[axis] = range->min; self->range_max[axis] = range->max;
        }
        return VX_DI_S_OK;
    }
    return DIERR_UNSUPPORTED;
}
static HRESULT WINAPI di_dev_acquire(di_device *self) {
    if (!self->source.hid) return DIERR_INPUTLOST;
    if (self->acquired) return DIERR_ACQUIRED;
    if (!self->format_set) return VX_DI_E_INVALIDARG;
    IOReturn status = IOHIDDeviceOpen(self->source.hid, kIOHIDOptionsTypeNone);
    if (status != kIOReturnSuccess) return DIERR_OTHERAPPHASPRIO;
    self->opened = self->acquired = 1;
    vx_di_buffer_clear(&self->event_buffer);
    if (self->last_valid) memset(self->last_valid, 0, self->format_count * sizeof *self->last_valid);
    return VX_DI_S_OK;
}
static HRESULT WINAPI di_dev_unacquire(di_device *self) {
    if (self->acquired && self->event) {
        BOOL32 (WINAPI *set_event)(HANDLE) = (BOOL32 (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "SetEvent");
        if (set_event) (void)set_event(self->event);
    }
    if (self->opened) IOHIDDeviceClose(self->source.hid, kIOHIDOptionsTypeNone);
    self->opened = self->acquired = 0;
    vx_di_buffer_clear(&self->event_buffer);
    if (self->last_valid) memset(self->last_valid, 0, self->format_count * sizeof *self->last_valid);
    return VX_DI_S_OK;
}
static LONG axis_usage_for(const GUID *guid, uint32_t instance) {
    if (!guid) return (LONG)(0x30 + instance);
    switch (guid->Data1) {
    case 0xa36d02e0: return 0x30; case 0xa36d02e1: return 0x31; case 0xa36d02e2: return 0x32;
    case 0xa36d02f4: return 0x33; case 0xa36d02f5: return 0x34; case 0xa36d02e3: return 0x35;
    case 0xa36d02e4: return (LONG)(0x36 + (instance > 1 ? 1 : instance));
    default: return (LONG)(0x30 + instance);
    }
}
static BOOL32 read_hid_value(di_device *self, hid_object *object, int64_t *value) {
    IOHIDValueRef result = NULL;
    IOReturn status = IOHIDDeviceGetValue(self->source.hid, object->element, &result);
    if (status != kIOReturnSuccess || !result) return 0;
    *value = IOHIDValueGetIntegerValue(result);
    return 1;
}
static LONG normalized_axis(di_device *self, hid_object *object, uint32_t instance) {
    int64_t value = 0;
    if (!object || !read_hid_value(self, object, &value)) return self->range_min[instance < 8 ? instance : 0];
    int64_t low = object->logical_min, high = object->logical_max;
    if (high <= low) return self->range_min[instance < 8 ? instance : 0];
    if (value < low) value = low;
    if (value > high) value = high;
    int64_t scaled = (value - low) * 65535 / (high - low);
    if (instance >= 8) instance = 0;
    return self->range_min[instance] + (LONG)(scaled * (self->range_max[instance] - self->range_min[instance]) / 65535);
}
static uint32_t normalized_pov(di_device *self, hid_object *object) {
    int64_t value = 0;
    if (!object || !read_hid_value(self, object, &value)) return UINT32_MAX;
    int64_t low = object->logical_min, high = object->logical_max;
    if (high < low || value < low || value > high) return UINT32_MAX;
    uint32_t sectors = (uint32_t)(high - low + 1);
    if (!sectors) return UINT32_MAX;
    return (uint32_t)((value - low) * 36000 / sectors);
}
static uint32_t format_object_value(di_device *self, const diobject *object) {
    if (object->source_index == UINT32_MAX || object->source_index >= self->object_count) {
        if (object->category == 1) {
            uint32_t range = object->has_guid ? axis_usage_for(&object->guid, 0) - 0x30 : 0;
            if (range >= 8) range = 0;
            return (uint32_t)self->range_min[range];
        }
        return object->category == 3 ? UINT32_MAX : 0;
    }
    hid_object *source = &self->objects[object->source_index];
    if (object->category == 1 && object->offset + sizeof(LONG) <= self->format_size) {
        uint32_t range = source->usage >= 0x30 && source->usage <= 0x37 ? source->usage - 0x30 : 0;
        return (uint32_t)normalized_axis(self, source, range);
    }
    if (object->category == 2 && object->offset < self->format_size) {
        int64_t value = 0;
        return source->page == 9 && read_hid_value(self, source, &value) && value ? 0x80u : 0;
    }
    if (object->category == 3 && object->offset + sizeof(DWORD) <= self->format_size)
        return source->page == 1 && source->usage == 0x39 ? normalized_pov(self, source) : UINT32_MAX;
    return 0;
}

static DWORD di_time_now(void) {
    DWORD (WINAPI *get_tick_count)(void) = (DWORD (WINAPI *)(void))vx_find_export("kernel32.dll", "GetTickCount");
    return get_tick_count ? get_tick_count() : 0;
}

static BOOL32 di_dev_sample(di_device *self) {
    if (!load_objects(self)) return 0;
    if (!self->format_set) return 1;
    BOOL32 changed = 0;
    DWORD timestamp = di_time_now();
    for (DWORD i = 0; i < self->format_count; i++) {
        const diobject *object = &self->format[i];
        uint32_t value = format_object_value(self, object);
        if (!self->last_valid[i]) {
            self->last_values[i] = value;
            self->last_valid[i] = 1;
        } else if (self->last_values[i] != value) {
            self->last_values[i] = value;
            changed = 1;
            vx_di_buffer_push(&self->event_buffer, object->offset, value, timestamp, 0);
        }
    }
    if (changed && self->event) {
        BOOL32 (WINAPI *set_event)(HANDLE) = (BOOL32 (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "SetEvent");
        if (set_event) (void)set_event(self->event);
    }
    return 1;
}

static HRESULT WINAPI di_dev_get_state(di_device *self, DWORD bytes, void *state) {
    if (!self->acquired) return DIERR_NOTACQUIRED;
    if (!state || !self->format_set || bytes < self->format_size) return VX_DI_E_INVALIDARG;
    if (!di_dev_sample(self)) return DIERR_INPUTLOST;
    memset(state, 0, self->format_size);
    for (DWORD i = 0; i < self->format_count; i++) {
        diobject *object = &self->format[i];
        if (object->offset >= self->format_size) continue;
        uint32_t kind = object->type & 0xffu;
        uint8_t *target = (uint8_t *)state + object->offset;
        uint32_t value = self->last_values[i];
        if ((kind & 0x03u) && object->offset + sizeof(LONG) <= self->format_size)
            memcpy(target, &value, sizeof(LONG));
        else if ((kind & 0x0cu) && object->offset < self->format_size)
            target[0] = (uint8_t)value;
        else if ((kind & 0x10u) && object->offset + sizeof(DWORD) <= self->format_size)
            memcpy(target, &value, sizeof(DWORD));
    }
    return VX_DI_S_OK;
}
static HRESULT WINAPI di_dev_get_data(di_device *self, DWORD object_bytes, void *data, DWORD *count, DWORD flags) {
    if (!self->acquired) return DIERR_NOTACQUIRED;
    if (!count) return VX_DI_E_POINTER;
    if (!self->format_set || !self->buffer_size) return DIERR_NOTBUFFERED;
    if (object_bytes != sizeof(diobjectdata) || (flags & ~DIGDD_PEEK)) return VX_DI_E_INVALIDARG;
    if (!di_dev_sample(self)) return DIERR_INPUTLOST;
    uint32_t obtained = *count;
    int overflow = 0;
    if (!vx_di_buffer_read(&self->event_buffer, data, &obtained, !!(flags & DIGDD_PEEK), &overflow))
        return VX_DI_E_INVALIDARG;
    *count = obtained;
    return overflow ? DI_BUFFEROVERFLOW : VX_DI_S_OK;
}
static HRESULT WINAPI di_dev_set_format(di_device *self, const didataformat *format) {
    if (!format || format->dwSize != sizeof *format || format->dwObjSize != sizeof(diobjectdataformat) ||
        format->dwDataSize == 0 || format->dwNumObjs > 1024 || (format->dwNumObjs && !format->objects)) return VX_DI_E_INVALIDARG;
    if (self->acquired) return DIERR_ACQUIRED;
    diobject *objects = format->dwNumObjs ? calloc(format->dwNumObjs, sizeof *objects) : NULL;
    uint32_t *last_values = format->dwNumObjs ? calloc(format->dwNumObjs, sizeof *last_values) : NULL;
    uint8_t *last_valid = format->dwNumObjs ? calloc(format->dwNumObjs, sizeof *last_valid) : NULL;
    if (format->dwNumObjs && (!objects || !last_values || !last_valid)) {
        free(objects); free(last_values); free(last_valid); return VX_DI_E_OUTOFMEMORY;
    }
    if (!load_objects(self)) { free(objects); free(last_values); free(last_valid); return DIERR_INPUTLOST; }
    vx_di_hid_usage *inventory = self->object_count ? calloc(self->object_count, sizeof *inventory) : NULL;
    vx_di_format_object *requests = format->dwNumObjs ? calloc(format->dwNumObjs, sizeof *requests) : NULL;
    vx_di_resolved_object *resolved = format->dwNumObjs ? calloc(format->dwNumObjs, sizeof *resolved) : NULL;
    if ((self->object_count && !inventory) || (format->dwNumObjs && (!requests || !resolved))) {
        free(inventory); free(requests); free(resolved); free(objects); free(last_values); free(last_valid);
        return VX_DI_E_OUTOFMEMORY;
    }
    for (size_t i = 0; i < self->object_count; i++)
        inventory[i] = (vx_di_hid_usage){ self->objects[i].page, self->objects[i].usage };
    const diobjectdataformat *input = format->objects;
    for (DWORD i = 0; i < format->dwNumObjs; i++) {
        objects[i].offset = input[i].offset; objects[i].type = input[i].type; objects[i].flags = input[i].flags;
        objects[i].has_guid = input[i].guid != NULL;
        if (input[i].guid) objects[i].guid = *input[i].guid;
        requests[i] = (vx_di_format_object){ input[i].guid ? input[i].guid->Data1 : 0,
                                             input[i].offset, input[i].type };
    }
    int resolve = vx_di_resolve_format(inventory, self->object_count, format->dwFlags, format->dwDataSize,
                                       requests, format->dwNumObjs, resolved);
    free(inventory); free(requests);
    if (resolve != VX_DI_FORMAT_OK) {
        free(resolved); free(objects); free(last_values); free(last_valid);
        return resolve == VX_DI_FORMAT_UNSUPPORTED ? DIERR_UNSUPPORTED : VX_DI_E_INVALIDARG;
    }
    for (DWORD i = 0; i < format->dwNumObjs; i++) {
        objects[i].source_index = resolved[i].source_index;
        objects[i].width = resolved[i].width;
        objects[i].category = resolved[i].kind;
    }
    free(resolved);
    free(self->format); free(self->last_values); free(self->last_valid);
    self->format = objects;
    self->last_values = last_values;
    self->last_valid = last_valid;
    self->format_count = format->dwNumObjs;
    self->format_size = format->dwDataSize;
    self->format_set = 1;
    vx_di_buffer_clear(&self->event_buffer);
    return VX_DI_S_OK;
}
static HRESULT WINAPI di_dev_set_event(di_device *self, HANDLE event) {
    if (self->acquired) return DIERR_ACQUIRED;
    if (event && !vx_find_export("kernel32.dll", "SetEvent")) return DIERR_UNSUPPORTED;
    self->event = event;
    return event ? DI_POLLEDDEVICE : VX_DI_S_OK;
}
static HRESULT WINAPI di_dev_set_coop(di_device *self, void *window, DWORD flags) {
    (void)self;
    if (!window || !(flags & 0x0fu) || ((flags & 3u) == 3u) || ((flags & 12u) == 12u)) return VX_DI_E_INVALIDARG;
    return VX_DI_S_OK;
}
static HRESULT WINAPI di_dev_get_object_info(di_device *self, dideviceobjectinstancew *out, DWORD object, DWORD how) {
    if (!out || !load_objects(self)) return VX_DI_E_INVALIDARG;
    uint32_t axes = 0, buttons = 0, povs = 0;
    for (size_t i = 0; i < self->object_count; i++) {
        hid_object *src = &self->objects[i];
        if (src->page != 9 && !(src->page == 1 && src->usage == 0x39) && !(src->page == 1 && src->usage >= 0x30 && src->usage <= 0x38)) continue;
        uint32_t instance = object_instance(src, buttons, povs, axes);
        uint32_t kind = src->page == 9 ? DIDFT_PSHBUTTON :
                        src->page == 1 && src->usage == 0x39 ? DIDFT_POV : DIDFT_ABSAXIS;
        DWORD offset = standard_object_offset(src, instance);
        BOOL32 match = how == DIPH_BYOFFSET ? offset == object :
                       how == DIPH_BYID ? (kind | (instance << 8)) == object :
                       how == 3 /* DIPH_BYUSAGE */ ? (((DWORD)src->page << 16) | src->usage) == object : 0;
        if (src->page == 9) buttons++;
        else if (src->usage == 0x39) povs++;
        else axes++;
        if (!match) continue;
        DWORD size = out->dwSize;
        dideviceobjectinstancew wide = { 0 };
        wide.guidType = object_guid(src, instance); wide.dwOfs = offset;
        wide.dwType = kind | (instance << 8);
        memcpy(wide.name, src->name, sizeof wide.name); wide.usagePage = (uint16_t)src->page; wide.usage = (uint16_t)src->usage;
        if (self->ansi) { object_to_ansi(&wide, (dideviceobjectinstancea *)out); ((dideviceobjectinstancea *)out)->dwSize = size; return VX_DI_S_OK; }
        *out = wide; out->dwSize = size;
        return VX_DI_S_OK;
    }
    return DIERR_NOTFOUND;
}
static HRESULT WINAPI di_dev_get_info(di_device *self, dideviceinstancew *out) {
    if (!out || out->dwSize < (self->ansi ? sizeof(dideviceinstancea) : sizeof *out)) return VX_DI_E_INVALIDARG;
    DWORD size = out->dwSize;
    if (self->ansi) {
        dideviceinstancew wide;
        fill_instance(&self->source, &wide);
        instance_to_ansi(&wide, (dideviceinstancea *)out); ((dideviceinstancea *)out)->dwSize = size;
        return VX_DI_S_OK;
    }
    fill_instance(&self->source, out); out->dwSize = size;
    return VX_DI_S_OK;
}
static HRESULT WINAPI di_dev_run_control_panel(di_device *self, void *window, DWORD flags) {
    (void)self; (void)window; (void)flags; return DIERR_UNSUPPORTED;
}
static HRESULT WINAPI di_dev_initialize(di_device *self, void *instance, DWORD version, const GUID *guid) {
    (void)self; (void)instance; (void)guid; return version >= 0x0700 && version <= 0x0800 ? VX_DI_S_OK : VX_DI_E_INVALIDARG;
}
static HRESULT WINAPI di_dev_poll(di_device *self) {
    if (!self->acquired) return DIERR_NOTACQUIRED;
    return di_dev_sample(self) ? VX_DI_S_OK : DIERR_INPUTLOST;
}
static HRESULT WINAPI di_dev_unimplemented(void) { return DIERR_UNSUPPORTED; }

static WINAPI HRESULT DirectInput8Create(void *instance, DWORD version, const GUID *iid, void **out, void *outer) {
    (void)instance;
    if (!out || !iid) return VX_DI_E_POINTER;
    *out = NULL;
    if (outer) return DIERR_NOAGGREGATION;
    if (version < 0x0700 || version > 0x0800) return VX_DI_E_INVALIDARG;
    if (!guid_equal(iid, &IID_IUnknown) && !guid_equal(iid, &IID_IDirectInput8W) &&
        !guid_equal(iid, &IID_IDirectInput7W) && !guid_equal(iid, &IID_IDirectInput2W) &&
        !guid_equal(iid, &IID_IDirectInputW) && !input_iid_ansi(iid)) return VX_DI_E_NOINTERFACE;
    di_input *input = calloc(1, sizeof *input);
    if (!input) return VX_DI_E_OUTOFMEMORY;
    input->vtable = g_input_vtable; input->refs = 1; input->version = version; input->ansi = input_iid_ansi(iid);
    *out = input;
    return VX_DI_S_OK;
}

static const vx_export g_directinput_exports[] = { VX_EXPORT(DirectInput8Create) };
void vx_directinput_init(void) {
    vx_register_module("dinput8.dll", g_directinput_exports, sizeof g_directinput_exports / sizeof g_directinput_exports[0]);
}

#ifdef __aarch64__
#include "thunks/directinput.m.inc"
#endif
