// Lists the WPF known-type ids (negative BAML type ids) every XAML component of the given assemblies starts elements
// with, so the CLR host's table of framework types (src/dotnet_runtime.c) can be completed before a program runs:
//   build/baml_types EDLaunch.exe FDUserControls.dll ...      -> "-442 x12" per id, most used first
#include "dotnet_metadata.h"
#include "dotnet_resources.h"
#include "dotnet_baml.h"
#include "pe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return b;
}

static uint32_t counts[1024];

static int read_7bit(const uint8_t *bytes, size_t size, size_t *cursor, uint32_t *value) {
    *value = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (*cursor >= size) return 0;
        uint8_t b = bytes[(*cursor)++];
        *value |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) return 1;
    }
    return 0;
}

static void scan_baml(const uint8_t *data, uint32_t size) {
    vx_dotnet_baml baml;
    vx_dotnet_baml_record record;
    if (!vx_dotnet_baml_open(data, size, &baml)) return;
    while (vx_dotnet_baml_next(&baml, &record) == 1) {
        if (record.type != 3 || record.payload_size < 2) continue;          // ElementStart: int16 type id
        int16_t id = (int16_t)((uint16_t)record.payload[0] | ((uint16_t)record.payload[1] << 8));
        if (id < 0 && -id < 1024) counts[-id]++;
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        size_t size;
        uint8_t *image = slurp(argv[i], &size);
        vx_pe_info pe;
        vx_dotnet_metadata md;
        if (!image || !vx_pe_inspect(image, size, &pe) || !vx_dotnet_metadata_open(image, size, &pe, &md)) { fprintf(stderr, "%s: not a managed assembly\n", argv[i]); continue; }
        for (uint32_t r = 0; r < md.row_count[40]; r++) {
            vx_dotnet_manifest_resource manifest;
            if (!vx_dotnet_manifest_resource_get(&md, &pe, 0x28000000u | (r + 1), &manifest) || !manifest.data || !manifest.name) continue;
            size_t n = strlen(manifest.name);
            if (n < 12 || strcmp(manifest.name + n - 12, ".g.resources")) continue;
            vx_dotnet_resources resources;
            if (!vx_dotnet_resources_open(manifest.data, manifest.size, &resources)) continue;
            // every resource, by position (the reader's own walk, as vx_dotnet_resource_get does by name)
            size_t array_bytes = (size_t)resources.resource_count * 4;
            size_t positions = resources.name_section_offset - 4 - array_bytes;
            for (uint32_t k = 0; k < resources.resource_count; k++) {
                uint32_t name_offset; memcpy(&name_offset, resources.bytes + positions + (size_t)k * 4, 4);
                size_t cursor = resources.name_section_offset + name_offset;
                uint32_t name_size;
                if (!read_7bit(resources.bytes, resources.data_section_offset, &cursor, &name_size)) break;
                const uint8_t *name = resources.bytes + cursor;
                cursor += name_size;
                uint32_t data_offset; memcpy(&data_offset, resources.bytes + cursor, 4);
                if (name_size < 10 || memcmp(name + name_size - 10, ".\0b\0a\0m\0l\0", 10)) continue;
                size_t value = (size_t)resources.data_section_offset + data_offset;
                uint32_t type_code;
                if (!read_7bit(resources.bytes, resources.size, &value, &type_code) || type_code != 33) continue;
                uint32_t length; memcpy(&length, resources.bytes + value, 4);
                if (value + 4 + length > resources.size) continue;
                scan_baml(resources.bytes + value + 4, length);
            }
        }
    }
    for (int id = 1; id < 1024; id++) if (counts[id]) printf("-%d x%u\n", id, counts[id]);
    return 0;
}
