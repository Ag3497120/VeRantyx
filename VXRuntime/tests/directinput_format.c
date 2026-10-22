#include "directinput_format.h"
#include <stdio.h>
#include <stdint.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "directinput format: check failed at line %d: %s\n", __LINE__, #x); return 1; } } while (0)

enum { ABS_AXIS = 2, AXIS = 3, BUTTON = 0x0c, POV = 0x10, ANY = 0xffff, OPTIONAL = 0x80000000u };
enum { X_AXIS = 0xa36d02e0, SLIDER = 0xa36d02e4, BUTTON_GUID = 0xa36d02f0, POV_GUID = 0xa36d02f2 };

int main(void) {
    const vx_di_hid_usage inventory[] = {
        { 1, 0x30 }, { 1, 0x31 }, { 1, 0x32 }, { 1, 0x33 }, { 1, 0x34 }, { 1, 0x35 },
        { 1, 0x36 }, { 1, 0x37 }, { 9, 1 }, { 9, 2 }, { 1, 0x39 }
    };
    const vx_di_format_object standard[] = {
        { X_AXIS, 0, AXIS | (ANY << 8) },
        { 0, 4, ABS_AXIS | (1u << 8) },
        { SLIDER, 24, ABS_AXIS | (ANY << 8) },
        { SLIDER, 28, ABS_AXIS | (ANY << 8) },
        { POV_GUID, 32, POV | (ANY << 8) },
        { BUTTON_GUID, 48, BUTTON | (ANY << 8) },
        { BUTTON_GUID, 49, BUTTON | (ANY << 8) }
    };
    vx_di_resolved_object resolved[7] = { 0 };
    CHECK(vx_di_resolve_format(inventory, sizeof inventory / sizeof inventory[0], 1, 52,
                               standard, sizeof standard / sizeof standard[0], resolved) == VX_DI_FORMAT_OK);
    CHECK(resolved[0].source_index == 0 && resolved[1].source_index == 1);
    CHECK(resolved[2].source_index == 6 && resolved[3].source_index == 7);
    CHECK(resolved[4].source_index == 10 && resolved[5].source_index == 8 && resolved[6].source_index == 9);

    const vx_di_format_object optional[] = { { BUTTON_GUID, 0, BUTTON | (ANY << 8) | OPTIONAL } };
    const vx_di_hid_usage no_buttons[] = { { 1, 0x30 } };
    CHECK(vx_di_resolve_format(no_buttons, 1, 1, 4, optional, 1, resolved) == VX_DI_FORMAT_OK);
    CHECK(resolved[0].source_index == UINT32_MAX && resolved[0].width == 1);
    const vx_di_format_object required[] = { { BUTTON_GUID, 0, BUTTON | (ANY << 8) } };
    CHECK(vx_di_resolve_format(no_buttons, 1, 1, 4, required, 1, resolved) == VX_DI_FORMAT_INVALID);
    const vx_di_format_object generic_button[] = { { 0, 0, BUTTON | (ANY << 8) } };
    const vx_di_hid_usage one_button[] = { { 9, 1 } };
    CHECK(vx_di_resolve_format(one_button, 1, 1, 4, generic_button, 1, resolved) == VX_DI_FORMAT_OK);
    CHECK(resolved[0].source_index == 0 && resolved[0].kind == 2);

    CHECK(vx_di_resolve_format(inventory, 11, 3, 52, standard, 7, resolved) == VX_DI_FORMAT_INVALID);
    const vx_di_format_object relative[] = { { 0, 0, 1 | (ANY << 8) } };
    CHECK(vx_di_resolve_format(inventory, 11, 2, 4, relative, 1, resolved) == VX_DI_FORMAT_UNSUPPORTED);
    const vx_di_format_object overlap[] = {
        { X_AXIS, 0, ABS_AXIS | (ANY << 8) }, { 0, 2, ABS_AXIS | (ANY << 8) }
    };
    CHECK(vx_di_resolve_format(inventory, 11, 1, 8, overlap, 2, resolved) == VX_DI_FORMAT_INVALID);
    const vx_di_format_object duplicate[] = {
        { X_AXIS, 0, ABS_AXIS | (ANY << 8) }, { X_AXIS, 4, ABS_AXIS | (ANY << 8) }
    };
    CHECK(vx_di_resolve_format(inventory, 11, 1, 8, duplicate, 2, resolved) == VX_DI_FORMAT_INVALID);
    const vx_di_format_object optional_overlap[] = {
        { X_AXIS, 0, AXIS | (ANY << 8) }, { X_AXIS, 0, AXIS | (ANY << 8) | OPTIONAL }
    };
    const vx_di_hid_usage one_axis[] = { { 1, 0x30 } };
    CHECK(vx_di_resolve_format(one_axis, 1, 1, 4, optional_overlap, 2, resolved) == VX_DI_FORMAT_OK);
    CHECK(resolved[0].source_index == 0 && resolved[1].source_index == UINT32_MAX);
    puts("directinput format: DIDFT_AXIS, DIDFT_ANYINSTANCE, HID mapping, optional objects and malformed layouts: ok");
    return 0;
}
