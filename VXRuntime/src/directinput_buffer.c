#include "directinput_buffer.h"
#include <stdlib.h>
#include <string.h>

#define VX_DI_MAX_BUFFER_SIZE 65536u

void vx_di_buffer_init(vx_di_buffer *buffer) {
    if (buffer) memset(buffer, 0, sizeof *buffer);
}

void vx_di_buffer_destroy(vx_di_buffer *buffer) {
    if (!buffer) return;
    free(buffer->events);
    memset(buffer, 0, sizeof *buffer);
}

void vx_di_buffer_clear(vx_di_buffer *buffer) {
    if (!buffer) return;
    buffer->head = buffer->count = 0;
    buffer->overflow = 0;
}

int vx_di_buffer_set_size(vx_di_buffer *buffer, uint32_t size) {
    if (!buffer) return 0;
    if (size > VX_DI_MAX_BUFFER_SIZE) size = VX_DI_MAX_BUFFER_SIZE;
    uint32_t capacity = size ? size - 1 : 0;
    vx_di_buffer_event *events = capacity ? calloc(capacity, sizeof *events) : NULL;
    if (capacity && !events) return 0;
    uint32_t old_count = buffer->count;
    uint32_t keep = old_count < capacity ? old_count : capacity;
    for (uint32_t i = 0; i < keep; i++)
        events[i] = buffer->events[(buffer->head + i) % (buffer->capacity ? buffer->capacity : 1)];
    free(buffer->events);
    buffer->events = events;
    buffer->capacity = capacity;
    buffer->head = 0;
    buffer->count = keep;
    if (keep < old_count) buffer->overflow = 1;
    return 1;
}

uint32_t vx_di_buffer_capacity(const vx_di_buffer *buffer) { return buffer ? buffer->capacity : 0; }
uint32_t vx_di_buffer_count(const vx_di_buffer *buffer) { return buffer ? buffer->count : 0; }

void vx_di_buffer_push(vx_di_buffer *buffer, uint32_t offset, uint32_t data,
                       uint32_t timestamp, uintptr_t app_data) {
    if (!buffer || !buffer->capacity) return;
    vx_di_buffer_event event = {
        .offset = offset, .data = data, .timestamp = timestamp,
        .sequence = ++buffer->sequence, .app_data = app_data
    };
    if (buffer->count == buffer->capacity) {
        buffer->head = (buffer->head + 1) % buffer->capacity;
        buffer->count--;
        buffer->overflow = 1;
    }
    uint32_t tail = (buffer->head + buffer->count) % buffer->capacity;
    buffer->events[tail] = event;
    buffer->count++;
}

int vx_di_buffer_read(vx_di_buffer *buffer, vx_di_buffer_event *destination,
                      uint32_t *count, int peek, int *overflowed) {
    if (!buffer || !count) return 0;
    uint32_t requested = *count;
    uint32_t take = requested == UINT32_MAX || requested > buffer->count ? buffer->count : requested;
    if (destination) {
        for (uint32_t i = 0; i < take; i++)
            destination[i] = buffer->events[(buffer->head + i) % buffer->capacity];
    }
    if (!peek && take) {
        buffer->head = buffer->capacity ? (buffer->head + take) % buffer->capacity : 0;
        buffer->count -= take;
    }
    if (overflowed) *overflowed = buffer->overflow;
    if (!peek) buffer->overflow = 0;
    *count = take;
    return 1;
}
