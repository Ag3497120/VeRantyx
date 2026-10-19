// Bounds-checked reader for the version-2 .resources format used by managed assemblies.
#ifndef VX_DOTNET_RESOURCES_H
#define VX_DOTNET_RESOURCES_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *bytes;
    size_t size;
    size_t name_section_offset;
    uint32_t data_section_offset;
    uint32_t resource_count;
    uint32_t type_count;
} vx_dotnet_resources;

typedef struct {
    const uint8_t *name_utf16le;
    uint32_t name_size_bytes;
    uint32_t type_code;
    const uint8_t *value;
    size_t value_size;
    const uint8_t *stream_data;
    uint32_t stream_size;
} vx_dotnet_resource;

// Opens a ResourceReader-compatible .resources blob. Returned pointers borrow the input buffer.
int vx_dotnet_resources_open(const uint8_t *bytes, size_t size, vx_dotnet_resources *out);

// Looks up an exact UTF-8 resource name. Stream and byte-array values are exposed as raw bytes.
int vx_dotnet_resource_get(const vx_dotnet_resources *resources, const char *name,
                           vx_dotnet_resource *out);

#endif
