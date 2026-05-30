/**********************************************************************************************************************
Copyright (c) 2024, <Your Name> <your_email@mail.com>

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file ring_buffer.c
/// @brief Defines a ring buffer data structure and associated functions in C (implementation).
///

// === Headers files inclusions ==================================================================================== //

#include <assert.h>
#include <stdlib.h>

#include "ring_buffer.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static void advance_head_pointer(ring_buffer_t* rb);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

static inline size_t advance_headtail_value(size_t value, size_t capacity) { return (value + 1) % capacity; }
static inline size_t regress_headtail_value(size_t value, size_t capacity, size_t steps)
{
    return (value + capacity - steps) % capacity;
}

static void advance_head_pointer(ring_buffer_t* rb)
{
    assert(rb);

    if (ring_buffer_is_full(rb)) { rb->tail = advance_headtail_value(rb->tail, rb->capacity); }

    rb->head = advance_headtail_value(rb->head, rb->capacity);
    rb->is_full = (rb->head == rb->tail);
}

// === Public function implementation ============================================================================== //

void ring_buffer_init(ring_buffer_t* rb, uint8_t* buffer, size_t size)
{
    assert(rb && buffer && size);

    rb->buffer = buffer;
    rb->capacity = size;
    ring_buffer_reset(rb);

    assert(ring_buffer_is_empty(rb));
}

void ring_buffer_reset(ring_buffer_t* rb)
{
    assert(rb);

    rb->head = 0;
    rb->tail = 0;
    rb->is_full = false;
}

size_t ring_buffer_size(ring_buffer_t* rb)
{
    assert(rb);

    size_t size = rb->capacity;

    if (!ring_buffer_is_full(rb)) {
        if (rb->head >= rb->tail) {
            size = (rb->head - rb->tail);
        } else {
            size = (rb->capacity + rb->head - rb->tail);
        }
    }

    return size;
}

size_t ring_buffer_capacity(ring_buffer_t* rb)
{
    assert(rb);
    return rb->capacity;
}

bool ring_buffer_is_empty(ring_buffer_t* rb)
{
    assert(rb);

    return (!ring_buffer_is_full(rb) && (rb->head == rb->tail));
}

bool ring_buffer_is_full(ring_buffer_t* rb)
{
    assert(rb);

    return rb->is_full;
}

void ring_buffer_write_byte(ring_buffer_t* rb, uint8_t data)
{
    assert(rb && rb->buffer);

    rb->buffer[rb->head] = data;

    advance_head_pointer(rb);
}

void ring_buffer_write_16_bits(ring_buffer_t* rb, uint16_t data)
{
    ring_buffer_write_byte(rb, (uint8_t)(data & 0xFF));
    ring_buffer_write_byte(rb, (uint8_t)((data >> 8) & 0xFF));
}

void ring_buffer_write_bytes(ring_buffer_t* rb, uint8_t* data, size_t lenght)
{
    assert(rb && rb->buffer);

    for (size_t i = 0; i < lenght; i++) {
        rb->buffer[rb->head] = data[i];
        advance_head_pointer(rb);
    }
}

int ring_buffer_read_byte(ring_buffer_t* rb, uint8_t* data)
{
    assert(rb && data && rb->buffer);

    int r = -1;

    if (!ring_buffer_is_empty(rb)) {
        *data = rb->buffer[rb->tail];
        rb->tail = advance_headtail_value(rb->tail, rb->capacity);
        rb->is_full = false;
        r = 0;
    }

    return r;
}

int ring_buffer_read_16_bits(ring_buffer_t* rb, uint16_t* data)
{
    assert(rb && data && rb->buffer);

    uint8_t bytes[sizeof(uint16_t)];

    // Check if the buffer has enough bytes to read (at least 2 bytes)
    if (ring_buffer_size(rb) < sizeof(uint16_t)) {
        return -1;  // Not enough data
    }

    // Perform the read in a loop
    for (size_t i = 0; i < sizeof(uint16_t); ++i) {
        if (ring_buffer_read_byte(rb, &bytes[i]) != 0) {
            return -1;  // Read failed
        }
    }

    // Combine high and low bytes into a uint16_t
    *data = (bytes[1] << 8) | bytes[0];

    return 0;  // Success
}

int ring_buffer_read_bytes(ring_buffer_t* rb, uint8_t* data, size_t lenght)
{
    assert(rb && data && rb->buffer);

    int r = -1;

    if (!ring_buffer_is_empty(rb)) {
        size_t bytes_read = 0;
        while (bytes_read < lenght && !ring_buffer_is_empty(rb)) {
            data[bytes_read] = rb->buffer[rb->tail];
            rb->tail = advance_headtail_value(rb->tail, rb->capacity);
            rb->is_full = false;
            bytes_read++;
        }
        r = 0;
    }

    return r;
}

void ring_buffer_set_tail_relative_to_head(ring_buffer_t* rb, size_t lenght)
{
    rb->tail = rb->head;
    rb->tail = regress_headtail_value(rb->tail, rb->capacity, lenght);
}

// === End of documentation ======================================================================================== //
