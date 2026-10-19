#include "directinput_buffer.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "directinput buffer: check failed at line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    vx_di_buffer buffer;
    vx_di_buffer_init(&buffer);
    CHECK(vx_di_buffer_set_size(&buffer, 4));
    CHECK(vx_di_buffer_capacity(&buffer) == 3);
    vx_di_buffer_push(&buffer, 0, 11, 100, 0);
    vx_di_buffer_push(&buffer, 4, 22, 101, 0);
    vx_di_buffer_event events[4];
    uint32_t count = UINT32_MAX;
    int overflow = 0;
    CHECK(vx_di_buffer_read(&buffer, events, &count, 1, &overflow));
    CHECK(count == 2 && !overflow && events[0].offset == 0 && events[1].data == 22);
    CHECK(vx_di_buffer_count(&buffer) == 2); // DIGDD_PEEK keeps entries queued.

    count = UINT32_MAX;
    CHECK(vx_di_buffer_read(&buffer, events, &count, 0, &overflow));
    CHECK(count == 2 && !overflow && vx_di_buffer_count(&buffer) == 0);

    vx_di_buffer_push(&buffer, 0, 1, 200, 7);
    vx_di_buffer_push(&buffer, 0, 2, 201, 8);
    vx_di_buffer_push(&buffer, 0, 3, 202, 9);
    vx_di_buffer_push(&buffer, 0, 4, 203, 10);
    CHECK(vx_di_buffer_count(&buffer) == 3);
    count = 0;
    CHECK(vx_di_buffer_read(&buffer, NULL, &count, 1, &overflow));
    CHECK(count == 0 && overflow); // A zero-count query observes overflow without clearing it.
    count = UINT32_MAX;
    CHECK(vx_di_buffer_read(&buffer, events, &count, 1, &overflow));
    CHECK(count == 3 && overflow && events[0].data == 2 && events[2].data == 4);
    CHECK(events[0].sequence + 1 == events[1].sequence && events[2].app_data == 10);
    count = 2;
    CHECK(vx_di_buffer_read(&buffer, events, &count, 0, &overflow));
    CHECK(count == 2 && overflow && vx_di_buffer_count(&buffer) == 1);
    count = UINT32_MAX;
    CHECK(vx_di_buffer_read(&buffer, events, &count, 0, &overflow));
    CHECK(count == 1 && !overflow && events[0].data == 4);

    CHECK(vx_di_buffer_set_size(&buffer, 0));
    CHECK(vx_di_buffer_capacity(&buffer) == 0 && vx_di_buffer_count(&buffer) == 0);
    vx_di_buffer_push(&buffer, 0, 99, 300, 0);
    count = UINT32_MAX;
    CHECK(vx_di_buffer_read(&buffer, NULL, &count, 0, &overflow));
    CHECK(count == 0);
    vx_di_buffer_destroy(&buffer);
    puts("directinput buffer: ring, peek, flush, overflow and disabled mode ok");
    return 0;
}
