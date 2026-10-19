#include "pe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "pe test failed: %s\n", m); exit(1); } } while (0)

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, sizeof v); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof v); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, sizeof v); }

static void make_image(uint8_t *b, uint16_t machine, uint16_t magic, int managed) {
    memset(b, 0, 0x600);
    b[0] = 'M'; b[1] = 'Z';
    put32(b + 0x3c, 0x80);
    memcpy(b + 0x80, "PE\0\0", 4);
    uint8_t *coff = b + 0x84, *opt = b + 0x98;
    put16(coff, machine); put16(coff + 2, 1); put16(coff + 16, magic == 0x10b ? 0xe0 : 0xf0);
    put16(coff + 18, 0x0102);
    put16(opt, magic); put32(opt + 16, 0x1000); put32(opt + 56, 0x2000); put32(opt + 60, 0x200);
    if (magic == 0x10b) { put32(opt + 28, 0x400000); put32(opt + 92, 16); }
    else { put64(opt + 24, 0x140000000); put32(opt + 108, 16); }
    if (managed) {
        size_t directory = magic == 0x10b ? 96 : 112;
        put32(opt + directory + 14 * 8, 0x1000);
        put32(opt + directory + 14 * 8 + 4, 72);
    }
    size_t section = 0x98 + (magic == 0x10b ? 0xe0 : 0xf0);
    put32(b + section + 8, 0x400); put32(b + section + 12, 0x1000);
    put32(b + section + 16, 0x400); put32(b + section + 20, 0x200);
    if (managed) {
        uint8_t *cor = b + 0x200;
        put32(cor, 72); put16(cor + 4, 2); put16(cor + 6, 5);
        put32(cor + 8, 0x1100); put32(cor + 12, 0x80);
        put32(cor + 16, 1); put32(cor + 20, 0x06000001);
        uint8_t *md = b + 0x300;
        put32(md, 0x424a5342); put16(md + 4, 1); put16(md + 6, 1);
        put32(md + 12, 12); memcpy(md + 16, "v4.0.30319", 11);
    }
}

static void make_version_resource(uint8_t *image) {
    uint8_t *opt = image + 0x98;
    put32(opt + 96 + 2 * 8, 0x1000);
    put32(opt + 96 + 2 * 8 + 4, 0x200);
    uint8_t *resource = image + 0x200;
    put16(resource + 14, 1);
    put32(resource + 16, 16); put32(resource + 20, 0x80000018u);
    put16(resource + 0x18 + 14, 1);
    put32(resource + 0x18 + 16, 1); put32(resource + 0x18 + 20, 0x80000030u);
    put16(resource + 0x30 + 14, 1);
    put32(resource + 0x30 + 16, 0x409); put32(resource + 0x30 + 20, 0x48);
    put32(resource + 0x48, 0x1100); put32(resource + 0x48 + 4, 200);

    uint8_t *version = image + 0x300;
    put16(version, 200); put16(version + 2, 52); put16(version + 4, 0);
    static const char key[] = "VS_VERSION_INFO";
    for (size_t i = 0; i < sizeof key; i++) put16(version + 6 + i * 2, (uint8_t)key[i]);
    uint8_t *fixed = version + 40;
    put32(fixed, 0xfeef04bd); put32(fixed + 4, 0x00010000);
    put32(fixed + 8, (1u << 16) | 2u); put32(fixed + 12, (3u << 16) | 4u);
    put32(fixed + 16, (5u << 16) | 6u); put32(fixed + 20, (7u << 16) | 8u);
    put32(fixed + 24, 0x3f); put32(fixed + 28, 1);
    uint8_t *strings = version + 92;
    put16(strings, 108); put16(strings + 2, 0); put16(strings + 4, 1);
    static const char string_file_info[] = "StringFileInfo";
    for (size_t i = 0; i < sizeof string_file_info; i++)
        put16(strings + 6 + i * 2, (uint8_t)string_file_info[i]);
    uint8_t *table = strings + 36;
    put16(table, 72); put16(table + 2, 0); put16(table + 4, 1);
    static const char table_key[] = "040904b0";
    for (size_t i = 0; i < sizeof table_key; i++) put16(table + 6 + i * 2, (uint8_t)table_key[i]);
    uint8_t *file_version = table + 24;
    put16(file_version, 48); put16(file_version + 2, 8); put16(file_version + 4, 1);
    static const char file_version_key[] = "FileVersion";
    for (size_t i = 0; i < sizeof file_version_key; i++)
        put16(file_version + 6 + i * 2, (uint8_t)file_version_key[i]);
    static const char file_version_text[] = "1.2.3.4";
    for (size_t i = 0; i < sizeof file_version_text; i++)
        put16(file_version + 32 + i * 2, (uint8_t)file_version_text[i]);
}

static void inspect_file(const char *path) {
    FILE *f = fopen(path, "rb");
    CHECK(f, "could not open PE sample");
    CHECK(!fseek(f, 0, SEEK_END), "could not seek PE sample");
    long length = ftell(f);
    CHECK(length > 0 && !fseek(f, 0, SEEK_SET), "could not size PE sample");
    uint8_t *data = malloc((size_t)length);
    CHECK(data && fread(data, 1, (size_t)length, f) == (size_t)length, "could not read PE sample");
    fclose(f);
    vx_pe_info pe;
    CHECK(vx_pe_inspect(data, (size_t)length, &pe), "real PE sample rejected");
    printf("%s: machine=%04x format=%s clr=%s metadata=%s flags=%08x\n", path, pe.machine,
           pe.optional_magic == 0x10b ? "PE32" : "PE32+", pe.has_cli ? "yes" : "no",
           pe.metadata_version[0] ? pe.metadata_version : "-", pe.clr_flags);
    free(data);
}

int main(int argc, char **argv) {
    uint8_t image[0x600];
    vx_pe_info pe;

    if (argc == 3 && !strcmp(argv[1], "--write-managed")) {
        make_image(image, 0x014c, 0x010b, 1);
        FILE *f = fopen(argv[2], "wb");
        CHECK(f && fwrite(image, 1, sizeof image, f) == sizeof image && !fclose(f),
              "could not write managed PE fixture");
        return 0;
    }

    make_image(image, 0x014c, 0x010b, 1);
    CHECK(vx_pe_inspect(image, sizeof image, &pe), "valid PE32 CLI image rejected");
    CHECK(pe.machine == 0x014c && pe.optional_magic == 0x010b, "PE32 architecture was not decoded");
    CHECK(pe.has_cli && pe.clr_major == 2 && pe.clr_minor == 5, "CLR header was not decoded");
    CHECK(pe.clr_flags == 1 && pe.clr_entry_point == 0x06000001, "CLR entry metadata was not decoded");
    CHECK(!strcmp(pe.metadata_version, "v4.0.30319"), "metadata version string was not decoded");

    make_image(image, 0x8664, 0x020b, 1);
    CHECK(vx_pe_inspect(image, sizeof image, &pe) && pe.has_cli && pe.optional_magic == 0x020b,
          "valid PE32+ CLI image rejected");

    make_image(image, 0x014c, 0x010b, 0);
    CHECK(vx_pe_inspect(image, sizeof image, &pe) && !pe.has_cli, "native PE32 image misclassified as managed");

    make_image(image, 0x014c, 0x010b, 0);
    make_version_resource(image);
    CHECK(vx_pe_inspect(image, sizeof image, &pe), "PE image with a VERSIONINFO resource was rejected");
    vx_pe_version_info version;
    CHECK(vx_pe_version_info_get(image, sizeof image, &pe, &version) &&
          version.file_version_ms == ((1u << 16) | 2u) &&
          version.file_version_ls == ((3u << 16) | 4u) &&
          version.product_version_ms == ((5u << 16) | 6u) &&
          version.product_version_ls == ((7u << 16) | 8u) &&
          version.file_flags_mask == 0x3f && version.file_flags == 1 &&
          !strcmp(version.file_version, "1.2.3.4"),
          "VERSIONINFO fixed version fields were not decoded");
    image[0x300 + 40] = 0;
    CHECK(!vx_pe_version_info_get(image, sizeof image, &pe, &version),
          "malformed VS_FIXEDFILEINFO signature was accepted");

    make_image(image, 0x014c, 0x010b, 1);
    image[0x300] = 0;
    CHECK(!vx_pe_inspect(image, sizeof image, &pe), "invalid CLR metadata signature accepted");
    make_image(image, 0x014c, 0x010b, 1);
    put32(image + 0x3c, 0xfffffff0);
    CHECK(!vx_pe_inspect(image, sizeof image, &pe), "out-of-file PE header offset accepted");

    for (int i = 1; i < argc; i++) inspect_file(argv[i]);

    puts("PE32 / PE32+ / CLR directory inspection: ok");
    return 0;
}
