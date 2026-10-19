#include "directinput_format.h"
#include <stdlib.h>
#include <string.h>

enum {
    DIDF_ABSAXIS = 0x01,
    DIDF_RELAXIS = 0x02,
    DIDFT_ABSAXIS = 0x02,
    DIDFT_BUTTON = 0x0c,
    DIDFT_POV = 0x10,
    DIDFT_TYPEMASK = 0xff,
    DIDFT_INSTANCEMASK = 0x00ffff00,
    DIDFT_OPTIONAL = 0x80000000u,
    DIDFT_ANYINSTANCE = 0xffff,
    GUID_JOYSTICK = 0x6f1d2b70,
    GUID_XAXIS = 0xa36d02e0,
    GUID_YAXIS = 0xa36d02e1,
    GUID_ZAXIS = 0xa36d02e2,
    GUID_RZAXIS = 0xa36d02e3,
    GUID_SLIDER = 0xa36d02e4,
    GUID_RXAXIS = 0xa36d02f4,
    GUID_RYAXIS = 0xa36d02f5,
    GUID_BUTTON = 0xa36d02f0,
    GUID_POV = 0xa36d02f2
};

typedef struct { uint32_t category, usage; } guid_match;

static int guid_category(uint32_t guid, guid_match *match) {
    switch (guid) {
    case 0: case GUID_JOYSTICK: *match = (guid_match){ 1, 0 }; return 1;
    case GUID_XAXIS: *match = (guid_match){ 1, 0x30 }; return 1;
    case GUID_YAXIS: *match = (guid_match){ 1, 0x31 }; return 1;
    case GUID_ZAXIS: *match = (guid_match){ 1, 0x32 }; return 1;
    case GUID_RXAXIS: *match = (guid_match){ 1, 0x33 }; return 1;
    case GUID_RYAXIS: *match = (guid_match){ 1, 0x34 }; return 1;
    case GUID_RZAXIS: *match = (guid_match){ 1, 0x35 }; return 1;
    case GUID_SLIDER: *match = (guid_match){ 1, 0x36 }; return 1;
    case GUID_BUTTON: *match = (guid_match){ 2, 0 }; return 1;
    case GUID_POV: *match = (guid_match){ 3, 0 }; return 1;
    default: return 0;
    }
}

static uint32_t object_category(const vx_di_hid_usage *object) {
    if (object->page == 1 && object->usage >= 0x30 && object->usage <= 0x37) return 1;
    if (object->page == 9 && object->usage) return 2;
    if (object->page == 1 && object->usage == 0x39) return 3;
    return 0;
}

static int object_matches_guid(const vx_di_hid_usage *object, const guid_match *match) {
    if (object_category(object) != match->category) return 0;
    if (!match->usage) return 1;
    if (match->usage == 0x36) return object->usage == 0x36 || object->usage == 0x37;
    return object->usage == match->usage;
}

static uint32_t object_width(uint32_t category) { return category == 2 ? 1 : 4; }

static int resolve_one(const vx_di_hid_usage *inventory, size_t inventory_count,
                       const vx_di_format_object *format, uint32_t category,
                       const guid_match *match, const uint8_t *used,
                       uint32_t *source_index) {
    uint32_t instance = (format->type & DIDFT_INSTANCEMASK) >> 8;
    uint32_t ordinal = 0;
    int any = instance == DIDFT_ANYINSTANCE;
    for (size_t i = 0; i < inventory_count; i++) {
        if (!object_matches_guid(&inventory[i], match)) continue;
        if (any) {
            if (used[i]) continue;
            *source_index = (uint32_t)i;
            return 1;
        }
        if (ordinal++ == instance) {
            if (used[i]) return 0;
            *source_index = (uint32_t)i;
            return 1;
        }
    }
    (void)category;
    return 0;
}

int vx_di_resolve_format(const vx_di_hid_usage *inventory, size_t inventory_count,
                         uint32_t format_flags, uint32_t data_size,
                         const vx_di_format_object *objects, size_t object_count,
                         vx_di_resolved_object *resolved) {
    if ((inventory_count && !inventory) || !object_count || !objects || !resolved ||
        object_count > 1024 || (inventory_count && inventory_count > UINT32_MAX) ||
        !data_size || (data_size & 3)) return VX_DI_FORMAT_INVALID;
    if (format_flags == DIDF_RELAXIS) return VX_DI_FORMAT_UNSUPPORTED;
    if (format_flags != DIDF_ABSAXIS) return VX_DI_FORMAT_INVALID;

    uint8_t *used = inventory_count ? calloc(inventory_count, 1) : NULL;
    if (inventory_count && !used) return VX_DI_FORMAT_INVALID;
    int result = VX_DI_FORMAT_OK;
    for (size_t i = 0; i < object_count; i++) {
        uint32_t type = objects[i].type;
        uint32_t kind = type & DIDFT_TYPEMASK;
        uint32_t category;
        if (kind == DIDFT_ABSAXIS) category = 1;
        else if (kind == 0x04 || kind == 0x08 || kind == DIDFT_BUTTON) category = 2;
        else if (kind == DIDFT_POV) category = 3;
        else if (kind == 0x01 || kind == 0x03) { result = VX_DI_FORMAT_UNSUPPORTED; break; }
        else { result = VX_DI_FORMAT_INVALID; break; }

        // Only the type, instance and optional bits are defined for the HID objects we expose.
        if (type & ~(DIDFT_TYPEMASK | DIDFT_INSTANCEMASK | DIDFT_OPTIONAL)) {
            result = VX_DI_FORMAT_INVALID;
            break;
        }
        guid_match match;
        if (!guid_category(objects[i].guid, &match) || match.category != category) {
            result = VX_DI_FORMAT_INVALID;
            break;
        }
        uint32_t width = object_width(category);
        if (objects[i].offset > data_size || width > data_size - objects[i].offset) {
            result = VX_DI_FORMAT_INVALID;
            break;
        }
        for (size_t j = 0; j < i; j++) {
            uint32_t other_width = resolved[j].width;
            if (objects[i].offset < objects[j].offset + other_width &&
                objects[j].offset < objects[i].offset + width) {
                result = VX_DI_FORMAT_INVALID;
                break;
            }
        }
        if (result != VX_DI_FORMAT_OK) break;

        uint32_t source = UINT32_MAX;
        int found = resolve_one(inventory, inventory_count, &objects[i], category, &match,
                                used, &source);
        if (!found && !(type & DIDFT_OPTIONAL)) { result = VX_DI_FORMAT_INVALID; break; }
        if (found) used[source] = 1;
        resolved[i] = (vx_di_resolved_object){ source, width, category };
    }
    free(used);
    return result;
}
