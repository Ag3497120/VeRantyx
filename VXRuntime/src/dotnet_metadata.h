// Bounds-checked ECMA-335 CLR metadata and method-body reader. The image and all
// returned heap/method pointers are borrowed from the caller's PE file buffer.
#ifndef VX_DOTNET_METADATA_H
#define VX_DOTNET_METADATA_H
#include "pe.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *image;
    size_t image_size;
    const uint8_t *metadata;
    size_t metadata_size;
    const uint8_t *tables;
    size_t tables_size;
    const uint8_t *strings;
    size_t strings_size;
    const uint8_t *blob;
    size_t blob_size;
    const uint8_t *guid;
    size_t guid_size;
    const uint8_t *user_strings;
    size_t user_strings_size;
    uint32_t row_count[45];
    uint32_t row_size[45];
    size_t table_offset[45];
    uint64_t valid_tables;
    uint32_t entry_point_token;
    uint8_t heap_sizes;
    uint8_t unoptimized_tables;
} vx_dotnet_metadata;

typedef struct {
    uint32_t token;
    uint32_t rva;
    uint16_t impl_flags;
    uint16_t flags;
    uint32_t name_index;
    uint32_t signature_index;
    uint32_t parameter_list;
    uint32_t declaring_type_token;
    const char *name;
    const char *declaring_type_name;
    const char *declaring_type_namespace;
} vx_dotnet_method;

typedef struct {
    uint32_t token;
    uint32_t parent_token;
    uint32_t name_index;
    uint32_t signature_index;
    const char *name;
    const char *parent_name;
    const char *parent_namespace;
} vx_dotnet_member_ref;

typedef struct {
    uint32_t token;
    uint16_t flags;
    uint32_t name_index;
    uint32_t signature_index;
    const char *name;
} vx_dotnet_field;

typedef struct {
    uint32_t token;
    uint32_t offset;
    uint32_t flags;
    uint32_t implementation;
    const char *name;
    const uint8_t *data;
    uint32_t size;
} vx_dotnet_manifest_resource;

typedef struct {
    uint32_t rva;
    uint16_t max_stack;
    uint32_t local_signature_token;
    const uint8_t *code;
    uint32_t code_size;
    const uint8_t *sections;
    uint32_t sections_size;
    int init_locals;
    int has_more_sections;
} vx_dotnet_method_body;

// Opens metadata only for a structurally valid managed PE image.
int vx_dotnet_metadata_open(const uint8_t *image, size_t image_size,
                            const vx_pe_info *pe, vx_dotnet_metadata *out);

// Read a schema-checked metadata table cell (row and column are zero-based).
int vx_dotnet_column(const vx_dotnet_metadata *md, uint8_t table, uint32_t row,
                     uint8_t column, uint32_t *value);
int vx_dotnet_string(const vx_dotnet_metadata *md, uint32_t index, const char **out);
int vx_dotnet_blob(const vx_dotnet_metadata *md, uint32_t index,
                   const uint8_t **out, uint32_t *length);
int vx_dotnet_user_string(const vx_dotnet_metadata *md, uint32_t index,
                          const uint8_t **utf16le, uint32_t *code_units);
int vx_dotnet_type_name(const vx_dotnet_metadata *md, uint32_t token,
                        const char **name_space, const char **name);
int vx_dotnet_type_assembly_ref(const vx_dotnet_metadata *md, uint32_t type_ref_token,
                                uint32_t *assembly_ref_token);
int vx_dotnet_assembly_ref_name(const vx_dotnet_metadata *md, uint32_t assembly_ref_token,
                                const char **name);
int vx_dotnet_method_get(const vx_dotnet_metadata *md, uint32_t token, vx_dotnet_method *out);
int vx_dotnet_member_ref_get(const vx_dotnet_metadata *md, uint32_t token, vx_dotnet_member_ref *out);
int vx_dotnet_field_get(const vx_dotnet_metadata *md, uint32_t token, vx_dotnet_field *out);
int vx_dotnet_manifest_resource_get(const vx_dotnet_metadata *md, const vx_pe_info *pe,
                                    uint32_t token, vx_dotnet_manifest_resource *out);
int vx_dotnet_method_body_get(const vx_dotnet_metadata *md, const vx_dotnet_method *method,
                              vx_dotnet_method_body *out);

#endif
