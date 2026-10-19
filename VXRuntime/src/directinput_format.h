// Portable DirectInput data-format validation and HID object resolution.
#ifndef VX_DIRECTINPUT_FORMAT_H
#define VX_DIRECTINPUT_FORMAT_H
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t page, usage; } vx_di_hid_usage;
typedef struct { uint32_t guid, offset, type; } vx_di_format_object;
typedef struct { uint32_t source_index, width, kind; } vx_di_resolved_object;

enum {
    VX_DI_FORMAT_OK = 0,
    VX_DI_FORMAT_INVALID = -1,
    VX_DI_FORMAT_UNSUPPORTED = -2
};

// `format_flags` accepts DIDF_ABSAXIS. Relative joystick axes are not provided by the HID backend.
// A missing object is accepted only when its format entry has DIDFT_OPTIONAL; its source_index is UINT32_MAX.
int vx_di_resolve_format(const vx_di_hid_usage *inventory, size_t inventory_count,
                         uint32_t format_flags, uint32_t data_size,
                         const vx_di_format_object *objects, size_t object_count,
                         vx_di_resolved_object *resolved);

#endif
