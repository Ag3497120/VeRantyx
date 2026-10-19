#include "dotnet_resources.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "dotnet resources test failed: %s\n", m); exit(1); } } while (0)

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof v); }

int main(void) {
    uint8_t data[96] = {0};
    const uint8_t name[] = { 'c',0, 'a',0, 'f',0, 0xe9,0, '.',0, 'b',0, 'i',0, 'n',0 };
    const uint8_t value[] = { 'M','S','B','A','M','L' };
    put32(data, 0xbeefcace); put32(data + 4, 1); put32(data + 8, 0);
    put32(data + 12, 2); put32(data + 16, 1); put32(data + 20, 0);
    put32(data + 24, 0); // name hash
    put32(data + 28, 0); // name position
    put32(data + 32, 57); // data section begins after the name and its value offset
    data[36] = sizeof name;
    memcpy(data + 37, name, sizeof name);
    put32(data + 53, 0); // value offset in the data section
    data[57] = 33;       // ResourceTypeCode.Stream
    put32(data + 58, sizeof value);
    memcpy(data + 62, value, sizeof value);

    vx_dotnet_resources resources;
    vx_dotnet_resource item;
    CHECK(vx_dotnet_resources_open(data, 62 + sizeof value, &resources), "valid stream rejected");
    CHECK(resources.resource_count == 1 && resources.type_count == 0, "resource counts were decoded incorrectly");
    CHECK(vx_dotnet_resource_get(&resources, "caf\xc3\xa9.bin", &item), "UTF-8 name did not match UTF-16 resource name");
    CHECK(item.type_code == 33 && item.stream_size == sizeof value &&
          !memcmp(item.stream_data, value, sizeof value), "stream resource payload was decoded incorrectly");
    CHECK(!vx_dotnet_resource_get(&resources, "missing.bin", &item), "missing resource unexpectedly resolved");

    data[32] = 0xff; data[33] = 0xff; data[34] = 0xff; data[35] = 0xff;
    CHECK(!vx_dotnet_resources_open(data, 62 + sizeof value, &resources), "out-of-range data section accepted");
    puts(".resources directory, UTF-8 lookup and stream bounds: ok");
    return 0;
}
