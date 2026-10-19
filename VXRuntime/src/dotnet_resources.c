#include "dotnet_resources.h"
#include <string.h>

#define RESOURCES_MAGIC 0xbeefcaceu
#define RESOURCES_MAX_COUNT 1048576u
#define RESOURCE_TYPE_BYTE_ARRAY 32u
#define RESOURCE_TYPE_STREAM 33u
#define RESOURCE_TYPE_FIRST_USER 64u

static uint32_t read32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static int range_ok(size_t size, size_t offset, size_t length) {
    return offset <= size && length <= size - offset;
}

static int read_7bit(const uint8_t *bytes, size_t size, size_t *cursor, uint32_t *value) {
    uint32_t result = 0;
    for (unsigned i = 0; i < 5; i++) {
        if (*cursor >= size) return 0;
        uint8_t byte = bytes[(*cursor)++];
        if (i == 4 && (byte & 0xf0)) return 0;
        result |= (uint32_t)(byte & 0x7f) << (i * 7);
        if (!(byte & 0x80)) { *value = result; return 1; }
    }
    return 0;
}

static int utf8_next(const uint8_t **cursor, const uint8_t *end, uint32_t *codepoint) {
    const uint8_t *p = *cursor;
    if (p == end) return 0;
    if (p[0] < 0x80) { *codepoint = p[0]; *cursor = p + 1; return 1; }
    unsigned count; uint32_t value;
    if ((p[0] & 0xe0) == 0xc0) { count = 1; value = p[0] & 0x1f; if (value < 2) return -1; }
    else if ((p[0] & 0xf0) == 0xe0) { count = 2; value = p[0] & 0x0f; }
    else if ((p[0] & 0xf8) == 0xf0) { count = 3; value = p[0] & 0x07; }
    else return -1;
    if ((size_t)(end - p) <= count) return -1;
    for (unsigned i = 1; i <= count; i++) {
        if ((p[i] & 0xc0) != 0x80) return -1;
        value = (value << 6) | (p[i] & 0x3f);
    }
    if ((count == 2 && value < 0x800) || (count == 3 && value < 0x10000) ||
        value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return -1;
    *codepoint = value; *cursor = p + count + 1;
    return 1;
}

static int utf8_matches_utf16(const char *utf8, const uint8_t *utf16, uint32_t size_bytes) {
    if (!utf8 || !utf16 || (size_bytes & 1)) return 0;
    const uint8_t *text = (const uint8_t *)utf8;
    const uint8_t *text_end = text + strlen(utf8);
    uint32_t offset = 0;
    while (offset < size_bytes) {
        uint32_t expected; int decoded = utf8_next(&text, text_end, &expected);
        if (decoded != 1) return 0;
        uint32_t actual = (uint32_t)utf16[offset] | ((uint32_t)utf16[offset + 1] << 8);
        offset += 2;
        if (actual >= 0xd800 && actual <= 0xdbff) {
            if (offset >= size_bytes) return 0;
            uint32_t low = (uint32_t)utf16[offset] | ((uint32_t)utf16[offset + 1] << 8);
            if (low < 0xdc00 || low > 0xdfff) return 0;
            actual = 0x10000 + ((actual - 0xd800) << 10) + low - 0xdc00;
            offset += 2;
        } else if (actual >= 0xdc00 && actual <= 0xdfff) return 0;
        if (actual != expected) return 0;
    }
    return text == text_end;
}

int vx_dotnet_resources_open(const uint8_t *bytes, size_t size, vx_dotnet_resources *out) {
    if (!bytes || !out || !range_ok(size, 0, 12) || read32(bytes) != RESOURCES_MAGIC) return 0;
    uint32_t header_version = read32(bytes + 4), header_size = read32(bytes + 8);
    if (header_version != 1 || !range_ok(size, 12, header_size)) return 0;
    size_t cursor = 12 + header_size;
    if (!range_ok(size, cursor, 12) || read32(bytes + cursor) != 2) return 0;
    uint32_t resource_count = read32(bytes + cursor + 4);
    uint32_t type_count = read32(bytes + cursor + 8);
    cursor += 12;
    if (resource_count > RESOURCES_MAX_COUNT || type_count > RESOURCES_MAX_COUNT) return 0;

    for (uint32_t i = 0; i < type_count; i++) {
        uint32_t name_size;
        if (!read_7bit(bytes, size, &cursor, &name_size) || !range_ok(size, cursor, name_size)) return 0;
        cursor += name_size;
    }
    if (cursor > SIZE_MAX - 7) return 0;
    cursor = (cursor + 7) & ~(size_t)7;
    size_t array_bytes = (size_t)resource_count * 4;
    if (!range_ok(size, cursor, array_bytes) || !range_ok(size, cursor + array_bytes, array_bytes) ||
        !range_ok(size, cursor + 2 * array_bytes, 4)) return 0;
    size_t name_section = cursor + 2 * array_bytes + 4;
    uint32_t data_section = read32(bytes + cursor + 2 * array_bytes);
    if (data_section < name_section || data_section > size) return 0;
    *out = (vx_dotnet_resources){ .bytes = bytes, .size = size,
        .name_section_offset = name_section, .data_section_offset = data_section,
        .resource_count = resource_count, .type_count = type_count };
    return 1;
}

int vx_dotnet_resource_get(const vx_dotnet_resources *resources, const char *name,
                           vx_dotnet_resource *out) {
    if (!resources || !resources->bytes || !name || !out) return 0;
    size_t array_bytes = (size_t)resources->resource_count * 4;
    if (resources->data_section_offset > resources->size ||
        resources->name_section_offset > resources->data_section_offset ||
        resources->name_section_offset < 4 + array_bytes) return 0;
    size_t positions = resources->name_section_offset - 4 - array_bytes;
    if (!range_ok(resources->size, positions, array_bytes)) return 0;
    const uint8_t *bytes = resources->bytes;
    for (uint32_t i = 0; i < resources->resource_count; i++) {
        uint32_t name_offset = read32(bytes + positions + (size_t)i * 4);
        if (name_offset > resources->data_section_offset - resources->name_section_offset) return 0;
        size_t cursor = resources->name_section_offset + name_offset;
        uint32_t name_size;
        if (!read_7bit(bytes, resources->data_section_offset, &cursor, &name_size) ||
            (name_size & 1) || !range_ok(resources->data_section_offset, cursor, name_size) ||
            !range_ok(resources->data_section_offset, cursor + name_size, 4)) return 0;
        const uint8_t *name_data = bytes + cursor;
        cursor += name_size;
        uint32_t data_offset = read32(bytes + cursor);
        if (!utf8_matches_utf16(name, name_data, name_size)) continue;
        if (data_offset >= resources->size - resources->data_section_offset) return 0;
        size_t value_cursor = (size_t)resources->data_section_offset + data_offset;
        uint32_t type_code;
        if (!read_7bit(bytes, resources->size, &value_cursor, &type_code)) return 0;
        if (type_code >= RESOURCE_TYPE_FIRST_USER &&
            type_code - RESOURCE_TYPE_FIRST_USER >= resources->type_count) return 0;
        vx_dotnet_resource resource = { .name_utf16le = name_data, .name_size_bytes = name_size,
            .type_code = type_code, .value = bytes + value_cursor, .value_size = resources->size - value_cursor };
        if (type_code == RESOURCE_TYPE_BYTE_ARRAY || type_code == RESOURCE_TYPE_STREAM) {
            if (!range_ok(resources->size, value_cursor, 4)) return 0;
            uint32_t length = read32(bytes + value_cursor);
            if (!range_ok(resources->size, value_cursor + 4, length)) return 0;
            resource.value_size = 4 + (size_t)length;
            if (type_code == RESOURCE_TYPE_STREAM) {
                resource.stream_data = bytes + value_cursor + 4;
                resource.stream_size = length;
            }
        }
        *out = resource;
        return 1;
    }
    return 0;
}
