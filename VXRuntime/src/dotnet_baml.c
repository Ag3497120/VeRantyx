#include "dotnet_baml.h"
#include <stdlib.h>
#include <string.h>

static uint16_t read16(const uint8_t *p) { uint16_t v; memcpy(&v, p, sizeof v); return v; }
static uint32_t read32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }

static int read_7bit(const uint8_t *bytes, size_t size, size_t *cursor,
                     uint32_t *value, size_t *encoded_size) {
    uint32_t result = 0;
    for (unsigned i = 0; i < 5; i++) {
        if (*cursor >= size) return 0;
        uint8_t byte = bytes[(*cursor)++];
        if (i == 4 && (byte & 0xf8)) return 0;
        result |= (uint32_t)(byte & 0x7f) << (i * 7);
        if (!(byte & 0x80)) {
            *value = result;
            *encoded_size = i + 1;
            return 1;
        }
    }
    return 0;
}

static int record_is_variable(uint8_t type) {
    switch (type) {
        case 5: case 6: case 15: case 16: case 17: case 18: case 19: case 20:
        case 21: case 22: case 23: case 25: case 27: case 28: case 29: case 30:
        case 31: case 32: case 36: case 38: case 51: case 52:
            return 1;
        default:
            return 0;
    }
}

static int record_fixed_size(uint8_t type, size_t *size) {
    switch (type) {
        case 1: *size = 6; return 1;
        case 2: case 4: case 8: case 10: case 12: case 14: case 26: case 41: case 42:
        case 43: case 49:
            *size = 0; return 1;
        case 3: case 48: *size = 3; return 1;
        case 7: case 9: case 11: case 13: case 44: case 46: case 50:
            *size = 2; return 1;
        case 33: case 34: case 37: case 45: case 56: case 54:
            *size = 4; return 1;
        case 35: *size = 6; return 1;
        case 39: case 40: *size = 9; return 1;
        case 53: *size = 8; return 1;
        case 55: *size = 3; return 1;
        default: return 0;
    }
}

const char *vx_dotnet_baml_record_name(uint8_t type) {
    static const char *const names[] = {
        "Unknown", "DocumentStart", "DocumentEnd", "ElementStart", "ElementEnd",
        "Property", "PropertyCustom", "PropertyComplexStart", "PropertyComplexEnd",
        "PropertyArrayStart", "PropertyArrayEnd", "PropertyIListStart", "PropertyIListEnd",
        "PropertyIDictionaryStart", "PropertyIDictionaryEnd", "LiteralContent", "Text",
        "TextWithConverter", "RoutedEvent", "ClrEvent", "XmlnsProperty", "XmlAttribute",
        "ProcessingInstruction", "Comment", "DefTag", "DefAttribute", "EndAttributes",
        "PIMapping", "AssemblyInfo", "TypeInfo", "TypeSerializerInfo", "AttributeInfo",
        "StringInfo", "PropertyStringReference", "PropertyTypeReference", "PropertyWithExtension",
        "PropertyWithConverter", "DeferableContentStart", "DefAttributeKeyString",
        "DefAttributeKeyType", "KeyElementStart", "KeyElementEnd", "ConstructorParametersStart",
        "ConstructorParametersEnd", "ConstructorParameterType", "ConnectionId", "ContentProperty",
        "NamedElementStart", "StaticResourceStart", "StaticResourceEnd", "StaticResourceId",
        "TextWithId", "PresentationOptionsAttribute", "LineNumberAndPosition", "LinePosition",
        "OptimizedStaticResource", "PropertyWithStaticResourceId"
    };
    return type < sizeof names / sizeof names[0] ? names[type] : "Invalid";
}

int vx_dotnet_baml_open(const uint8_t *bytes, size_t size, vx_dotnet_baml *out) {
    static const uint8_t signature[] = { 'M',0, 'S',0, 'B',0, 'A',0, 'M',0, 'L',0 };
    if (!bytes || !out || size < 35 || read32(bytes) != sizeof signature ||
        memcmp(bytes + 4, signature, sizeof signature)) return 0;
    vx_dotnet_baml baml = {
        .writer_major = read16(bytes + 16), .writer_minor = read16(bytes + 18),
        .reader_major = read16(bytes + 20), .reader_minor = read16(bytes + 22),
        .updater_major = read16(bytes + 24), .updater_minor = read16(bytes + 26),
        .records = bytes + 28, .records_size = size - 28, .cursor = 0
    };
    if (baml.writer_major != 0 || baml.writer_minor != 96 ||
        baml.reader_major != 0 || baml.reader_minor != 96 || baml.records[0] != 1) return 0;
    vx_dotnet_baml_record first;
    if (vx_dotnet_baml_next(&baml, &first) != 1 || first.type != 1 || first.payload_size != 6) return 0;
    baml.cursor = 0;
    *out = baml;
    return 1;
}

int vx_dotnet_baml_next(vx_dotnet_baml *baml, vx_dotnet_baml_record *out) {
    if (!baml || !out || (!baml->records && baml->records_size) || baml->cursor > baml->records_size)
        return -1;
    if (baml->cursor == baml->records_size) return 0;
    size_t start = baml->cursor;
    uint8_t type = baml->records[baml->cursor++];
    if (!type || type >= 57 || type == 24) return -1;
    size_t payload_size;
    if (record_is_variable(type)) {
        uint32_t record_size;
        size_t encoded_size;
        if (!read_7bit(baml->records, baml->records_size, &baml->cursor,
                       &record_size, &encoded_size) || record_size < encoded_size)
            return -1;
        payload_size = (size_t)record_size - encoded_size;
    } else if (type == 47) {
        uint32_t string_size;
        size_t encoded_size;
        if (baml->records_size - baml->cursor < 2) return -1;
        size_t string_cursor = baml->cursor + 2;
        if (!read_7bit(baml->records, baml->records_size, &string_cursor,
                       &string_size, &encoded_size) || string_size > baml->records_size - string_cursor)
            return -1;
        payload_size = string_cursor + string_size - baml->cursor;
    } else if (!record_fixed_size(type, &payload_size)) {
        return -1;
    }
    if (payload_size > baml->records_size - baml->cursor) return -1;
    out->type = type;
    out->payload = baml->records + baml->cursor;
    out->payload_size = payload_size;
    out->offset = start;
    baml->cursor += payload_size;
    return 1;
}

int vx_dotnet_baml_validate(const vx_dotnet_baml *baml, uint32_t *record_count) {
    if (!baml) return 0;
    vx_dotnet_baml reader = *baml;
    reader.cursor = 0;
    uint32_t count = 0;
    int saw_start = 0, saw_end = 0;
    vx_dotnet_baml_record record;
    int result;
    while ((result = vx_dotnet_baml_next(&reader, &record)) == 1) {
        if (!count) {
            if (record.type != 1 || record.payload_size != 6) return 0;
            saw_start = 1;
        } else if (record.type == 1 || saw_end) {
            return 0;
        }
        if (record.type == 2) {
            if (!saw_start || record.payload_size != 0 || reader.cursor != reader.records_size) return 0;
            saw_end = 1;
        }
        if (count == UINT32_MAX) return 0;
        count++;
    }
    if (result < 0 || !saw_start || !saw_end) return 0;
    if (record_count) *record_count = count;
    return 1;
}

typedef struct {
    int16_t id;
    int16_t owner_type_id;
    const uint8_t *name;
    size_t name_size;
} baml_attribute;

static int read_string(const uint8_t *bytes, size_t size, size_t *cursor,
                       const uint8_t **value, size_t *value_size) {
    uint32_t length;
    size_t encoded_size;
    if (!read_7bit(bytes, size, cursor, &length, &encoded_size) ||
        length > size - *cursor) return 0;
    *value = bytes + *cursor;
    *value_size = length;
    *cursor += length;
    return 1;
}

int vx_dotnet_baml_type_info_get(const vx_dotnet_baml *baml, int16_t id,
                                 vx_dotnet_baml_type_info *out) {
    if (!baml || !out || (!baml->records && baml->records_size)) return -1;
    vx_dotnet_baml reader = *baml;
    reader.cursor = 0;
    int found = 0;
    vx_dotnet_baml_type_info result = { 0 };
    vx_dotnet_baml_record record;
    int next;
    while ((next = vx_dotnet_baml_next(&reader, &record)) == 1) {
        if (record.type != 29 && record.type != 30) continue;
        if (record.payload_size < (record.type == 29 ? 5u : 7u)) return -1;
        int16_t entry_id = (int16_t)read16(record.payload);
        uint16_t assembly_and_flags = read16(record.payload + 2);
        uint16_t raw_assembly_id = assembly_and_flags & 0x0fffu;
        int16_t assembly_id = (int16_t)((raw_assembly_id & 0x0800u) ?
                                         (raw_assembly_id | 0xf000u) : raw_assembly_id);
        uint8_t flags = (uint8_t)(assembly_and_flags >> 12);
        size_t cursor = 4, name_size;
        const uint8_t *name;
        if (!read_string(record.payload, record.payload_size, &cursor, &name, &name_size) ||
            (record.type == 30 && record.payload_size - cursor != 2) ||
            (record.type == 29 && cursor != record.payload_size)) return -1;
        if (entry_id != id) continue;
        if (found && (result.assembly_id != assembly_id || result.flags != flags ||
                      result.name_size != name_size ||
                      memcmp(result.name, name, name_size))) return -1;
        if (!found) {
            result = (vx_dotnet_baml_type_info){ .id = entry_id, .assembly_id = assembly_id,
                .flags = flags, .name = name, .name_size = name_size };
            found = 1;
        }
    }
    if (next < 0) return -1;
    if (found) *out = result;
    return found;
}

int vx_dotnet_baml_attribute_info_get(const vx_dotnet_baml *baml, int16_t id,
                                      vx_dotnet_baml_attribute_info *out) {
    if (!baml || !out || (!baml->records && baml->records_size)) return -1;
    vx_dotnet_baml reader = *baml;
    reader.cursor = 0;
    int found = 0;
    vx_dotnet_baml_attribute_info result = { 0 };
    vx_dotnet_baml_record record;
    int next;
    while ((next = vx_dotnet_baml_next(&reader, &record)) == 1) {
        if (record.type != 31) continue;
        if (record.payload_size < 6) return -1;
        int16_t entry_id = (int16_t)read16(record.payload);
        int16_t owner_id = (int16_t)read16(record.payload + 2);
        uint8_t usage = record.payload[4];
        size_t cursor = 5, name_size;
        const uint8_t *name;
        if (!read_string(record.payload, record.payload_size, &cursor, &name, &name_size) ||
            cursor != record.payload_size) return -1;
        if (entry_id != id) continue;
        if (found && (result.owner_type_id != owner_id || result.usage != usage ||
                      result.name_size != name_size || memcmp(result.name, name, name_size))) return -1;
        if (!found) {
            result = (vx_dotnet_baml_attribute_info){ .id = entry_id, .owner_type_id = owner_id,
                .usage = usage, .name = name, .name_size = name_size };
            found = 1;
        }
    }
    if (next < 0) return -1;
    if (found) *out = result;
    return found;
}

int vx_dotnet_baml_string_info_get(const vx_dotnet_baml *baml, int16_t id,
                                   vx_dotnet_baml_string_info *out) {
    if (!baml || !out || (!baml->records && baml->records_size)) return -1;
    vx_dotnet_baml reader = *baml;
    reader.cursor = 0;
    int found = 0;
    vx_dotnet_baml_string_info result = { 0 };
    vx_dotnet_baml_record record;
    int next;
    while ((next = vx_dotnet_baml_next(&reader, &record)) == 1) {
        if (record.type != 32) continue;
        if (record.payload_size < 3) return -1;
        int16_t entry_id = (int16_t)read16(record.payload);
        size_t cursor = 2, value_size;
        const uint8_t *value;
        if (!read_string(record.payload, record.payload_size, &cursor, &value, &value_size) ||
            cursor != record.payload_size) return -1;
        if (entry_id != id) continue;
        if (found && (result.value_size != value_size ||
                      memcmp(result.value, value, value_size))) return -1;
        if (!found) {
            result = (vx_dotnet_baml_string_info){ .id = entry_id, .value = value,
                .value_size = value_size };
            found = 1;
        }
    }
    if (next < 0) return -1;
    if (found) *out = result;
    return found;
}

int vx_dotnet_baml_root_type_name(const vx_dotnet_baml *baml, char *name, size_t capacity) {
    if (!baml || !name || !capacity) return 0;
    typedef struct { int16_t id; const uint8_t *name; size_t size; } type_entry;
    type_entry types[512];
    size_t type_count = 0;
    vx_dotnet_baml reader = *baml;
    reader.cursor = 0;
    vx_dotnet_baml_record record;
    int result;
    while ((result = vx_dotnet_baml_next(&reader, &record)) == 1) {
        if (record.type == 29) {
            if (record.payload_size < 5) return 0;
            int16_t id = (int16_t)read16(record.payload);
            size_t cursor = 4, type_name_size;
            const uint8_t *type_name;
            if (!read_string(record.payload, record.payload_size, &cursor,
                             &type_name, &type_name_size) || cursor != record.payload_size ||
                type_count == sizeof types / sizeof types[0]) return 0;
            int duplicate = 0;
            for (size_t i = 0; i < type_count; i++) {
                if (types[i].id != id) continue;
                if (types[i].size != type_name_size || memcmp(types[i].name, type_name, type_name_size)) return 0;
                duplicate = 1;
                break;
            }
            if (!duplicate) types[type_count++] = (type_entry){ id, type_name, type_name_size };
            continue;
        }
        if (record.type != 3) continue;
        if (record.payload_size < 3) return 0;
        int16_t root_id = (int16_t)read16(record.payload);
        if (root_id < 0) return 0;
        for (size_t i = 0; i < type_count; i++) {
            if (types[i].id != root_id) continue;
            if (!types[i].size || types[i].size >= capacity ||
                memchr(types[i].name, 0, types[i].size)) return 0;
            memcpy(name, types[i].name, types[i].size);
            name[types[i].size] = 0;
            return 1;
        }
        return 0;
    }
    return 0;
}

static const baml_attribute *find_attribute(const baml_attribute *attributes,
                                            size_t count, int16_t id) {
    for (size_t i = 0; i < count; i++)
        if (attributes[i].id == id) return &attributes[i];
    return NULL;
}

static int find_string_property_internal(const vx_dotnet_baml *baml, const char *name,
                                         int restrict_owner, int16_t owner_type_id,
                                         size_t after_record_offset,
                                         vx_dotnet_baml_string_property *out) {
    if (!baml || !name || !out || (!baml->records && baml->records_size)) return -1;
    vx_dotnet_baml reader = *baml;
    reader.cursor = 0;
    size_t name_size = strlen(name);
    baml_attribute *attributes = NULL;
    size_t attribute_count = 0, attribute_capacity = 0;
    int16_t element_types[512];
    size_t element_count = 0;
    vx_dotnet_baml_record record;
    int result;
    while ((result = vx_dotnet_baml_next(&reader, &record)) == 1) {
        if (record.type == 31) {
            if (record.payload_size < 5) { result = -1; break; }
            int16_t id = (int16_t)read16(record.payload);
            int16_t owner = (int16_t)read16(record.payload + 2);
            size_t cursor = 5;
            const uint8_t *attribute_name;
            size_t attribute_name_size;
            if (!read_string(record.payload, record.payload_size, &cursor,
                             &attribute_name, &attribute_name_size) || cursor != record.payload_size) {
                result = -1; break;
            }
            const baml_attribute *existing = find_attribute(attributes, attribute_count, id);
            if (existing) {
                if (existing->owner_type_id != owner || existing->name_size != attribute_name_size ||
                    memcmp(existing->name, attribute_name, attribute_name_size)) {
                    result = -1; break;
                }
                continue;
            }
            if (attribute_count == attribute_capacity) {
                size_t capacity = attribute_capacity ? attribute_capacity * 2 : 16;
                if (capacity < attribute_capacity || capacity > SIZE_MAX / sizeof *attributes) {
                    result = -1; break;
                }
                baml_attribute *grown = realloc(attributes, capacity * sizeof *attributes);
                if (!grown) { result = -1; break; }
                attributes = grown;
                attribute_capacity = capacity;
            }
            attributes[attribute_count++] = (baml_attribute){
                .id = id, .owner_type_id = owner, .name = attribute_name,
                .name_size = attribute_name_size
            };
        } else if (record.type == 3 || record.type == 39 || record.type == 40 || record.type == 47) {
            if (record.payload_size < 2 || element_count == sizeof element_types / sizeof element_types[0]) {
                result = -1; break;
            }
            element_types[element_count++] = (int16_t)read16(record.payload);
        } else if (record.type == 4 || record.type == 41) {
            if (!element_count) { result = -1; break; }
            element_count--;
        } else if (record.type == 5 || record.type == 36) {
            if (record.payload_size < 3) { result = -1; break; }
            int16_t id = (int16_t)read16(record.payload);
            const baml_attribute *attribute = find_attribute(attributes, attribute_count, id);
            size_t cursor = 2;
            const uint8_t *value;
            size_t value_size;
            if (!read_string(record.payload, record.payload_size, &cursor, &value, &value_size)) {
                result = -1; break;
            }
            if (record.type == 36) {
                if (record.payload_size - cursor != 2) { result = -1; break; }
                cursor += 2;
            } else if (cursor != record.payload_size) {
                result = -1; break;
            }
            if (attribute && element_count &&
                attribute->owner_type_id == element_types[element_count - 1] &&
                (!restrict_owner || attribute->owner_type_id == owner_type_id) &&
                record.offset >= after_record_offset &&
                attribute->name_size == name_size && !memcmp(attribute->name, name, name_size)) {
                *out = (vx_dotnet_baml_string_property){
                    .owner_type_id = attribute->owner_type_id, .value = value,
                    .value_size = value_size, .record_offset = record.offset
                };
                free(attributes);
                return 1;
            }
        }
    }
    free(attributes);
    if (result < 0) return -1;
    return 0;
}

int vx_dotnet_baml_find_string_property(const vx_dotnet_baml *baml, const char *name,
                                        vx_dotnet_baml_string_property *out) {
    return find_string_property_internal(baml, name, 0, 0, 0, out);
}

int vx_dotnet_baml_find_string_property_for_owner(const vx_dotnet_baml *baml, const char *name,
                                                   int16_t owner_type_id, size_t after_record_offset,
                                                   vx_dotnet_baml_string_property *out) {
    return find_string_property_internal(baml, name, 1, owner_type_id,
                                         after_record_offset, out);
}
