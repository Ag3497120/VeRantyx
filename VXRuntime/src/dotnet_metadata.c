#include "dotnet_metadata.h"
#include <string.h>

#define MD_MAX_STREAMS 32
#define MD_MAX_STREAM_NAME 32
#define CLI_DIRECTORY 14
#define COR20_HEADER_SIZE 72

typedef enum { C_U1, C_U2, C_U4, C_STRING, C_GUID, C_BLOB, C_TABLE, C_CODED } col_kind;
typedef struct { uint8_t kind, value; } col_spec;
typedef struct { const col_spec *columns; uint8_t count; } table_spec;
typedef struct { uint8_t tag_bits, count; const uint8_t *tables; } coded_spec;

#define U1       { C_U1, 0 }
#define U2       { C_U2, 0 }
#define U4       { C_U4, 0 }
#define STR      { C_STRING, 0 }
#define GUID     { C_GUID, 0 }
#define BLOB     { C_BLOB, 0 }
#define T(t)     { C_TABLE, (t) }
#define CD(c)    { C_CODED, (c) }

static const col_spec t0[]  = { U2, STR, GUID, GUID, GUID };
static const col_spec t1[]  = { CD(0), STR, STR };
static const col_spec t2[]  = { U4, STR, STR, CD(1), T(4), T(6) };
static const col_spec t3[]  = { T(4) };
static const col_spec t4[]  = { U2, STR, BLOB };
static const col_spec t5[]  = { T(6) };
static const col_spec t6[]  = { U4, U2, U2, STR, BLOB, T(8) };
static const col_spec t7[]  = { T(8) };
static const col_spec t8[]  = { U2, U2, STR };
static const col_spec t9[]  = { T(2), CD(1) };
static const col_spec t10[] = { CD(2), STR, BLOB };
static const col_spec t11[] = { U1, U1, CD(3), BLOB };
static const col_spec t12[] = { CD(4), CD(8), BLOB };
static const col_spec t13[] = { CD(5), BLOB };
static const col_spec t14[] = { U2, CD(6), BLOB };
static const col_spec t15[] = { U2, U4, T(2) };
static const col_spec t16[] = { U4, T(4) };
static const col_spec t17[] = { BLOB };
static const col_spec t18[] = { T(2), T(20) };
static const col_spec t19[] = { T(20) };
static const col_spec t20[] = { U2, STR, CD(1) };
static const col_spec t21[] = { T(2), T(23) };
static const col_spec t22[] = { T(23) };
static const col_spec t23[] = { U2, STR, BLOB };
static const col_spec t24[] = { U2, T(6), CD(9) };
static const col_spec t25[] = { T(2), CD(10), CD(10) };
static const col_spec t26[] = { STR };
static const col_spec t27[] = { BLOB };
static const col_spec t28[] = { U2, CD(11), STR, T(26) };
static const col_spec t29[] = { U4, T(4) };
static const col_spec t30[] = { U4, U4 };
static const col_spec t31[] = { U4 };
static const col_spec t32[] = { U4, U2, U2, U2, U2, U4, BLOB, STR, STR };
static const col_spec t33[] = { U4 };
static const col_spec t34[] = { U4, U4, U4 };
static const col_spec t35[] = { U2, U2, U2, U2, U4, BLOB, STR, STR, BLOB };
static const col_spec t36[] = { U4, T(35) };
static const col_spec t37[] = { U4, U4, U4, T(35) };
static const col_spec t38[] = { U4, STR, BLOB };
static const col_spec t39[] = { U4, U4, STR, STR, CD(12) };
static const col_spec t40[] = { U4, U4, STR, CD(12) };
static const col_spec t41[] = { T(2), T(2) };
// GenericParam.Owner is the TypeOrMethodDef coded index (index 12).
static const col_spec t42[] = { U2, U2, CD(12), STR };
static const col_spec t43[] = { CD(10), BLOB };
static const col_spec t44[] = { T(42), CD(1) };

#define TS(t) { t, (uint8_t)(sizeof(t) / sizeof((t)[0])) }
static const table_spec g_tables[45] = {
    [0]=TS(t0), [1]=TS(t1), [2]=TS(t2), [3]=TS(t3), [4]=TS(t4), [5]=TS(t5),
    [6]=TS(t6), [7]=TS(t7), [8]=TS(t8), [9]=TS(t9), [10]=TS(t10), [11]=TS(t11),
    [12]=TS(t12), [13]=TS(t13), [14]=TS(t14), [15]=TS(t15), [16]=TS(t16), [17]=TS(t17),
    [18]=TS(t18), [19]=TS(t19), [20]=TS(t20), [21]=TS(t21), [22]=TS(t22), [23]=TS(t23),
    [24]=TS(t24), [25]=TS(t25), [26]=TS(t26), [27]=TS(t27), [28]=TS(t28), [29]=TS(t29),
    [30]=TS(t30), [31]=TS(t31), [32]=TS(t32), [33]=TS(t33), [34]=TS(t34), [35]=TS(t35),
    [36]=TS(t36), [37]=TS(t37), [38]=TS(t38), [39]=TS(t39), [40]=TS(t40), [41]=TS(t41),
    [42]=TS(t42), [43]=TS(t43), [44]=TS(t44)
};

static const uint8_t c_resolution_scope[] = { 0, 26, 35, 1 };
static const uint8_t c_type_def_or_ref[] = { 2, 1, 27 };
static const uint8_t c_member_ref_parent[] = { 2, 1, 26, 6, 27 };
static const uint8_t c_has_constant[] = { 4, 8, 23 };
static const uint8_t c_has_custom_attribute[] = {
    6, 4, 1, 2, 8, 9, 10, 0, 14, 23, 20, 17, 26, 27, 32, 35, 38, 39, 40, 42, 44, 43
};
static const uint8_t c_has_field_marshal[] = { 4, 8 };
static const uint8_t c_has_decl_security[] = { 2, 6, 32 };
static const uint8_t c_custom_attribute_type[] = { 6, 10 };
static const uint8_t c_has_semantics[] = { 20, 23 };
static const uint8_t c_method_def_or_ref[] = { 6, 10 };
static const uint8_t c_member_forwarded[] = { 4, 6 };
static const uint8_t c_implementation[] = { 38, 35, 39 };
static const uint8_t c_type_or_method_def[] = { 2, 6 };
static const coded_spec g_coded[] = {
    { 2, 4, c_resolution_scope }, { 2, 3, c_type_def_or_ref },
    { 3, 5, c_member_ref_parent }, { 2, 3, c_has_constant },
    { 5, 22, c_has_custom_attribute }, { 1, 2, c_has_field_marshal },
    { 2, 3, c_has_decl_security }, { 3, 2, c_custom_attribute_type },
    { 1, 2, c_has_semantics }, { 1, 2, c_method_def_or_ref },
    { 1, 2, c_member_forwarded }, { 2, 3, c_implementation },
    { 1, 2, c_type_or_method_def }
};

static uint16_t read16(const uint8_t *p) { uint16_t v; memcpy(&v, p, sizeof v); return v; }
static uint32_t read32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static uint32_t read24(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); }

static int validate_method_sections(const vx_dotnet_metadata *md, const uint8_t *sections,
                                    size_t available, uint32_t code_size, uint32_t *total_size) {
    size_t cursor = 0;
    for (unsigned count = 0; count < 32; count++) {
        if (cursor > available || available - cursor < 4) return 0;
        const uint8_t *section = sections + cursor;
        uint8_t kind = section[0];
        int fat = !!(kind & 0x40), more = !!(kind & 0x80);
        if ((kind & 0x3f) != 1) return 0; // EH table
        uint32_t size = fat ? read24(section + 1) : section[1];
        uint32_t clause_size = fat ? 24 : 12;
        if (size < 4 || size > available - cursor || (size - 4) % clause_size) return 0;
        if (!fat && (section[2] || section[3])) return 0;
        for (size_t at = 4; at < size; at += clause_size) {
            const uint8_t *clause = section + at;
            uint32_t flags = fat ? read32(clause) : read16(clause);
            uint32_t try_offset = fat ? read32(clause + 4) : read16(clause + 2);
            uint32_t try_length = fat ? read32(clause + 8) : clause[4];
            uint32_t handler_offset = fat ? read32(clause + 12) : read16(clause + 5);
            uint32_t handler_length = fat ? read32(clause + 16) : clause[7];
            uint32_t class_or_filter = fat ? read32(clause + 20) : read32(clause + 8);
            if ((flags != 0 && flags != 1 && flags != 2 && flags != 4) ||
                try_offset > code_size || try_length > code_size - try_offset ||
                handler_offset > code_size || handler_length > code_size - handler_offset) return 0;
            if (flags == 0) {
                uint8_t table = (uint8_t)(class_or_filter >> 24);
                uint32_t rid = class_or_filter & 0x00ffffffu;
                if ((table != 1 && table != 2) || !rid || rid > md->row_count[table]) return 0;
            } else if (flags == 1 && class_or_filter >= code_size) return 0;
        }
        cursor += size;
        if (!more) {
            *total_size = (uint32_t)cursor;
            return 1;
        }
        cursor = (cursor + 3u) & ~(size_t)3u;
    }
    return 0;
}
static uint64_t read64(const uint8_t *p) { uint64_t v; memcpy(&v, p, sizeof v); return v; }
static int range_ok(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= (uint64_t)size - offset;
}
static uint32_t align4(uint32_t n) { return (n + 3u) & ~3u; }

static int coded_width(const vx_dotnet_metadata *md, uint8_t coded) {
    if (coded >= sizeof g_coded / sizeof g_coded[0]) return 0;
    const coded_spec *c = &g_coded[coded];
    uint32_t max_rows = 0;
    for (uint8_t i = 0; i < c->count; i++) {
        uint8_t table = c->tables[i];
        if (md->row_count[table] > max_rows) max_rows = md->row_count[table];
    }
    return max_rows < (1u << (16 - c->tag_bits)) ? 2 : 4;
}

static int column_width(const vx_dotnet_metadata *md, col_spec col) {
    switch (col.kind) {
        case C_U1: return 1;
        case C_U2: return 2;
        case C_U4: return 4;
        case C_STRING: return (md->heap_sizes & 1) ? 4 : 2;
        case C_GUID: return (md->heap_sizes & 2) ? 4 : 2;
        case C_BLOB: return (md->heap_sizes & 4) ? 4 : 2;
        case C_TABLE:
            if (col.value >= 45) return 0;
            return md->row_count[col.value] < 0x10000u ? 2 : 4;
        case C_CODED: return coded_width(md, col.value);
    }
    return 0;
}

static int next_stream(const uint8_t *md, size_t md_size, uint32_t *cursor,
                       const char **name, size_t *name_length,
                       const uint8_t **data, size_t *length) {
    if (!range_ok(md_size, *cursor, 8)) return 0;
    uint32_t offset = read32(md + *cursor), size = read32(md + *cursor + 4);
    size_t name_at = (size_t)*cursor + 8;
    size_t cap = md_size - name_at;
    if (cap > MD_MAX_STREAM_NAME) cap = MD_MAX_STREAM_NAME;
    const uint8_t *nul = memchr(md + name_at, 0, cap);
    if (!nul) return 0;
    size_t name_len = (size_t)(nul - (md + name_at));
    uint32_t header_len = 8u + align4((uint32_t)name_len + 1u);
    if (!range_ok(md_size, *cursor, header_len) || !range_ok(md_size, offset, size)) return 0;
    *name = (const char *)(md + name_at);
    *name_length = name_len;
    *data = md + offset;
    *length = size;
    *cursor += header_len;
    return 1;
}

int vx_dotnet_metadata_open(const uint8_t *image, size_t image_size,
                            const vx_pe_info *pe, vx_dotnet_metadata *out) {
    if (!image || !pe || !out || !pe->has_cli || image_size < 0x40) return 0;
    memset(out, 0, sizeof *out);
    const vx_pe_directory cli = pe->directories[CLI_DIRECTORY];
    size_t cli_offset;
    if (!cli.rva || cli.size < COR20_HEADER_SIZE ||
        !vx_pe_rva_to_file(image, image_size, pe, cli.rva, COR20_HEADER_SIZE, &cli_offset)) return 0;
    const uint8_t *cor = image + cli_offset;
    if (read32(cor) < COR20_HEADER_SIZE) return 0;
    uint32_t metadata_rva = read32(cor + 8), metadata_size = read32(cor + 12);
    size_t metadata_offset;
    if (!metadata_rva || metadata_size < 20 ||
        !vx_pe_rva_to_file(image, image_size, pe, metadata_rva, metadata_size, &metadata_offset)) return 0;
    const uint8_t *root = image + metadata_offset;
    if (read32(root) != 0x424a5342u) return 0;
    uint32_t version_length = read32(root + 12);
    if (!version_length || version_length > UINT32_MAX - 3 || version_length > metadata_size - 16) return 0;
    uint32_t stream_cursor = 16u + align4(version_length);
    if (!range_ok(metadata_size, stream_cursor, 4)) return 0;
    uint16_t stream_count = read16(root + stream_cursor + 2);
    stream_cursor += 4;
    if (!stream_count || stream_count > MD_MAX_STREAMS) return 0;

    out->metadata = root;
    out->metadata_size = metadata_size;
    out->image = image;
    out->image_size = image_size;
    for (uint16_t i = 0; i < stream_count; i++) {
        const char *name;
        size_t name_length, stream_length;
        const uint8_t *stream;
        if (!next_stream(root, metadata_size, &stream_cursor, &name, &name_length,
                         &stream, &stream_length)) return 0;
#define MATCH(n) (name_length == sizeof(n) - 1 && !memcmp(name, n, sizeof(n) - 1))
        const uint8_t **slot = NULL;
        size_t *slot_size = NULL;
        if (MATCH("#~") || MATCH("#-")) {
            slot = &out->tables; slot_size = &out->tables_size;
            out->unoptimized_tables = MATCH("#-");
        } else if (MATCH("#Strings")) { slot = &out->strings; slot_size = &out->strings_size; }
        else if (MATCH("#Blob")) { slot = &out->blob; slot_size = &out->blob_size; }
        else if (MATCH("#GUID")) { slot = &out->guid; slot_size = &out->guid_size; }
        else if (MATCH("#US")) { slot = &out->user_strings; slot_size = &out->user_strings_size; }
        if (slot) {
            if (*slot) return 0;
            *slot = stream;
            *slot_size = stream_length;
        }
#undef MATCH
    }
    if (!out->tables || !out->strings || !out->blob || out->tables_size < 24) return 0;
    out->entry_point_token = read32(cor + 20);

    const uint8_t *table_data = out->tables;
    out->heap_sizes = table_data[6];
    uint64_t valid = read64(table_data + 8);
    out->valid_tables = valid;
    if (valid >> 45) return 0;
    size_t cursor = 24;
    for (uint8_t t = 0; t < 45; t++) {
        if (!(valid & (1ull << t))) continue;
        if (!range_ok(out->tables_size, cursor, 4)) return 0;
        out->row_count[t] = read32(table_data + cursor);
        cursor += 4;
    }
    size_t row_data_start = cursor;
    for (uint8_t t = 0; t < 45; t++) {
        if (!(valid & (1ull << t))) continue;
        const table_spec *spec = &g_tables[t];
        uint64_t row_bytes = 0;
        for (uint8_t c = 0; c < spec->count; c++) {
            int width = column_width(out, spec->columns[c]);
            if (!width) return 0;
            row_bytes += (unsigned)width;
        }
        if (!row_bytes || row_bytes > UINT32_MAX) return 0;
        out->row_size[t] = (uint32_t)row_bytes;
        out->table_offset[t] = row_data_start;
        uint64_t table_bytes = row_bytes * out->row_count[t];
        if (table_bytes > SIZE_MAX || !range_ok(out->tables_size, row_data_start, table_bytes)) return 0;
        row_data_start += (size_t)table_bytes;
    }
    return row_data_start <= out->tables_size;
}

int vx_dotnet_column(const vx_dotnet_metadata *md, uint8_t table, uint32_t row,
                     uint8_t column, uint32_t *value) {
    if (!md || !value || table >= 45 || row >= md->row_count[table] || !md->tables) return 0;
    const table_spec *spec = &g_tables[table];
    if (column >= spec->count) return 0;
    size_t offset = md->table_offset[table] + (size_t)row * md->row_size[table];
    for (uint8_t c = 0; c < column; c++) {
        int width = column_width(md, spec->columns[c]);
        if (!width) return 0;
        offset += (unsigned)width;
    }
    int width = column_width(md, spec->columns[column]);
    if (!width || !range_ok(md->tables_size, offset, (unsigned)width)) return 0;
    if (width == 1) *value = md->tables[offset];
    else if (width == 2) *value = read16(md->tables + offset);
    else *value = read32(md->tables + offset);
    return 1;
}

int vx_dotnet_string(const vx_dotnet_metadata *md, uint32_t index, const char **out) {
    if (!md || !out || !md->strings || index >= md->strings_size) return 0;
    const char *s = (const char *)md->strings + index;
    if (!memchr(s, 0, md->strings_size - index)) return 0;
    *out = s;
    return 1;
}

static int compressed_u32(const uint8_t *p, size_t size, uint32_t *value, size_t *used) {
    if (!size) return 0;
    if (!(p[0] & 0x80)) { *value = p[0]; *used = 1; return 1; }
    if ((p[0] & 0xc0) == 0x80) {
        if (size < 2) return 0;
        *value = ((uint32_t)(p[0] & 0x3f) << 8) | p[1]; *used = 2; return 1;
    }
    if ((p[0] & 0xe0) == 0xc0) {
        if (size < 4) return 0;
        *value = ((uint32_t)(p[0] & 0x1f) << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | p[3];
        if (*value > 0x1fffffffu) return 0;
        *used = 4; return 1;
    }
    return 0;
}

int vx_dotnet_blob(const vx_dotnet_metadata *md, uint32_t index,
                   const uint8_t **out, uint32_t *length) {
    if (!md || !out || !length || !md->blob || index >= md->blob_size) return 0;
    uint32_t len; size_t prefix;
    if (!compressed_u32(md->blob + index, md->blob_size - index, &len, &prefix) ||
        !range_ok(md->blob_size, (uint64_t)index + prefix, len)) return 0;
    *out = md->blob + index + prefix;
    *length = len;
    return 1;
}

int vx_dotnet_user_string(const vx_dotnet_metadata *md, uint32_t index,
                          const uint8_t **utf16le, uint32_t *code_units) {
    if (!md || !utf16le || !code_units || !md->user_strings || index >= md->user_strings_size) return 0;
    uint32_t byte_length; size_t prefix;
    if (!compressed_u32(md->user_strings + index, md->user_strings_size - index,
                        &byte_length, &prefix) || byte_length < 1 ||
        !range_ok(md->user_strings_size, (uint64_t)index + prefix, byte_length)) return 0;
    uint32_t text_bytes = byte_length - 1; // final byte records whether the string needs special handling
    if (text_bytes & 1) return 0;
    *utf16le = md->user_strings + index + prefix;
    *code_units = text_bytes / 2;
    return 1;
}

static int type_name_depth(const vx_dotnet_metadata *md, uint32_t token,
                           const char **name_space, const char **name, unsigned depth) {
    if (!md || !name_space || !name || depth > 8) return 0;
    uint8_t table = (uint8_t)(token >> 24);
    uint32_t rid = token & 0x00ffffffu;
    uint32_t name_index, namespace_index;
    if (!rid) return 0;
    if (table == 2) {
        if (!vx_dotnet_column(md, 2, rid - 1, 1, &name_index) ||
            !vx_dotnet_column(md, 2, rid - 1, 2, &namespace_index)) return 0;
    } else if (table == 1) {
        if (!vx_dotnet_column(md, 1, rid - 1, 1, &name_index) ||
            !vx_dotnet_column(md, 1, rid - 1, 2, &namespace_index)) return 0;
    } else if (table == 27) {
        uint32_t blob_index; const uint8_t *sig; uint32_t length;
        if (!vx_dotnet_column(md, 27, rid - 1, 0, &blob_index) ||
            !vx_dotnet_blob(md, blob_index, &sig, &length) || !length) return 0;
        size_t cursor = 0;
        if (sig[cursor] == 0x15) {
            if (length < 2 || (sig[cursor + 1] != 0x11 && sig[cursor + 1] != 0x12)) return 0;
            cursor += 2;
        } else if (sig[cursor] == 0x11 || sig[cursor] == 0x12) cursor++;
        else return 0;
        uint32_t coded; size_t used;
        if (!compressed_u32(sig + cursor, length - cursor, &coded, &used) || !used) return 0;
        uint8_t tag = coded & 3;
        uint32_t type_rid = coded >> 2;
        static const uint8_t type_tables[] = { 2, 1, 27 };
        if (tag >= sizeof type_tables || !type_rid) return 0;
        uint32_t type_token = ((uint32_t)type_tables[tag] << 24) | type_rid;
        return type_name_depth(md, type_token, name_space, name, depth + 1);
    } else return 0;
    return vx_dotnet_string(md, name_index, name) && vx_dotnet_string(md, namespace_index, name_space);
}

int vx_dotnet_type_name(const vx_dotnet_metadata *md, uint32_t token,
                        const char **name_space, const char **name) {
    return type_name_depth(md, token, name_space, name, 0);
}

int vx_dotnet_type_assembly_ref(const vx_dotnet_metadata *md, uint32_t type_ref_token,
                                uint32_t *assembly_ref_token) {
    if (!md || !assembly_ref_token || (type_ref_token >> 24) != 1) return 0;
    uint32_t token = type_ref_token;
    for (unsigned depth = 0; depth < 32; depth++) {
        uint32_t rid = token & 0x00ffffffu, scope;
        if (!rid || rid > md->row_count[1] || !vx_dotnet_column(md, 1, rid - 1, 0, &scope)) return 0;
        uint32_t scope_rid = scope >> 2;
        switch (scope & 3) {
            case 2: // AssemblyRef
                if (!scope_rid || scope_rid > md->row_count[35]) return 0;
                *assembly_ref_token = 0x23000000u | scope_rid;
                return 1;
            case 3: // Nested TypeRef: follow its enclosing type's resolution scope.
                if (!scope_rid || scope_rid > md->row_count[1]) return 0;
                token = 0x01000000u | scope_rid;
                break;
            default: // Module and ModuleRef scopes do not name an AssemblyRef.
                return 0;
        }
    }
    return 0;
}

int vx_dotnet_assembly_ref_name(const vx_dotnet_metadata *md, uint32_t token, const char **name) {
    if (!md || !name || (token >> 24) != 35) return 0;
    uint32_t rid = token & 0x00ffffffu, name_index;
    return rid && rid <= md->row_count[35] &&
           vx_dotnet_column(md, 35, rid - 1, 6, &name_index) &&
           vx_dotnet_string(md, name_index, name);
}

int vx_dotnet_method_get(const vx_dotnet_metadata *md, uint32_t token, vx_dotnet_method *out) {
    if (!md || !out || (token >> 24) != 6) return 0;
    uint32_t rid = token & 0x00ffffffu;
    if (!rid || rid > md->row_count[6]) return 0;
    uint32_t fields[6];
    for (uint8_t i = 0; i < 6; i++) if (!vx_dotnet_column(md, 6, rid - 1, i, &fields[i])) return 0;
    const char *name;
    if (!vx_dotnet_string(md, fields[3], &name)) return 0;
    uint32_t owner = 0;
    const char *type_name = NULL, *type_namespace = NULL;
    uint8_t method_list_table = md->row_count[5] ? 5 : 6;
    uint32_t method_list_count = md->row_count[method_list_table];
    for (uint32_t i = 0; i < md->row_count[2] && !owner; i++) {
        uint32_t first, next;
        if (!vx_dotnet_column(md, 2, i, 5, &first)) return 0;
        if (i + 1 < md->row_count[2]) {
            if (!vx_dotnet_column(md, 2, i + 1, 5, &next)) return 0;
        } else next = method_list_count + 1;
        if (!first || first > next || next > method_list_count + 1) return 0;
        for (uint32_t list_rid = first; list_rid < next; list_rid++) {
            uint32_t method_rid = list_rid;
            if (method_list_table == 5 && !vx_dotnet_column(md, 5, list_rid - 1, 0, &method_rid)) return 0;
            if (method_rid != rid) continue;
            uint32_t n, ns;
            if (!vx_dotnet_column(md, 2, i, 1, &n) || !vx_dotnet_column(md, 2, i, 2, &ns) ||
                !vx_dotnet_string(md, n, &type_name) || !vx_dotnet_string(md, ns, &type_namespace)) return 0;
            owner = 0x02000000u | (i + 1);
            break;
        }
    }
    *out = (vx_dotnet_method){
        .token = token, .rva = fields[0], .impl_flags = (uint16_t)fields[1],
        .flags = (uint16_t)fields[2], .name_index = fields[3], .signature_index = fields[4],
        .parameter_list = fields[5], .declaring_type_token = owner, .name = name,
        .declaring_type_name = type_name, .declaring_type_namespace = type_namespace
    };
    return 1;
}

int vx_dotnet_member_ref_get(const vx_dotnet_metadata *md, uint32_t token, vx_dotnet_member_ref *out) {
    if (!md || !out || (token >> 24) != 10) return 0;
    uint32_t rid = token & 0x00ffffffu, parent, name_index, signature_index;
    if (!rid || rid > md->row_count[10] ||
        !vx_dotnet_column(md, 10, rid - 1, 0, &parent) ||
        !vx_dotnet_column(md, 10, rid - 1, 1, &name_index) ||
        !vx_dotnet_column(md, 10, rid - 1, 2, &signature_index)) return 0;
    uint8_t tag = parent & 7;
    uint32_t parent_rid = parent >> 3;
    static const uint8_t parent_tables[] = { 2, 1, 26, 6, 27 };
    if (tag >= sizeof parent_tables || !parent_rid) return 0;
    uint32_t parent_token = ((uint32_t)parent_tables[tag] << 24) | parent_rid;
    const char *parent_name = "", *parent_namespace = "";
    if (parent_tables[tag] == 2 || parent_tables[tag] == 1 || parent_tables[tag] == 27) {
        if (!vx_dotnet_type_name(md, parent_token, &parent_namespace, &parent_name)) return 0;
    } else if (parent_tables[tag] == 6) {
        vx_dotnet_method method;
        if (!vx_dotnet_method_get(md, parent_token, &method)) return 0;
        parent_name = method.declaring_type_name ? method.declaring_type_name : "";
        parent_namespace = method.declaring_type_namespace ? method.declaring_type_namespace : "";
        parent_token = method.declaring_type_token;
    }
    const char *member_name;
    if (!vx_dotnet_string(md, name_index, &member_name)) return 0;
    *out = (vx_dotnet_member_ref){ .token = token, .parent_token = parent_token,
        .name_index = name_index, .signature_index = signature_index, .name = member_name,
        .parent_name = parent_name, .parent_namespace = parent_namespace };
    return 1;
}

int vx_dotnet_field_get(const vx_dotnet_metadata *md, uint32_t token, vx_dotnet_field *out) {
    if (!md || !out || (token >> 24) != 4) return 0;
    uint32_t rid = token & 0x00ffffffu, flags, name_index, signature_index;
    if (!rid || rid > md->row_count[4] || !vx_dotnet_column(md, 4, rid - 1, 0, &flags) ||
        !vx_dotnet_column(md, 4, rid - 1, 1, &name_index) ||
        !vx_dotnet_column(md, 4, rid - 1, 2, &signature_index)) return 0;
    const char *name;
    if (!vx_dotnet_string(md, name_index, &name)) return 0;
    *out = (vx_dotnet_field){ .token = token, .flags = (uint16_t)flags,
        .name_index = name_index, .signature_index = signature_index, .name = name };
    return 1;
}

int vx_dotnet_manifest_resource_get(const vx_dotnet_metadata *md, const vx_pe_info *pe,
                                    uint32_t token, vx_dotnet_manifest_resource *out) {
    if (!md || !pe || !out || (token >> 24) != 40) return 0;
    uint32_t rid = token & 0x00ffffffu, offset, flags, name_index, implementation;
    if (!rid || rid > md->row_count[40] ||
        !vx_dotnet_column(md, 40, rid - 1, 0, &offset) ||
        !vx_dotnet_column(md, 40, rid - 1, 1, &flags) ||
        !vx_dotnet_column(md, 40, rid - 1, 2, &name_index) ||
        !vx_dotnet_column(md, 40, rid - 1, 3, &implementation)) return 0;
    const char *name;
    if (!vx_dotnet_string(md, name_index, &name)) return 0;
    *out = (vx_dotnet_manifest_resource){ .token = token, .offset = offset,
        .flags = flags, .implementation = implementation, .name = name };
    if (implementation) return 1; // linked File/AssemblyRef/ExportedType resource, not embedded here
    if (!pe->clr_resources.rva || offset > pe->clr_resources.size || pe->clr_resources.size - offset < 4 ||
        (uint64_t)pe->clr_resources.rva + offset > UINT32_MAX) return 0;
    size_t length_offset;
    if (!vx_pe_rva_to_file(md->image, md->image_size, pe,
                           pe->clr_resources.rva + offset, 4, &length_offset)) return 0;
    uint32_t length = read32(md->image + length_offset);
    if (length > pe->clr_resources.size - offset - 4 ||
        (uint64_t)pe->clr_resources.rva + offset + 4 > UINT32_MAX) return 0;
    size_t data_offset;
    if (!vx_pe_rva_to_file(md->image, md->image_size, pe,
                           pe->clr_resources.rva + offset + 4, length, &data_offset)) return 0;
    out->data = md->image + data_offset;
    out->size = length;
    return 1;
}

int vx_dotnet_method_body_get(const vx_dotnet_metadata *md, const vx_dotnet_method *method,
                              vx_dotnet_method_body *out) {
    if (!md || !method || !out || !method->rva) return 0;
    vx_pe_info pe;
    if (!vx_pe_inspect(md->image, md->image_size, &pe)) return 0;
    size_t offset;
    if (!vx_pe_rva_to_file(md->image, md->image_size, &pe, method->rva, 1, &offset)) return 0;
    const uint8_t *body = md->image + offset;
    if ((body[0] & 3) == 2) {
        uint32_t code_size = body[0] >> 2;
        if (!range_ok(md->image_size, offset + 1, code_size)) return 0;
        *out = (vx_dotnet_method_body){ .rva = method->rva, .max_stack = 8,
            .code = body + 1, .code_size = code_size };
        return 1;
    }
    if ((body[0] & 3) != 3 || !range_ok(md->image_size, offset, 12)) return 0;
    uint16_t flags_size = read16(body);
    uint32_t header_size = (uint32_t)(flags_size >> 12) * 4u;
    uint32_t code_size = read32(body + 4);
    if (header_size < 12 || !range_ok(md->image_size, offset, (uint64_t)header_size + code_size)) return 0;
    const uint8_t *sections = NULL;
    uint32_t sections_size = 0;
    if (flags_size & 0x08) {
        uint64_t section_offset = ((uint64_t)offset + header_size + code_size + 3u) & ~UINT64_C(3);
        if (section_offset > md->image_size ||
            !validate_method_sections(md, md->image + section_offset,
                                      md->image_size - (size_t)section_offset,
                                      code_size, &sections_size)) return 0;
        sections = md->image + section_offset;
    }
    *out = (vx_dotnet_method_body){ .rva = method->rva, .max_stack = read16(body + 2),
        .local_signature_token = read32(body + 8), .code = body + header_size,
        .code_size = code_size, .sections = sections, .sections_size = sections_size,
        .init_locals = !!(flags_size & 0x10),
        .has_more_sections = !!(flags_size & 0x08) };
    return 1;
}
