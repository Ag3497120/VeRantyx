#include "pe.h"
#include <string.h>

#define PE_MAX_SECTIONS 96
#define PE_DIRECTORY_CLR 14
#define PE_COR20_HEADER_SIZE 72
#define PE_COMIMAGE_FLAGS_NATIVE_ENTRYPOINT 0x10

static int range_ok(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= (uint64_t)size - offset;
}

static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, sizeof v); return v; }
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, sizeof v); return v; }

typedef struct {
    uint16_t length, value_length, type;
    size_t end, value_offset, children_offset;
    char key[128];
} pe_version_block;

static size_t align4_size(size_t value) { return (value + 3u) & ~(size_t)3u; }

static int version_block_get(const uint8_t *data, size_t size, size_t offset,
                             pe_version_block *out) {
    if (!data || !out || offset > size || size - offset < 6) return 0;
    uint16_t length = u16(data + offset), value_length = u16(data + offset + 2);
    uint16_t type = u16(data + offset + 4);
    if (length < 6 || length > size - offset) return 0;
    size_t end = offset + length, cursor = offset + 6, key_length = 0;
    char key[128];
    while (cursor <= end && end - cursor >= 2) {
        uint16_t unit = u16(data + cursor);
        cursor += 2;
        if (!unit) break;
        if (key_length + 1 >= sizeof key) return 0;
        key[key_length++] = unit < 0x80 ? (char)unit : '?';
    }
    if (cursor > end || end - cursor < 2 || u16(data + cursor - 2) != 0) return 0;
    key[key_length] = 0;
    size_t value_offset = align4_size(cursor);
    size_t value_size = type == 1 ? (size_t)value_length * 2 : value_length;
    if (value_offset > end || value_size > end - value_offset) return 0;
    size_t children_offset = align4_size(value_offset + value_size);
    if (children_offset > end) return 0;
    *out = (pe_version_block){ .length = length, .value_length = value_length, .type = type,
        .end = end, .value_offset = value_offset, .children_offset = children_offset };
    memcpy(out->key, key, key_length + 1);
    return 1;
}

static int version_find_child(const uint8_t *data, const pe_version_block *parent,
                              const char *key, pe_version_block *out) {
    for (size_t cursor = parent->children_offset; cursor + 6 <= parent->end;) {
        pe_version_block child;
        if (!version_block_get(data, parent->end, cursor, &child) || child.end <= cursor) return 0;
        if (!strcmp(child.key, key)) { *out = child; return 1; }
        size_t next = align4_size(child.end);
        if (next <= cursor || next > parent->end) return 0;
        cursor = next;
    }
    return 0;
}

static int append_utf8(uint32_t codepoint, char *out, size_t capacity, size_t *length) {
    uint8_t bytes[4]; size_t count;
    if (codepoint <= 0x7f) { bytes[0] = (uint8_t)codepoint; count = 1; }
    else if (codepoint <= 0x7ff) {
        bytes[0] = (uint8_t)(0xc0 | (codepoint >> 6)); bytes[1] = (uint8_t)(0x80 | (codepoint & 0x3f)); count = 2;
    } else if (codepoint <= 0xffff) {
        bytes[0] = (uint8_t)(0xe0 | (codepoint >> 12)); bytes[1] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = (uint8_t)(0x80 | (codepoint & 0x3f)); count = 3;
    } else {
        bytes[0] = (uint8_t)(0xf0 | (codepoint >> 18)); bytes[1] = (uint8_t)(0x80 | ((codepoint >> 12) & 0x3f));
        bytes[2] = (uint8_t)(0x80 | ((codepoint >> 6) & 0x3f)); bytes[3] = (uint8_t)(0x80 | (codepoint & 0x3f)); count = 4;
    }
    if (count >= capacity - *length) return 0;
    memcpy(out + *length, bytes, count);
    *length += count;
    return 1;
}

static int version_string_copy(const uint8_t *data, const pe_version_block *block,
                               char *out, size_t capacity) {
    if (!capacity || block->type != 1) return 0;
    size_t count = block->value_length, length = 0;
    const uint8_t *value = data + block->value_offset;
    while (count && u16(value + (count - 1) * 2) == 0) count--;
    for (size_t i = 0; i < count; i++) {
        uint32_t codepoint = u16(value + i * 2);
        if (codepoint >= 0xd800 && codepoint <= 0xdbff && i + 1 < count) {
            uint32_t low = u16(value + (i + 1) * 2);
            if (low >= 0xdc00 && low <= 0xdfff) {
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                i++;
            } else codepoint = 0xfffd;
        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) codepoint = 0xfffd;
        if (!append_utf8(codepoint, out, capacity, &length)) return 0;
    }
    out[length] = 0;
    return 1;
}

static void version_info_strings(const uint8_t *version, size_t size, vx_pe_version_info *out) {
    pe_version_block root, string_file_info;
    if (!version_block_get(version, size, 0, &root) || strcmp(root.key, "VS_VERSION_INFO") ||
        !version_find_child(version, &root, "StringFileInfo", &string_file_info)) return;
    for (size_t table_offset = string_file_info.children_offset;
         table_offset + 6 <= string_file_info.end;) {
        pe_version_block table;
        if (!version_block_get(version, string_file_info.end, table_offset, &table)) return;
        pe_version_block entry;
        if (!out->has_file_version_string &&
            version_find_child(version, &table, "FileVersion", &entry))
            out->has_file_version_string = version_string_copy(version, &entry, out->file_version,
                                                                sizeof out->file_version);
        if (!out->has_product_version_string &&
            version_find_child(version, &table, "ProductVersion", &entry))
            out->has_product_version_string = version_string_copy(version, &entry, out->product_version,
                                                                   sizeof out->product_version);
        size_t next = align4_size(table.end);
        if (next <= table_offset || next > string_file_info.end) return;
        table_offset = next;
    }
}

static int version_resource_find(const uint8_t *base, uint32_t size, uint32_t directory_offset,
                                 unsigned depth, uint32_t *data_rva, uint32_t *data_size) {
    if (directory_offset > size || size - directory_offset < 16 || depth > 2) return 0;
    const uint8_t *directory = base + directory_offset;
    uint32_t count = (uint32_t)u16(directory + 12) + u16(directory + 14);
    if (count > (size - directory_offset - 16) / 8) return 0;
    const uint8_t *entries = directory + 16;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t name = u32(entries + i * 8);
        uint32_t target = u32(entries + i * 8 + 4);
        if (!depth && ((name & 0x80000000u) || (name & 0xffffu) != 16)) continue;
        uint32_t relative = target & 0x7fffffffu;
        if (depth < 2) {
            if (!(target & 0x80000000u)) continue;
            if (version_resource_find(base, size, relative, depth + 1, data_rva, data_size)) return 1;
        } else {
            if ((target & 0x80000000u) || relative > size || size - relative < 16) continue;
            const uint8_t *record = base + relative;
            uint32_t rva = u32(record), length = u32(record + 4);
            if (!rva || !length) continue;
            *data_rva = rva;
            *data_size = length;
            return 1;
        }
    }
    return 0;
}

int vx_pe_version_resource(const uint8_t *data, size_t size, const vx_pe_info *pe, size_t *offset, uint32_t *length) {
    if (!data || !pe || !offset || !length || !pe->directories[2].rva || pe->directories[2].size < 16) return 0;
    size_t resource_offset;
    uint32_t version_rva, version_size;
    if (!vx_pe_rva_to_file(data, size, pe, pe->directories[2].rva, pe->directories[2].size, &resource_offset) ||
        !version_resource_find(data + resource_offset, pe->directories[2].size, 0, 0, &version_rva, &version_size) ||
        !vx_pe_rva_to_file(data, size, pe, version_rva, version_size, offset)) return 0;
    *length = version_size;
    return 1;
}

int vx_pe_version_info_get(const uint8_t *data, size_t size, const vx_pe_info *pe,
                           vx_pe_version_info *out) {
    if (!data || !pe || !out || !pe->directories[2].rva || pe->directories[2].size < 16) return 0;
    size_t resource_offset;
    uint32_t resource_rva = pe->directories[2].rva;
    uint32_t resource_size = pe->directories[2].size;
    if (!vx_pe_rva_to_file(data, size, pe, resource_rva, resource_size, &resource_offset)) return 0;
    uint32_t version_rva, version_size;
    if (!version_resource_find(data + resource_offset, resource_size, 0, 0,
                               &version_rva, &version_size)) return 0;
    size_t version_offset;
    if (!vx_pe_rva_to_file(data, size, pe, version_rva, version_size, &version_offset)) return 0;

    const uint8_t *version = data + version_offset;
    if (version_size < 6) return 0;
    uint16_t block_size = u16(version), value_size = u16(version + 2), type = u16(version + 4);
    if (block_size < 6 || block_size > version_size || type != 0) return 0;
    static const char key[] = "VS_VERSION_INFO";
    size_t cursor = 6;
    for (size_t i = 0; i < sizeof key; i++) {
        if (cursor > block_size || block_size - cursor < 2 || u16(version + cursor) != (uint8_t)key[i])
            return 0;
        cursor += 2;
    }
    cursor = (cursor + 3u) & ~(size_t)3u;
    if (value_size < 52 || cursor > block_size || block_size - cursor < 52 ||
        u32(version + cursor) != 0xfeef04bdu) return 0;
    const uint8_t *fixed = version + cursor;
    *out = (vx_pe_version_info){
        .file_version_ms = u32(fixed + 8), .file_version_ls = u32(fixed + 12),
        .product_version_ms = u32(fixed + 16), .product_version_ls = u32(fixed + 20),
        .file_flags_mask = u32(fixed + 24), .file_flags = u32(fixed + 28)
    };
    version_info_strings(version, block_size, out);
    return 1;
}

int vx_pe_rva_to_file(const uint8_t *data, size_t size, const vx_pe_info *pe,
                      uint32_t rva, uint32_t length, size_t *offset) {
    if (!data || !pe || !offset) return 0;
    if (rva < pe->headers_size && range_ok(size, rva, length)) {
        *offset = rva;
        return 1;
    }
    for (uint16_t i = 0; i < pe->section_count; i++) {
        const uint8_t *section = data + pe->section_table_offset + (size_t)i * 40;
        uint32_t virtual_size = u32(section + 8), virtual_address = u32(section + 12);
        uint32_t raw_size = u32(section + 16), raw_offset = u32(section + 20);
        uint32_t span = virtual_size > raw_size ? virtual_size : raw_size;
        if (rva < virtual_address || (uint64_t)rva + length > (uint64_t)virtual_address + span)
            continue;
        uint64_t delta = (uint64_t)rva - virtual_address;
        if (delta + length > raw_size || !range_ok(size, (uint64_t)raw_offset + delta, length))
            return 0;
        *offset = (size_t)(raw_offset + delta);
        return 1;
    }
    return 0;
}

int vx_pe_inspect(const uint8_t *data, size_t size, vx_pe_info *out) {
    if (!data || !out || size < 0x40 || data[0] != 'M' || data[1] != 'Z') return 0;
    memset(out, 0, sizeof *out);

    uint32_t pe_offset = u32(data + 0x3c);
    if (!range_ok(size, pe_offset, 24) || memcmp(data + pe_offset, "PE\0\0", 4)) return 0;
    const uint8_t *coff = data + pe_offset + 4;
    out->machine = u16(coff);
    out->section_count = u16(coff + 2);
    uint16_t optional_size = u16(coff + 16);
    out->characteristics = u16(coff + 18);
    out->optional_offset = (size_t)pe_offset + 24;
    if (!out->section_count || out->section_count > PE_MAX_SECTIONS ||
        !range_ok(size, out->optional_offset, optional_size)) return 0;

    const uint8_t *optional = data + out->optional_offset;
    if (optional_size < 2) return 0;
    out->optional_magic = u16(optional);
    size_t directory_offset;
    size_t count_offset;
    if (out->optional_magic == 0x10b) {
        directory_offset = 96;
        count_offset = 92;
        if (optional_size < directory_offset) return 0;
        out->image_base = u32(optional + 28);
    } else if (out->optional_magic == 0x20b) {
        directory_offset = 112;
        count_offset = 108;
        if (optional_size < directory_offset) return 0;
        out->image_base = u64(optional + 24);
    } else {
        return 0;
    }

    out->entry_rva = u32(optional + 16);
    out->image_size = u32(optional + 56);
    out->headers_size = u32(optional + 60);
    if (!out->image_size || !out->headers_size || out->headers_size > out->image_size ||
        out->headers_size > size) return 0;

    uint32_t directory_count = u32(optional + count_offset);
    size_t available_directories = (optional_size - directory_offset) / 8;
    if (directory_count > available_directories) return 0;
    for (size_t i = 0; i < 16 && i < directory_count; i++) {
        out->directories[i].rva = u32(optional + directory_offset + i * 8);
        out->directories[i].size = u32(optional + directory_offset + i * 8 + 4);
    }

    out->section_table_offset = out->optional_offset + optional_size;
    if (!range_ok(size, out->section_table_offset, (uint64_t)out->section_count * 40)) return 0;
    for (uint16_t i = 0; i < out->section_count; i++) {
        const uint8_t *section = data + out->section_table_offset + (size_t)i * 40;
        uint32_t virtual_size = u32(section + 8), virtual_address = u32(section + 12);
        uint32_t raw_size = u32(section + 16), raw_offset = u32(section + 20);
        uint32_t span = virtual_size > raw_size ? virtual_size : raw_size;
        if ((raw_size && !range_ok(size, raw_offset, raw_size)) ||
            (span && ((uint64_t)virtual_address + span > out->image_size))) return 0;
    }

    vx_pe_directory cli = out->directories[PE_DIRECTORY_CLR];
    if (!cli.rva && !cli.size) return 1;
    if (!cli.rva || cli.size < PE_COR20_HEADER_SIZE) return 0;
    size_t cli_offset;
    if (!vx_pe_rva_to_file(data, size, out, cli.rva, PE_COR20_HEADER_SIZE, &cli_offset)) return 0;
    const uint8_t *cor = data + cli_offset;
    uint32_t header_size = u32(cor);
    if (header_size < PE_COR20_HEADER_SIZE || header_size > cli.size ||
        !range_ok(size, cli_offset, PE_COR20_HEADER_SIZE)) return 0;

    uint32_t metadata_rva = u32(cor + 8), metadata_size = u32(cor + 12);
    size_t metadata_offset;
    if (!metadata_rva || metadata_size < 16 ||
        !vx_pe_rva_to_file(data, size, out, metadata_rva, metadata_size, &metadata_offset)) return 0;
    const uint8_t *metadata = data + metadata_offset;
    if (u32(metadata) != 0x424a5342) return 0;          // "BSJB"
    uint32_t version_length = u32(metadata + 12);
    if (!version_length || version_length > metadata_size - 16) return 0;

    out->has_cli = 1;
    out->clr_major = u16(cor + 4);
    out->clr_minor = u16(cor + 6);
    out->clr_flags = u32(cor + 16);
    out->clr_entry_point = u32(cor + 20);
    out->clr_resources.rva = u32(cor + 24);
    out->clr_resources.size = u32(cor + 28);
    if ((out->clr_resources.rva == 0) != (out->clr_resources.size == 0)) return 0;
    if (out->clr_resources.rva) {
        size_t resources_offset;
        if (!vx_pe_rva_to_file(data, size, out, out->clr_resources.rva,
                               out->clr_resources.size, &resources_offset)) return 0;
    }
    size_t copy = version_length < sizeof out->metadata_version - 1
        ? version_length : sizeof out->metadata_version - 1;
    memcpy(out->metadata_version, metadata + 16, copy);
    out->metadata_version[copy] = 0;
    for (size_t i = 0; i < copy; i++) {
        if (!out->metadata_version[i]) break;
        if ((unsigned char)out->metadata_version[i] < 0x20 ||
            (unsigned char)out->metadata_version[i] > 0x7e)
            out->metadata_version[i] = '?';
    }
    if (out->clr_flags & PE_COMIMAGE_FLAGS_NATIVE_ENTRYPOINT) {
        if (out->clr_entry_point >= out->image_size) return 0;
    } else if (out->clr_entry_point && (out->clr_entry_point >> 24) == 0) {
        return 0;                                       // EntryPointToken is a metadata token, not an RVA.
    }
    return 1;
}
