// Bounds-checked reader for WPF BAML headers and record framing.
#ifndef VX_DOTNET_BAML_H
#define VX_DOTNET_BAML_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *records;
    size_t records_size;
    size_t cursor;
    uint16_t writer_major, writer_minor;
    uint16_t reader_major, reader_minor;
    uint16_t updater_major, updater_minor;
} vx_dotnet_baml;

typedef struct {
    uint8_t type;
    const uint8_t *payload;
    size_t payload_size;
    size_t offset;
} vx_dotnet_baml_record;

typedef struct {
    int16_t owner_type_id;
    const uint8_t *value;
    size_t value_size;
    size_t record_offset;
} vx_dotnet_baml_string_property;

typedef struct {
    int16_t id;
    int16_t assembly_id;
    uint8_t flags;
    const uint8_t *name;
    size_t name_size;
} vx_dotnet_baml_type_info;

typedef struct {
    int16_t id;
    int16_t owner_type_id;
    uint8_t usage;
    const uint8_t *name;
    size_t name_size;
} vx_dotnet_baml_attribute_info;

typedef struct {
    int16_t id;
    const uint8_t *value;
    size_t value_size;
} vx_dotnet_baml_string_info;

// Validates the 28-byte MSBAML header and the initial DocumentStart record.
int vx_dotnet_baml_open(const uint8_t *bytes, size_t size, vx_dotnet_baml *out);

// Returns 1 for a record, 0 at the end of the stream, or -1 for invalid framing.
int vx_dotnet_baml_next(vx_dotnet_baml *baml, vx_dotnet_baml_record *out);

// Checks the complete record stream, including one leading DocumentStart and a trailing DocumentEnd.
int vx_dotnet_baml_validate(const vx_dotnet_baml *baml, uint32_t *record_count);

// Resolves the first ElementStart against the preceding custom TypeInfo records.
// The output is the qualified XML type name when the root is a custom BAML type.
int vx_dotnet_baml_root_type_name(const vx_dotnet_baml *baml, char *name, size_t capacity);

// Looks up BAML map-table entries. Names borrow the original BAML stream.
// Returns 1 when found, 0 when absent, and -1 when the map table is malformed
// or contains conflicting duplicate IDs.
int vx_dotnet_baml_type_info_get(const vx_dotnet_baml *baml, int16_t id,
                                 vx_dotnet_baml_type_info *out);
int vx_dotnet_baml_attribute_info_get(const vx_dotnet_baml *baml, int16_t id,
                                      vx_dotnet_baml_attribute_info *out);
// Looks up a StringInfo table value. The returned bytes borrow the original BAML stream.
// Returns 1 when found, 0 when absent, and -1 for malformed or conflicting duplicate entries.
int vx_dotnet_baml_string_info_get(const vx_dotnet_baml *baml, int16_t id,
                                   vx_dotnet_baml_string_info *out);

// Finds the first inline string property in its owning element. The returned value borrows the BAML stream.
// Returns 1 when found, 0 when absent, and -1 for malformed or unsupported framing.
int vx_dotnet_baml_find_string_property(const vx_dotnet_baml *baml, const char *name,
                                        vx_dotnet_baml_string_property *out);

// Finds an inline string property owned by a specific BAML type, starting at or after
// after_record_offset. Pass 0 to start at the beginning; advance with record_offset + 1.
int vx_dotnet_baml_find_string_property_for_owner(const vx_dotnet_baml *baml, const char *name,
                                                   int16_t owner_type_id, size_t after_record_offset,
                                                   vx_dotnet_baml_string_property *out);

const char *vx_dotnet_baml_record_name(uint8_t type);

#endif
