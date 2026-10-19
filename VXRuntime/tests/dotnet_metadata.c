#include "dotnet_metadata.h"
#include "dotnet_exec.h"
#include "dotnet_il.h"
#include "dotnet_baml.h"
#include "dotnet_resources.h"
#include "vx.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "dotnet metadata test failed: %s\n", m); exit(1); } } while (0)

// dotnet_runtime.c includes the native WPF host path. Framework file calls use
// isolated path and DeleteFileW stubs so the managed test never touches host data.
vx_process vx_proc;
static DWORD test_last_error;
static uint32_t fixture_path_user_string;
static uint32_t fixture_content_user_string;
static uint32_t fixture_directory_user_string;
static uint32_t fixture_version_file_user_string;

void vx_log(const char *fmt, ...) {
    (void)fmt;
}

static BOOL WINAPI test_delete_file_w(const WCHAR *path) {
    static const WCHAR expected[] = {
        'C', ':', '\\', 'v', 'x', 'r', 't', '-', 'w', 'r', 'i', 't', 'e', 'a', 'l', 'l',
        't', 'e', 'x', 't', '-', 't', 'e', 's', 't', '.', 't', 'x', 't', 0
    };
    if (!path || memcmp(path, expected, sizeof expected)) {
        test_last_error = 87;
        return 0;
    }
    if (!unlink("build/vxrt-dotnet-writealltext-test.txt")) {
        test_last_error = 0;
        return 1;
    }
    test_last_error = errno == ENOENT ? 2u : vx_error_from_errno(errno);
    return 0;
}

void *vx_find_export(const char *dll, const char *name) {
    if (dll && name && !strcmp(dll, "kernel32.dll") && !strcmp(name, "DeleteFileW"))
        return (void *)test_delete_file_w;
    return NULL;
}

const char *vx_special_folder_path(int csidl) {
    return csidl == 0x1c ? "C:\\users\\vxrt\\AppData\\Local" : NULL;
}

const vx_image *vx_image_at(uint64_t pc) {
    (void)pc;
    return NULL;
}

void vx_windows_path(const char *unix_path, char *out, size_t capacity) {
    if (!out || !capacity) return;
    snprintf(out, capacity, "%s", unix_path ? unix_path : "");
}

int vx_unix_path(const char *windows_path, char *out, size_t capacity) {
    if (!windows_path || !out || !capacity) return 0;
    const char *mapped = !strcmp(windows_path, "C:\\vxrt-writealltext-test.txt") ?
        "build/vxrt-dotnet-writealltext-test.txt" :
        !strcmp(windows_path, "C:\\vxrt-dir-create-test\\child") ?
        "build/vxrt-dotnet-directory-test/child" :
        !strcmp(windows_path, "C:\\users\\vxrt\\AppData\\Local") ?
        "build/vxrt-dotnet-localappdata-test" :
        !strcmp(windows_path, "C:\\vxrt-versioninfo-test.exe") ?
        "build/vxrt-dotnet-versioninfo-test.exe" : NULL;
    if (!mapped) return 0;
    int length = snprintf(out, capacity, "%s", mapped);
    return length >= 0 && (size_t)length < capacity;
}

int vx_unix_path_write(const char *windows_path, char *out, size_t capacity, int keep_contents) {
    (void)keep_contents;
    return vx_unix_path(windows_path, out, capacity);
}

void vx_set_last_error(DWORD error) {
    test_last_error = error;
}

DWORD vx_get_last_error(void) {
    return test_last_error;
}

DWORD vx_error_from_errno(int error) {
    return error == EACCES || error == EPERM ? 5u : error == ENOENT ? 2u : 1u;
}

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, sizeof v); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof v); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, sizeof v); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static uint32_t align4(uint32_t n) { return (n + 3u) & ~3u; }

static uint32_t stream_header(uint8_t *root, uint32_t *cursor, const char *name,
                              uint32_t offset, uint32_t size) {
    uint32_t at = *cursor;
    put32(root + at, offset); put32(root + at + 4, size);
    size_t n = strlen(name) + 1;
    memcpy(root + at + 8, name, n);
    *cursor += 8 + align4((uint32_t)n);
    return at;
}

static uint16_t fixture_string_index(const char *heap, const char *value) {
    for (const char *entry = heap + 1; *entry; entry += strlen(entry) + 1) {
        if (strcmp(entry, value)) continue;
        ptrdiff_t index = entry - heap;
        CHECK(index <= UINT16_MAX, "fixture string heap index exceeds two-byte metadata columns");
        return (uint16_t)index;
    }
    CHECK(0, "fixture string is missing from the #Strings heap");
    return 0;
}

static uint32_t fixture_add_user_string(uint8_t *heap, size_t capacity,
                                        size_t *cursor, const char *text) {
    size_t length = strlen(text);
    CHECK(length <= 62 && *cursor + length * 2 + 2 <= capacity,
          "fixture #US string exceeds its bounded heap");
    uint32_t index = (uint32_t)*cursor;
    heap[(*cursor)++] = (uint8_t)(length * 2 + 1);
    for (size_t i = 0; i < length; i++) {
        heap[(*cursor)++] = (uint8_t)text[i];
        heap[(*cursor)++] = 0;
    }
    heap[(*cursor)++] = 0;
    return index;
}

static uint8_t *make_fixture(size_t *length) {
    const size_t size = 0xc00;
    uint8_t *image = calloc(1, size);
    CHECK(image, "fixture allocation failed");
    image[0] = 'M'; image[1] = 'Z'; put32(image + 0x3c, 0x80);
    memcpy(image + 0x80, "PE\0\0", 4);
    uint8_t *coff = image + 0x84, *opt = image + 0x98;
    put16(coff, 0x014c); put16(coff + 2, 1); put16(coff + 16, 0xe0); put16(coff + 18, 0x0102);
    put16(opt, 0x010b); put32(opt + 16, 0x1000); put32(opt + 28, 0x400000);
    put32(opt + 56, 0x2000); put32(opt + 60, 0x200); put32(opt + 92, 16);
    put32(opt + 96 + 14 * 8, 0x1000); put32(opt + 96 + 14 * 8 + 4, 72);
    uint8_t *section = image + 0x178;
    put32(section + 8, 0xa00); put32(section + 12, 0x1000);
    put32(section + 16, 0xa00); put32(section + 20, 0x200);

    uint8_t *cor = image + 0x200;
    put32(cor, 72); put16(cor + 4, 2); put16(cor + 6, 5);
    put32(cor + 8, 0x1100); put32(cor + 12, 0x340); put32(cor + 16, 1);
    put32(cor + 20, 0x06000001);
    uint8_t *root = image + 0x300;
    put32(root, 0x424a5342); put16(root + 4, 1); put16(root + 6, 1);
    put32(root + 12, 12); memcpy(root + 16, "v4.0.30319", 10);
    put16(root + 28, 0); put16(root + 30, 5);

    const uint32_t table_offset = 108;
    const uint32_t table_size = 204;
    const uint32_t strings_offset = align4(table_offset + table_size);
    const char strings[] = "\0Module\0Entry\0Test\0Main\0Fixture\0File\0WriteAllText\0System.IO\0mscorlib\0Delete\0CreateDirectory\0Directory\0Exists\0Environment\0System\0GetFolderPath\0FileVersionInfo\0System.Diagnostics\0GetVersionInfo\0get_FilePrivatePart\0";
    const uint32_t strings_size = sizeof strings;
    const uint32_t blob_offset = align4(strings_offset + strings_size);
    const uint8_t blob[] = { 0, 3, 0, 0, 8, 5, 0, 2, 1, 14, 14,
                             4, 0, 1, 1, 14, 5, 0, 1, 0x12, 9, 14,
                             4, 0, 1, 2, 14, 5, 0, 1, 14, 0x11, 0x0d,
                             5, 0, 1, 0x12, 0x11, 14, 3, 0x20, 0, 8 };
    const uint32_t guid_offset = align4(blob_offset + sizeof blob);
    const uint32_t guid_size = 16;
    uint8_t user_strings[256] = {0};
    size_t user_strings_size = 1;
    fixture_path_user_string = fixture_add_user_string(user_strings, sizeof user_strings,
        &user_strings_size, "C:\\vxrt-writealltext-test.txt");
    fixture_content_user_string = fixture_add_user_string(user_strings, sizeof user_strings,
        &user_strings_size, "new managed contents");
    fixture_directory_user_string = fixture_add_user_string(user_strings, sizeof user_strings,
        &user_strings_size, "C:\\vxrt-dir-create-test\\child");
    fixture_version_file_user_string = fixture_add_user_string(user_strings, sizeof user_strings,
        &user_strings_size, "C:\\vxrt-versioninfo-test.exe");
    const uint32_t user_strings_offset = align4(guid_offset + guid_size);
    uint32_t stream_cursor = 32;
    stream_header(root, &stream_cursor, "#~", table_offset, table_size);
    stream_header(root, &stream_cursor, "#Strings", strings_offset, strings_size);
    stream_header(root, &stream_cursor, "#Blob", blob_offset, sizeof blob);
    stream_header(root, &stream_cursor, "#GUID", guid_offset, guid_size);
    stream_header(root, &stream_cursor, "#US", user_strings_offset, (uint32_t)user_strings_size);
    CHECK(stream_cursor == table_offset, "fixture stream headers have wrong size");

    uint8_t *tables = root + table_offset;
    tables[4] = 2; tables[5] = 0; tables[6] = 0; tables[7] = 1;
    put64(tables + 8, (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 6) |
                      (1ull << 10) | (1ull << 32) | (1ull << 35));
    size_t p = 24;
    put32(tables + p, 1); p += 4; put32(tables + p, 4); p += 4;
    put32(tables + p, 1); p += 4; put32(tables + p, 1); p += 4;
    put32(tables + p, 8); p += 4; put32(tables + p, 1); p += 4; put32(tables + p, 1); p += 4;
    put16(tables + p, 0); put16(tables + p + 2, 1); put16(tables + p + 4, 1);
    put16(tables + p + 6, 0); put16(tables + p + 8, 0); p += 10;
    put16(tables + p, 6); put16(tables + p + 2, fixture_string_index(strings, "File"));
    put16(tables + p + 4, fixture_string_index(strings, "System.IO")); p += 6;
    put16(tables + p, 6); put16(tables + p + 2, fixture_string_index(strings, "Directory"));
    put16(tables + p + 4, fixture_string_index(strings, "System.IO")); p += 6;
    put16(tables + p, 6); put16(tables + p + 2, fixture_string_index(strings, "Environment"));
    put16(tables + p + 4, fixture_string_index(strings, "System")); p += 6;
    put16(tables + p, 6); put16(tables + p + 2, fixture_string_index(strings, "FileVersionInfo"));
    put16(tables + p + 4, fixture_string_index(strings, "System.Diagnostics")); p += 6;
    put32(tables + p, 1); put16(tables + p + 4, fixture_string_index(strings, "Entry"));
    put16(tables + p + 6, fixture_string_index(strings, "Test"));
    put16(tables + p + 8, 0); put16(tables + p + 10, 1); put16(tables + p + 12, 1); p += 14;
    put32(tables + p, 0x1500); put16(tables + p + 4, 0); put16(tables + p + 6, 0x16);
    put16(tables + p + 8, fixture_string_index(strings, "Main"));
    put16(tables + p + 10, 1); put16(tables + p + 12, 1); p += 14;
    put16(tables + p, 9); put16(tables + p + 2, fixture_string_index(strings, "WriteAllText"));
    put16(tables + p + 4, 5); p += 6;
    put16(tables + p, 9); put16(tables + p + 2, fixture_string_index(strings, "Delete"));
    put16(tables + p + 4, 11); p += 6;
    put16(tables + p, 17); put16(tables + p + 2, fixture_string_index(strings, "CreateDirectory"));
    put16(tables + p + 4, 16); p += 6;
    put16(tables + p, 9); put16(tables + p + 2, fixture_string_index(strings, "Exists"));
    put16(tables + p + 4, 22); p += 6;
    put16(tables + p, 25); put16(tables + p + 2, fixture_string_index(strings, "GetFolderPath"));
    put16(tables + p + 4, 27); p += 6;
    put16(tables + p, 17); put16(tables + p + 2, fixture_string_index(strings, "Exists"));
    put16(tables + p + 4, 22); p += 6;
    put16(tables + p, 33); put16(tables + p + 2, fixture_string_index(strings, "GetVersionInfo"));
    put16(tables + p + 4, 33); p += 6;
    put16(tables + p, 33); put16(tables + p + 2, fixture_string_index(strings, "get_FilePrivatePart"));
    put16(tables + p + 4, 39); p += 6;
    put32(tables + p, 0); put16(tables + p + 4, 1); put16(tables + p + 6, 0);
    put16(tables + p + 8, 0); put16(tables + p + 10, 0);
    put32(tables + p + 12, 0); put16(tables + p + 16, 0);
    put16(tables + p + 18, fixture_string_index(strings, "Fixture"));
    put16(tables + p + 20, 0); p += 22;
    put16(tables + p, 4); put16(tables + p + 2, 0); put16(tables + p + 4, 0);
    put16(tables + p + 6, 0); put32(tables + p + 8, 0); put16(tables + p + 12, 0);
    put16(tables + p + 14, fixture_string_index(strings, "mscorlib"));
    put16(tables + p + 16, 0); put16(tables + p + 18, 0); p += 20;
    CHECK(p == table_size, "fixture metadata table size mismatch");

    memcpy(root + strings_offset, strings, sizeof strings);
    memcpy(root + blob_offset, blob, sizeof blob);
    memset(root + guid_offset, 0x5a, guid_size);
    memcpy(root + user_strings_offset, user_strings, user_strings_size);
    put32(opt + 96 + 2 * 8, 0x1800); put32(opt + 96 + 2 * 8 + 4, 0x100);
    uint8_t *resource = image + 0xa00;
    put16(resource + 14, 1);
    put32(resource + 16, 16); put32(resource + 20, 0x80000018u);
    put16(resource + 0x18 + 14, 1);
    put32(resource + 0x18 + 16, 1); put32(resource + 0x18 + 20, 0x80000030u);
    put16(resource + 0x30 + 14, 1);
    put32(resource + 0x30 + 16, 0x409); put32(resource + 0x30 + 20, 0x48);
    put32(resource + 0x48, 0x1880); put32(resource + 0x48 + 4, 92);
    uint8_t *version = image + 0xa80;
    put16(version, 92); put16(version + 2, 52); put16(version + 4, 0);
    static const char version_key[] = "VS_VERSION_INFO";
    for (size_t i = 0; i < sizeof version_key; i++) put16(version + 6 + i * 2, (uint8_t)version_key[i]);
    uint8_t *fixed = version + 40;
    put32(fixed, 0xfeef04bd); put32(fixed + 4, 0x00010000);
    put32(fixed + 8, (1u << 16) | 2u); put32(fixed + 12, (3u << 16) | 4u);
    put32(fixed + 16, (5u << 16) | 6u); put32(fixed + 20, (7u << 16) | 8u);
    put32(fixed + 24, 0x3f); put32(fixed + 28, 1);
    uint8_t *body = image + 0x700;
    body[0] = 0x1a; body[1] = 0x1f; body[2] = 20; body[3] = 0x1f;
    body[4] = 22; body[5] = 0x58; body[6] = 0x2a;
    *length = size;
    return image;
}

static void dump_method(const vx_dotnet_metadata *md, uint32_t token) {
    vx_dotnet_method method; vx_dotnet_method_body body;
    if (!vx_dotnet_method_get(md, token, &method) || !vx_dotnet_method_body_get(md, &method, &body)) return;
    printf("  method %08x %s.%s IL (%u bytes):", token,
           method.declaring_type_name ? method.declaring_type_name : "?", method.name, body.code_size);
    for (uint32_t i = 0; i < body.code_size; i++) printf(" %02x", body.code[i]);
    putchar('\n');
    uint32_t cursor = 0; vx_dotnet_instruction ins;
    while (vx_dotnet_il_next(body.code, body.code_size, &cursor, &ins) == 1) {
        if (ins.opcode == 0x0072 && ins.operand_size == 4) {
            uint32_t string_token = get32(ins.operand), units; const uint8_t *text;
            if ((string_token >> 24) == 0x70 &&
                vx_dotnet_user_string(md, string_token & 0x00ffffffu, &text, &units)) {
                printf("    IL_%04x ldstr ", ins.offset);
                for (uint32_t i = 0; i < units; i++) {
                    uint16_t ch = (uint16_t)(text[i * 2] | ((uint16_t)text[i * 2 + 1] << 8));
                    putchar(ch >= 0x20 && ch <= 0x7e ? (int)ch : '?');
                }
                putchar('\n');
            }
        }
        int method_operand = ins.opcode == 0x0028 || ins.opcode == 0x0029 || ins.opcode == 0x006f ||
                             ins.opcode == 0x0073 || ins.opcode == 0xfe06 || ins.opcode == 0xfe07;
        int field_operand = ins.opcode >= 0x007b && ins.opcode <= 0x0080;
        if ((!method_operand && !field_operand) || ins.operand_kind != VX_IL_OPERAND_TOKEN || ins.operand_size != 4) continue;
        uint32_t ref; memcpy(&ref, ins.operand, sizeof ref);
        if (method_operand && (ref >> 24) == 6) {
            vx_dotnet_method target;
            if (vx_dotnet_method_get(md, ref, &target))
                printf("    IL_%04x %s %s.%s token=%08x\n", ins.offset,
                       ins.opcode == 0x0073 ? "newobj" : ins.opcode == 0x006f ? "callvirt" : "call",
                       target.declaring_type_name ? target.declaring_type_name : "?", target.name, ref);
        } else if (method_operand && (ref >> 24) == 10) {
            vx_dotnet_member_ref target;
            if (vx_dotnet_member_ref_get(md, ref, &target))
                printf("    IL_%04x %s %s.%s::%s token=%08x\n", ins.offset,
                       ins.opcode == 0x0073 ? "newobj" : ins.opcode == 0x006f ? "callvirt" : "call",
                       target.parent_namespace, target.parent_name, target.name, ref);
        } else if (field_operand && (ref >> 24) == 4) {
            vx_dotnet_field field;
            if (vx_dotnet_field_get(md, ref, &field))
                printf("    IL_%04x field-op %s token=%08x\n", ins.offset, field.name, ref);
        }
    }
}

static void inspect_file(const char *path) {
    FILE *f = fopen(path, "rb"); CHECK(f, "could not open managed PE sample");
    CHECK(!fseek(f, 0, SEEK_END), "could not seek managed PE sample");
    long n = ftell(f); CHECK(n > 0 && !fseek(f, 0, SEEK_SET), "could not size managed PE sample");
    uint8_t *image = malloc((size_t)n); CHECK(image, "sample allocation failed");
    CHECK(fread(image, 1, (size_t)n, f) == (size_t)n, "could not read managed PE sample"); fclose(f);
    vx_pe_info pe; vx_dotnet_metadata md;
    CHECK(vx_pe_inspect(image, (size_t)n, &pe) && pe.has_cli, "sample is not a valid CLI image");
    CHECK(vx_dotnet_metadata_open(image, (size_t)n, &pe, &md), "could not parse real CLR metadata");
    uint32_t assembly_name;
    CHECK(vx_dotnet_column(&md, 32, 0, 7, &assembly_name), "assembly name column unavailable");
    const char *assembly;
    CHECK(vx_dotnet_string(&md, assembly_name, &assembly), "assembly name is invalid");
    vx_dotnet_method method;
    CHECK(vx_dotnet_method_get(&md, md.entry_point_token, &method), "entry-point method is invalid");
    printf("%s: assembly=%s entry=%s%s%s.%s token=%08x rva=%08x\n", path, assembly,
           method.declaring_type_namespace ? method.declaring_type_namespace : "",
           method.declaring_type_namespace && method.declaring_type_namespace[0] ? "." : "",
           method.declaring_type_name ? method.declaring_type_name : "?",
           method.name, method.token, method.rva);
    printf("  AssemblyRef (%u):", md.row_count[35]);
    for (uint32_t i = 0; i < md.row_count[35]; i++) {
        uint32_t name_index; const char *name;
        if (vx_dotnet_column(&md, 35, i, 6, &name_index) && vx_dotnet_string(&md, name_index, &name))
            printf(" %s", name);
    }
    putchar('\n');
    printf("  ManifestResource (%u), CLI resource dir=%08x+%08x:\n", md.row_count[40],
           pe.clr_resources.rva, pe.clr_resources.size);
    for (uint32_t i = 0; i < md.row_count[40]; i++) {
        vx_dotnet_manifest_resource resource;
        if (!vx_dotnet_manifest_resource_get(&md, &pe, 0x28000000u | (i + 1), &resource)) continue;
        printf("    %s offset=%08x flags=%08x implementation=%08x", resource.name, resource.offset,
               resource.flags, resource.implementation);
        if (resource.data) {
            printf(" data=%08x bytes", resource.size);
            if (resource.size >= 4) printf(" magic=%08x", get32(resource.data));
        }
        putchar('\n');
        if (resource.data) {
            if (!strcmp(resource.name, "EDLaunch.g.resources"))
                CHECK(resource.size > 0 && resource.size >= 4 && get32(resource.data) == 0xbeefcace,
                      "WPF .g.resources payload was not resolved from the CLR resources directory");
            if (!strcmp(resource.name, "EDLaunch.g.resources")) {
                vx_dotnet_resources items;
                vx_dotnet_resource item;
                CHECK(vx_dotnet_resources_open(resource.data, resource.size, &items) &&
                      items.resource_count == 64,
                      "WPF .g.resources directory could not be parsed");
                CHECK(vx_dotnet_resource_get(&items, "app.baml", &item) && item.type_code == 33 &&
                      item.stream_data && item.stream_size > 28,
                      "application BAML stream could not be resolved");
                uint32_t app_stream_size = item.stream_size;
                vx_dotnet_baml baml;
                CHECK(vx_dotnet_baml_open(item.stream_data, item.stream_size, &baml),
                      "application BAML header is unsupported or malformed");
                uint32_t baml_records = 0;
                CHECK(vx_dotnet_baml_validate(&baml, &baml_records) && baml_records == 64,
                      "application BAML records are truncated, malformed, or unsupported");
                vx_dotnet_baml_string_property source;
                CHECK(vx_dotnet_baml_find_string_property(&baml, "Source", &source) == 1 &&
                      source.owner_type_id == -524 &&
                      source.value_size == strlen("Styles/WindowStyles.xaml") &&
                      !memcmp(source.value, "Styles/WindowStyles.xaml", source.value_size),
                      "first merged ResourceDictionary.Source was not decoded");
                size_t source_after_offset = 0;
                uint32_t source_count = 0;
                int source_result;
                while ((source_result = vx_dotnet_baml_find_string_property_for_owner(
                            &baml, "Source", (int16_t)-524, source_after_offset, &source)) == 1) {
                    CHECK(source.record_offset != SIZE_MAX,
                          "merged ResourceDictionary.Source has an invalid BAML offset");
                    source_after_offset = source.record_offset + 1;
                    source_count++;
                }
                CHECK(source_result == 0 && source_count == 13,
                      "all app.baml ResourceDictionary.Source properties were not decoded");
                CHECK(vx_dotnet_resource_get(&items, "styles/windowstyles.baml", &item) &&
                      item.type_code == 33 && item.stream_data && item.stream_size > 28,
                      "embedded WindowStyles BAML stream could not be resolved");
                vx_dotnet_baml window_styles;
                uint32_t window_style_records = 0;
                CHECK(vx_dotnet_baml_open(item.stream_data, item.stream_size, &window_styles) &&
                      vx_dotnet_baml_validate(&window_styles, &window_style_records) &&
                      window_style_records > 100,
                      "embedded WindowStyles BAML record boundaries were rejected");
                CHECK(vx_dotnet_resource_get(&items, "styles/buttonstyles.baml", &item) &&
                      item.type_code == 33 && item.stream_data && item.stream_size > 28,
                      "embedded ButtonStyles BAML stream could not be resolved");
                vx_dotnet_baml button_styles;
                uint32_t button_style_records = 0;
                CHECK(vx_dotnet_baml_open(item.stream_data, item.stream_size, &button_styles) &&
                      vx_dotnet_baml_validate(&button_styles, &button_style_records) &&
                      button_style_records > 800,
                      "embedded ButtonStyles BAML dictionary-key records were rejected");
                CHECK(vx_dotnet_resource_get(&items, "launcherwindow.baml", &item) && item.type_code == 33,
                      "launcher window BAML stream could not be resolved");
                printf("      .resources entries=%u, app.baml=%u bytes BAML v%u.%u, records=%u, merged dictionaries=%u, WindowStyles BAML records=%u, ButtonStyles BAML records=%u; launcherwindow.baml present\n",
                       items.resource_count, app_stream_size, baml.reader_major, baml.reader_minor, baml_records,
                       source_count, window_style_records, button_style_records);
            }
        }
    }
    vx_dotnet_method_body body;
    if (vx_dotnet_method_body_get(&md, &method, &body)) {
        printf("  entry IL (%u bytes):", body.code_size);
        uint32_t shown = body.code_size < 64 ? body.code_size : 64;
        for (uint32_t i = 0; i < shown; i++) printf(" %02x", body.code[i]);
        if (shown < body.code_size) printf(" ...");
        putchar('\n');
        uint32_t cursor = 0;
        vx_dotnet_instruction ins;
        int decoded;
        while ((decoded = vx_dotnet_il_next(body.code, body.code_size, &cursor, &ins)) == 1) {
            uint16_t op = ins.opcode;
            if ((op != 0x28 && op != 0x6f && op != 0x73) ||
                ins.operand_kind != VX_IL_OPERAND_TOKEN || ins.operand_size != 4) continue;
            uint32_t token; memcpy(&token, ins.operand, sizeof token);
            if ((token >> 24) == 6) {
                vx_dotnet_method target;
                if (vx_dotnet_method_get(&md, token, &target))
                    printf("  %s %s.%s token=%08x\n", op == 0x73 ? "newobj" : op == 0x6f ? "callvirt" : "call",
                           target.declaring_type_name ? target.declaring_type_name : "?", target.name, token);
            } else if ((token >> 24) == 0x0a) {
                uint32_t rid = token & 0x00ffffffu, parent, name_index;
                const char *member_name = "?", *type_name = "?", *type_ns = "";
                if (rid && rid <= md.row_count[10] && vx_dotnet_column(&md, 10, rid - 1, 0, &parent) &&
                    vx_dotnet_column(&md, 10, rid - 1, 1, &name_index)) {
                    vx_dotnet_string(&md, name_index, &member_name);
                    uint8_t tag = parent & 7; uint32_t parent_rid = parent >> 3, tn, ns;
                    if (tag == 0 && parent_rid && vx_dotnet_column(&md, 2, parent_rid - 1, 1, &tn) &&
                        vx_dotnet_column(&md, 2, parent_rid - 1, 2, &ns)) {
                        vx_dotnet_string(&md, tn, &type_name); vx_dotnet_string(&md, ns, &type_ns);
                    } else if (tag == 1 && parent_rid && vx_dotnet_column(&md, 1, parent_rid - 1, 1, &tn) &&
                               vx_dotnet_column(&md, 1, parent_rid - 1, 2, &ns)) {
                        vx_dotnet_string(&md, tn, &type_name); vx_dotnet_string(&md, ns, &type_ns);
                    }
                }
                printf("  %s %s.%s::%s token=%08x\n", op == 0x73 ? "newobj" : op == 0x6f ? "callvirt" : "call",
                       type_ns, type_name, member_name, token);
            }
        }
        if (decoded < 0) printf("  entry method contains invalid or unsupported CIL framing\n");
    }
    dump_method(&md, 0x060001ae); // App..ctor
    dump_method(&md, 0x060001ac); // App.InitializeComponent
    free(image);
}

int main(int argc, char **argv) {
    size_t length;
    uint8_t *image = make_fixture(&length);
    vx_pe_info pe;
    vx_dotnet_metadata md;
    CHECK(vx_pe_inspect(image, length, &pe), "synthetic PE rejected");
    CHECK(vx_dotnet_metadata_open(image, length, &pe, &md), "synthetic metadata rejected");
    CHECK(md.entry_point_token == 0x06000001, "entry point token was not read");
    CHECK(md.row_count[0] == 1 && md.row_count[1] == 4 && md.row_count[2] == 1 &&
          md.row_count[6] == 1 && md.row_count[10] == 8 && md.row_count[32] == 1 &&
          md.row_count[35] == 1,
          "metadata table counts were not read");
    uint32_t assembly_name;
    CHECK(vx_dotnet_column(&md, 32, 0, 7, &assembly_name), "assembly name column unavailable");
    const char *assembly;
    CHECK(vx_dotnet_string(&md, assembly_name, &assembly) && !strcmp(assembly, "Fixture"),
          "assembly name heap index is wrong");
    vx_dotnet_method method;
    CHECK(vx_dotnet_method_get(&md, md.entry_point_token, &method), "method definition could not be resolved");
    CHECK(!strcmp(method.name, "Main") && !strcmp(method.declaring_type_name, "Entry") &&
          !strcmp(method.declaring_type_namespace, "Test"), "method or declaring type was decoded incorrectly");
    const uint8_t *signature; uint32_t signature_size;
    CHECK(vx_dotnet_blob(&md, method.signature_index, &signature, &signature_size) && signature_size == 3 &&
          signature[0] == 0 && signature[1] == 0 && signature[2] == 8, "method signature blob was not decoded");
    vx_dotnet_method_body body;
    CHECK(vx_dotnet_method_body_get(&md, &method, &body), "tiny IL method body was rejected");
    CHECK(body.code_size == 6 && body.code[0] == 0x1f && body.code[1] == 20 && body.code[2] == 0x1f &&
          body.code[3] == 22 && body.code[4] == 0x58 && body.code[5] == 0x2a,
          "tiny IL method body was decoded incorrectly");
    int64_t value; char error[160];
    CHECK(vx_dotnet_execute_static(&md, md.entry_point_token, &value, error, sizeof error) && value == 42,
          error[0] ? error : "integer CIL interpreter returned the wrong result");
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 42,
          error[0] ? error : "managed entry interpreter returned the wrong result");

    uint8_t *entry_code = image + 0x701;
    entry_code[0] = 0x16; entry_code[1] = 0x2c; entry_code[2] = 0;
    entry_code[3] = 0x1f; entry_code[4] = 42; entry_code[5] = 0x2a;
    CHECK(vx_dotnet_execute_static(&md, md.entry_point_token, &value, error, sizeof error) && value == 42,
          error[0] ? error : "CIL conditional branch did not reach its target");
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 42,
          error[0] ? error : "managed entry interpreter did not follow the conditional branch");
    entry_code[0] = 0x73; entry_code[1] = 0xae; entry_code[2] = 1;
    entry_code[3] = 0; entry_code[4] = 6; entry_code[5] = 0x2a;
    CHECK(!vx_dotnet_execute_static(&md, md.entry_point_token, &value, error, sizeof error) &&
          strstr(error, "unsupported CIL opcode 0x73"), "unsupported newobj did not fail explicitly");
    entry_code[0] = 0x1f; entry_code[1] = 20; entry_code[2] = 0x1f;
    entry_code[3] = 22; entry_code[4] = 0x58; entry_code[5] = 0x2a;

    uint8_t ceq_body[] = { 0x1f, 42, 0x1f, 42, 0xfe, 0x01, 0x2a };
    entry_code[-1] = (uint8_t)(sizeof ceq_body * 4 + 2);
    memcpy(entry_code, ceq_body, sizeof ceq_body);
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 1,
          error[0] ? error : "CIL ceq did not report equal I4 values");
    ceq_body[3] = 43;
    memcpy(entry_code, ceq_body, sizeof ceq_body);
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 0,
          error[0] ? error : "CIL ceq did not report unequal I4 values");

    uint8_t float_ceq_body[] = {
        0x22, 0, 0, 0, 0,
        0x22, 0, 0, 0, 0,
        0xfe, 0x01, 0x2a
    };
    put32(float_ceq_body + 1, 0x3f800000u);
    put32(float_ceq_body + 6, 0x3f800000u);
    entry_code[-1] = (uint8_t)(sizeof float_ceq_body * 4 + 2);
    memcpy(entry_code, float_ceq_body, sizeof float_ceq_body);
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 1,
          error[0] ? error : "CIL ceq did not report equal R4 values");
    put32(float_ceq_body + 1, 0x7fc00000u);
    put32(float_ceq_body + 6, 0x7fc00000u);
    memcpy(entry_code, float_ceq_body, sizeof float_ceq_body);
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 0,
          error[0] ? error : "CIL ceq treated unordered NaN operands as equal");

    uint8_t null_ceq_body[] = { 0x14, 0x14, 0xfe, 0x01, 0x2a };
    entry_code[-1] = (uint8_t)(sizeof null_ceq_body * 4 + 2);
    memcpy(entry_code, null_ceq_body, sizeof null_ceq_body);
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 1,
          error[0] ? error : "CIL ceq did not compare null references by identity");
    entry_code[-1] = 26;
    entry_code[0] = 0x1f; entry_code[1] = 20; entry_code[2] = 0x1f;
    entry_code[3] = 22; entry_code[4] = 0x58; entry_code[5] = 0x2a;

    const char *output_path = "build/vxrt-dotnet-writealltext-test.txt";
    FILE *stale = fopen(output_path, "wb");
    CHECK(stale, "could not create the stale managed output fixture");
    CHECK(fwrite("stale contents that must be truncated", 1, 37, stale) == 37 && !fclose(stale),
          "could not seed the managed output fixture");
    uint8_t write_body[] = {
        0x72, 0, 0, 0, 0,
        0x72, 0, 0, 0, 0,
        0x28, 1, 0, 0, 0x0a,
        0x1f, 42, 0x2a
    };
    put32(write_body + 1, 0x70000000u | fixture_path_user_string);
    put32(write_body + 6, 0x70000000u | fixture_content_user_string);
    put32(write_body + 11, 0x0a000001u);
    entry_code[-1] = (uint8_t)(sizeof write_body * 4 + 2);
    memcpy(entry_code, write_body, sizeof write_body);
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 42,
          error[0] ? error : "managed File.WriteAllText call did not complete");
    FILE *written = fopen(output_path, "rb");
    CHECK(written, "managed File.WriteAllText did not create its output file");
    char output[64] = {0};
    size_t output_size = fread(output, 1, sizeof output, written);
    CHECK(!ferror(written) && !fclose(written) && output_size == strlen("new managed contents") &&
          !memcmp(output, "new managed contents", output_size),
          "managed File.WriteAllText did not write UTF-8 text while truncating the old file");
    uint8_t exists_body[] = {
        0x72, 0, 0, 0, 0,
        0x28, 4, 0, 0, 0x0a,
        0x2a
    };
    put32(exists_body + 1, 0x70000000u | fixture_path_user_string);
    put32(exists_body + 6, 0x0a000004u);
    entry_code[-1] = (uint8_t)(sizeof exists_body * 4 + 2);
    memcpy(entry_code, exists_body, sizeof exists_body);
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 1,
          error[0] ? error : "managed File.Exists did not find the written file");
    uint8_t delete_body[] = {
        0x72, 0, 0, 0, 0,
        0x28, 2, 0, 0, 0x0a,
        0x1f, 42, 0x2a
    };
    put32(delete_body + 1, 0x70000000u | fixture_path_user_string);
    put32(delete_body + 6, 0x0a000002u);
    entry_code[-1] = (uint8_t)(sizeof delete_body * 4 + 2);
    memcpy(entry_code, delete_body, sizeof delete_body);
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 42,
          error[0] ? error : "managed File.Delete call did not complete");
    errno = 0;
    written = fopen(output_path, "rb");
    CHECK(!written && errno == ENOENT, "managed File.Delete did not remove the written file");
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 42,
          error[0] ? error : "managed File.Delete failed for a missing file");
    entry_code[-1] = (uint8_t)(sizeof exists_body * 4 + 2);
    memcpy(entry_code, exists_body, sizeof exists_body);
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 0,
          error[0] ? error : "managed File.Exists reported a deleted file as present");

    const char *directory_path = "build/vxrt-dotnet-directory-test/child";
    const char *directory_parent = "build/vxrt-dotnet-directory-test";
    rmdir(directory_path);
    rmdir(directory_parent);
    uint8_t create_directory_body[] = {
        0x72, 0, 0, 0, 0,
        0x28, 3, 0, 0, 0x0a,
        0x26, 0x1f, 42, 0x2a
    };
    put32(create_directory_body + 1, 0x70000000u | fixture_directory_user_string);
    put32(create_directory_body + 6, 0x0a000003u);
    entry_code[-1] = (uint8_t)(sizeof create_directory_body * 4 + 2);
    memcpy(entry_code, create_directory_body, sizeof create_directory_body);
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 42,
          error[0] ? error : "managed Directory.CreateDirectory call did not complete");
    struct stat directory_status;
    CHECK(!stat(directory_path, &directory_status) && S_ISDIR(directory_status.st_mode),
          "managed Directory.CreateDirectory did not create its directory");
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 42,
          error[0] ? error : "managed Directory.CreateDirectory failed for an existing directory");
    CHECK(!rmdir(directory_path), "could not remove the managed directory fixture");
    CHECK(!rmdir(directory_parent), "could not remove the recursive parent directory fixture");

    const char *version_file_path = "build/vxrt-dotnet-versioninfo-test.exe";
    FILE *version_file = fopen(version_file_path, "wb");
    CHECK(version_file && fwrite(image, 1, length, version_file) == length && !fclose(version_file),
          "could not write the isolated managed FileVersionInfo fixture");
    uint8_t version_info_body[] = {
        0x72, 0, 0, 0, 0,
        0x28, 7, 0, 0, 0x0a,
        0x6f, 8, 0, 0, 0x0a,
        0x2a
    };
    put32(version_info_body + 1, 0x70000000u | fixture_version_file_user_string);
    put32(version_info_body + 6, 0x0a000007u);
    put32(version_info_body + 11, 0x0a000008u);
    entry_code[-1] = (uint8_t)(sizeof version_info_body * 4 + 2);
    memcpy(entry_code, version_info_body, sizeof version_info_body);
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 4,
          error[0] ? error : "FileVersionInfo.GetVersionInfo did not return the PE private version part");
    CHECK(!unlink(version_file_path), "could not remove the isolated FileVersionInfo PE fixture");

    const char *local_data_path = "build/vxrt-dotnet-localappdata-test";
    CHECK(!mkdir(local_data_path, 0777) || errno == EEXIST,
          "could not create the isolated LocalApplicationData fixture");
    uint8_t folder_path_body[] = {
        0x1f, 28,
        0x28, 5, 0, 0, 0x0a,
        0x28, 6, 0, 0, 0x0a,
        0x2a
    };
    put32(folder_path_body + 3, 0x0a000005u);
    put32(folder_path_body + 8, 0x0a000006u);
    entry_code[-1] = (uint8_t)(sizeof folder_path_body * 4 + 2);
    memcpy(entry_code, folder_path_body, sizeof folder_path_body);
    CHECK(vx_dotnet_execute_entry(&md, md.entry_point_token, &value, error, sizeof error) && value == 1,
          error[0] ? error : "Environment.GetFolderPath did not return the mapped local app-data folder");
    CHECK(!rmdir(local_data_path), "could not remove the isolated LocalApplicationData fixture");

    uint8_t *fat = image + 0x800;
    put16(fat, 0x3003); put16(fat + 2, 8); put32(fat + 4, 6); put32(fat + 8, 0);
    fat[12] = 0x1f; fat[13] = 20; fat[14] = 0x1f; fat[15] = 22; fat[16] = 0x58; fat[17] = 0x2a;
    method.rva = 0x1600;
    CHECK(vx_dotnet_method_body_get(&md, &method, &body) && body.code_size == 6 && body.max_stack == 8,
          "fat IL method body was decoded incorrectly");
    put16(fat, 0x300b); // Fat method with an aligned EH section.
    uint8_t *eh = image + 0x814;
    eh[0] = 1; eh[1] = 16; // small EH table, one 12-byte clause
    put16(eh + 4, 2); put16(eh + 6, 0); eh[8] = 2; // finally try range
    put16(eh + 9, 2); eh[11] = 2;
    CHECK(vx_dotnet_method_body_get(&md, &method, &body) && body.has_more_sections &&
          body.sections == eh && body.sections_size == 16,
          "valid small exception-handling section was not bounds-checked and retained");
    eh[1] = 15;
    CHECK(!vx_dotnet_method_body_get(&md, &method, &body),
          "malformed exception-handling clause size was accepted");
    eh[1] = 16;
    method.rva = 0x19ff;
    image[0xbff] = 0x3e;
    CHECK(!vx_dotnet_method_body_get(&md, &method, &body), "truncated tiny method body accepted");

    put64(image + 0x300 + 108 + 8, 1ull << 45);
    CHECK(!vx_dotnet_metadata_open(image, length, &pe, &md), "corrupt metadata table accepted");
    free(image);
    for (int i = 1; i < argc; i++) inspect_file(argv[i]);
    puts("ECMA-335 metadata tables, heaps and IL method bodies: ok");
    return 0;
}
