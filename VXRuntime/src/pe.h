// Bounds-checked PE32 / PE32+ image header and CLR-directory reader.
#ifndef VX_PE_H
#define VX_PE_H
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t rva, size; } vx_pe_directory;

typedef struct {
    uint32_t file_version_ms, file_version_ls;
    uint32_t product_version_ms, product_version_ls;
    uint32_t file_flags_mask, file_flags;
    char file_version[256], product_version[256];
    uint8_t has_file_version_string, has_product_version_string;
} vx_pe_version_info;

typedef struct {
    uint16_t machine, optional_magic, section_count, characteristics;
    uint32_t entry_rva, image_size, headers_size;
    uint64_t image_base;
    size_t pe_offset, optional_offset, section_table_offset;
    vx_pe_directory directories[16];
    int has_cli;
    uint16_t clr_major, clr_minor;
    uint32_t clr_flags, clr_entry_point;
    vx_pe_directory clr_resources;
    char metadata_version[32];
} vx_pe_info;

// Returns nonzero only for a structurally valid PE image. A present CLR directory must also
// have a complete IMAGE_COR20_HEADER and a readable metadata root.
int vx_pe_inspect(const uint8_t *data, size_t size, vx_pe_info *out);

// Maps an RVA wholly contained in a file-backed PE header or section to its file offset.
int vx_pe_rva_to_file(const uint8_t *data, size_t size, const vx_pe_info *pe,
                      uint32_t rva, uint32_t length, size_t *offset);

// Reads the fixed VS_FIXEDFILEINFO fields from an RT_VERSION resource.
int vx_pe_version_info_get(const uint8_t *data, size_t size, const vx_pe_info *pe,
                           vx_pe_version_info *out);

#endif
