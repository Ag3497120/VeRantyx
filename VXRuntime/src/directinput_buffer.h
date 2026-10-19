// Buffered DirectInput object-change queue, independent of the host HID implementation.
#ifndef VX_DIRECTINPUT_BUFFER_H
#define VX_DIRECTINPUT_BUFFER_H
#include <stdint.h>

typedef struct {
    uint32_t offset, data, timestamp, sequence;
    uintptr_t app_data;
} vx_di_buffer_event;

typedef struct {
    vx_di_buffer_event *events;
    uint32_t capacity, head, count, sequence;
    int overflow;
} vx_di_buffer;

void vx_di_buffer_init(vx_di_buffer *buffer);
void vx_di_buffer_destroy(vx_di_buffer *buffer);
void vx_di_buffer_clear(vx_di_buffer *buffer);
// DirectInput reserves one slot in the configured buffer size; size zero disables buffering.
int vx_di_buffer_set_size(vx_di_buffer *buffer, uint32_t size);
uint32_t vx_di_buffer_capacity(const vx_di_buffer *buffer);
uint32_t vx_di_buffer_count(const vx_di_buffer *buffer);
void vx_di_buffer_push(vx_di_buffer *buffer, uint32_t offset, uint32_t data,
                       uint32_t timestamp, uintptr_t app_data);
// Reads up to *count entries; UINT32_MAX requests all entries. A NULL destination still
// applies consume/peek semantics. Overflow is reported once on a consuming read/query.
int vx_di_buffer_read(vx_di_buffer *buffer, vx_di_buffer_event *destination,
                      uint32_t *count, int peek, int *overflowed);

#endif
