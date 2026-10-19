#include "dotnet_baml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "dotnet BAML test failed: %s\n", m); exit(1); } } while (0)
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, sizeof v); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof v); }

static size_t write_var_string(uint8_t *p, const char *s) {
    size_t n = strlen(s), used = 0;
    CHECK(n < 128, "test BAML string too long");
    p[used++] = (uint8_t)n;
    memcpy(p + used, s, n);
    return used + n;
}

static size_t write_variable_record(uint8_t *p, uint8_t type,
                                    const uint8_t *payload, size_t payload_size) {
    CHECK(payload_size < 126, "test BAML record too long");
    p[0] = type;
    p[1] = (uint8_t)(payload_size + 1);
    memcpy(p + 2, payload, payload_size);
    return payload_size + 2;
}

int main(void) {
    uint8_t bytes[36] = { 0 };
    static const uint8_t signature[] = { 'M',0, 'S',0, 'B',0, 'A',0, 'M',0, 'L',0 };
    put32(bytes, sizeof signature);
    memcpy(bytes + 4, signature, sizeof signature);
    put16(bytes + 18, 96); put16(bytes + 22, 96); put16(bytes + 24, 1);
    bytes[28] = 1; // DocumentStart
    bytes[29] = 1; // LoadAsync
    put32(bytes + 30, 32); // MaxAsyncRecords
    bytes[34] = 1; // DebugBaml
    bytes[35] = 2; // DocumentEnd
    vx_dotnet_baml baml;
    CHECK(vx_dotnet_baml_open(bytes, sizeof bytes, &baml), "valid BAML header rejected");
    CHECK(baml.writer_major == 0 && baml.writer_minor == 96 &&
          baml.reader_major == 0 && baml.reader_minor == 96 && baml.updater_major == 1 &&
          baml.updater_minor == 0 && baml.records_size == 8,
          "BAML version pairs were decoded incorrectly");
    vx_dotnet_baml_record record;
    CHECK(vx_dotnet_baml_next(&baml, &record) == 1 && record.type == 1 &&
          record.payload_size == 6 && record.offset == 0 && !memcmp(record.payload, bytes + 29, 6),
          "DocumentStart record framing is incorrect");
    CHECK(vx_dotnet_baml_next(&baml, &record) == 1 && record.type == 2 &&
          record.payload_size == 0 && record.offset == 7,
          "DocumentEnd record framing is incorrect");
    CHECK(vx_dotnet_baml_next(&baml, &record) == 0, "end of BAML stream was not reported");
    uint32_t record_count = 0;
    CHECK(vx_dotnet_baml_validate(&baml, &record_count) && record_count == 2,
          "complete BAML document framing was rejected");

    uint8_t framing_bytes[96] = { 0 };
    memcpy(framing_bytes, bytes, 28);
    size_t framing_cursor = 28;
    framing_bytes[framing_cursor++] = 1;
    framing_cursor += 6;
    framing_bytes[framing_cursor++] = 8; // PropertyComplexEnd has no payload.
    framing_bytes[framing_cursor++] = 7; // PropertyComplexStart has an attribute id.
    framing_cursor += 2;
    framing_bytes[framing_cursor++] = 4;
    framing_bytes[framing_cursor++] = 3; // ElementStart has type id and flags.
    framing_cursor += 3;
    framing_bytes[framing_cursor++] = 35; // PropertyWithExtension has three Int16 fields.
    framing_cursor += 6;
    framing_bytes[framing_cursor++] = 39; // DefAttributeKeyType extends ElementStart with dictionary-key metadata.
    framing_cursor += 9;
    framing_bytes[framing_cursor++] = 40; // KeyElementStart uses the same 9-byte payload.
    framing_cursor += 9;
    framing_bytes[framing_cursor++] = 41;
    framing_bytes[framing_cursor++] = 2;
    CHECK(vx_dotnet_baml_open(framing_bytes, framing_cursor, &baml) &&
          vx_dotnet_baml_validate(&baml, &record_count) && record_count == 10,
          "fixed-size WPF BAML records were not framed according to the record definitions");

    bytes[35] = 1;
    CHECK(vx_dotnet_baml_open(bytes, sizeof bytes, &baml), "valid header rejected before stream validation");
    CHECK(!vx_dotnet_baml_validate(&baml, NULL), "missing DocumentEnd was accepted");
    bytes[35] = 57;
    CHECK(vx_dotnet_baml_open(bytes, sizeof bytes, &baml), "valid header rejected before record validation");
    CHECK(!vx_dotnet_baml_validate(&baml, NULL), "unknown BAML record was accepted");
    bytes[35] = 2;
    bytes[20] = 1;
    CHECK(!vx_dotnet_baml_open(bytes, sizeof bytes, &baml), "unsupported reader version accepted");

    uint8_t source_bytes[128] = { 0 };
    put32(source_bytes, sizeof signature);
    memcpy(source_bytes + 4, signature, sizeof signature);
    put16(source_bytes + 18, 96); put16(source_bytes + 22, 96);
    size_t cursor = 28;
    source_bytes[cursor++] = 1;
    cursor += 6; // DocumentStart payload
    uint8_t attribute[32];
    put16(attribute, 3); put16(attribute + 2, (uint16_t)-524); attribute[4] = 0;
    size_t attribute_size = 5 + write_var_string(attribute + 5, "Source");
    cursor += write_variable_record(source_bytes + cursor, 31, attribute, attribute_size);
    source_bytes[cursor++] = 3;
    put16(source_bytes + cursor, (uint16_t)-524); source_bytes[cursor + 2] = 0; cursor += 3;
    uint8_t property[64];
    put16(property, 3);
    size_t property_size = 2 + write_var_string(property + 2, "Styles/demo.xaml");
    put16(property + property_size, (uint16_t)-705); property_size += 2;
    cursor += write_variable_record(source_bytes + cursor, 36, property, property_size);
    source_bytes[cursor++] = 4;
    source_bytes[cursor++] = 2;
    vx_dotnet_baml source_baml;
    CHECK(vx_dotnet_baml_open(source_bytes, cursor, &source_baml) &&
          vx_dotnet_baml_validate(&source_baml, NULL), "synthetic ResourceDictionary BAML rejected");
    vx_dotnet_baml_string_property source;
    CHECK(vx_dotnet_baml_find_string_property(&source_baml, "Source", &source) == 1 &&
          source.owner_type_id == -524 && source.value_size == strlen("Styles/demo.xaml") &&
          !memcmp(source.value, "Styles/demo.xaml", source.value_size),
          "inline ResourceDictionary.Source property was not resolved");
    CHECK(vx_dotnet_baml_find_string_property(&source_baml, "Missing", &source) == 0,
          "missing BAML property was reported as present");
    CHECK(vx_dotnet_baml_find_string_property_for_owner(&source_baml, "Source", -524, 0, &source) == 1 &&
          vx_dotnet_baml_find_string_property_for_owner(&source_baml, "Source", -523, 0, &source) == 0 &&
          vx_dotnet_baml_find_string_property_for_owner(&source_baml, "Source", -524,
                                                        source.record_offset + 1, &source) == 0,
          "owner-filtered or sequential BAML string-property lookup failed");

    uint8_t string_bytes[128] = { 0 };
    memcpy(string_bytes, source_bytes, 28);
    size_t string_cursor = 28;
    string_bytes[string_cursor++] = 1;
    memcpy(string_bytes + string_cursor, source_bytes + 29, 6);
    string_cursor += 6;
    uint8_t string_info[32] = { 0 };
    put16(string_info, 9);
    size_t string_info_size = 2 + write_var_string(string_info + 2, "login.caption");
    string_cursor += write_variable_record(string_bytes + string_cursor, 32,
                                           string_info, string_info_size);
    string_bytes[string_cursor++] = 2;
    vx_dotnet_baml string_baml;
    vx_dotnet_baml_string_info string_value;
    CHECK(vx_dotnet_baml_open(string_bytes, string_cursor, &string_baml) &&
          vx_dotnet_baml_validate(&string_baml, NULL) &&
          vx_dotnet_baml_string_info_get(&string_baml, 9, &string_value) == 1 &&
          string_value.value_size == strlen("login.caption") &&
          !memcmp(string_value.value, "login.caption", string_value.value_size) &&
          vx_dotnet_baml_string_info_get(&string_baml, 10, &string_value) == 0,
          "BAML StringInfo entries were not resolved with bounded string decoding");

    uint8_t root_bytes[128] = { 0 };
    memcpy(root_bytes, source_bytes, 28);
    size_t root_cursor = 28;
    root_bytes[root_cursor++] = 1;
    root_cursor += 6;
    uint8_t type_info[64] = { 0 };
    put16(type_info, 7);
    put16(type_info + 2, 0);
    size_t type_info_size = 4 + write_var_string(type_info + 4, "Demo.LauncherWindow");
    root_cursor += write_variable_record(root_bytes + root_cursor, 29, type_info, type_info_size);
    root_bytes[root_cursor++] = 3;
    put16(root_bytes + root_cursor, 7);
    root_bytes[root_cursor + 2] = 0;
    root_cursor += 3;
    root_bytes[root_cursor++] = 4;
    root_bytes[root_cursor++] = 2;
    vx_dotnet_baml root_baml;
    char root_name[64];
    CHECK(vx_dotnet_baml_open(root_bytes, root_cursor, &root_baml) &&
          vx_dotnet_baml_validate(&root_baml, NULL) &&
          vx_dotnet_baml_root_type_name(&root_baml, root_name, sizeof root_name) &&
          !strcmp(root_name, "Demo.LauncherWindow"),
          "root custom type was not resolved from its BAML TypeInfo record");
    puts("WPF BAML version header, record framing, and document boundaries: ok");
    return 0;
}
