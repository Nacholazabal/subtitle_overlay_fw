/**********************************************************************************************************************
Copyright (c) 2024, <Your Name> <your_email@mail.com>

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file ring_buffer.h
/// @brief Defines a ring buffer data structure and associated functions in C.
///
/// A ring buffer, also known as a circular buffer, is a data structure that uses a fixed-size
/// buffer which wraps around itself. It is particularly useful in scenarios where you need
/// to efficiently manage a continuous stream of data with a fixed buffer size.
///
/// This file provides the definition of the ring buffer structure and functions to initialize,
/// read from, and write to the buffer.
///

// === Headers files inclusions ==================================================================================== //

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //

///
/// @brief Structure representing a ring buffer.
///
/// The ring buffer maintains a fixed-size circular buffer with read and write indices.
///
typedef struct ring_buffer_s
{
    uint8_t* buffer;  ///< Pointer to the underlying buffer.
    size_t capacity;  ///< Length of the buffer.
    size_t tail;      ///< Index pointing to the next element to be read.
    size_t head;      ///< Index pointing to the next element to be written.
    bool is_full;     ///< Whether is full or not
} ring_buffer_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

///
/// @brief Initializes a ring buffer with the given parameters.
///
/// This function initializes a ring buffer with the provided buffer and size. The buffer
/// must be pre-allocated by the caller. Additionally, the size parameter should be a power
/// of two as per the API assumption.
///
/// @param buffer Pointer to the pre-allocated buffer.
/// @param size Size of the buffer. Must be a power of two.
///
void ring_buffer_init(ring_buffer_t* rb, uint8_t* buffer, size_t size);

///
/// @brief Resets the ring buffer state to empty (ie tail == head). Data is not cleared.
/// @param rb Ring buffer to reset.
///
void ring_buffer_reset(ring_buffer_t* rb);

///
/// @brief Returns the number of elements stored on the ring buffer.
/// @param rb Ring buffer to check.
/// @return Size of the buffer.
///
size_t ring_buffer_size(ring_buffer_t* rb);

///
/// @brief Returns the ring buffer capacity.
/// @param rb Ring buffer to check.
/// @return Maximum size of the buffer.
///
size_t ring_buffer_capacity(ring_buffer_t* rb);

///
/// @brief Checks if the ring buffer is empty.
///
/// @param rb Pointer to the ring buffer structure to check.
/// @return true if the ring buffer is empty, false otherwise.
///
bool ring_buffer_is_empty(ring_buffer_t* rb);

///
/// @brief Checks if the ring buffer is full.
///
/// @param rb Pointer to the ring buffer structure to check.
/// @return true if the ring buffer is full, false otherwise.
///
bool ring_buffer_is_full(ring_buffer_t* rb);

///
/// @brief Writes a byte of data to the ring buffer.
///
/// This function writes a byte of data to the ring buffer. If the buffer is full, it discards
/// the oldest data to make space for the new data.
///
/// @param rb Pointer to the ring buffer structure to write to.
/// @param data The byte of data to write to the buffer.
///
void ring_buffer_write_byte(ring_buffer_t* rb, uint8_t data);

///
/// @brief Writes a 16 bits of data to the ring buffer.
///
/// This function writes 16 bits of data to the ring buffer. If the buffer is full, it discards
/// the oldest data to make space for the new data.
///
/// @param rb Pointer to the ring buffer structure to write to.
/// @param data The 16 bits of data to write to the buffer.
///
void ring_buffer_write_16_bits(ring_buffer_t* rb, uint16_t data);

///
/// @brief Writes `lenght` bytes of data to the ring buffer.
///
/// This function writes several bytes of data to the ring buffer. If the buffer is full, it discards
/// the oldest data to make space for the new data.
///
/// @param rb Pointer to the ring buffer structure to write to.
/// @param data The byte of data to write to the buffer.
///
void ring_buffer_write_bytes(ring_buffer_t* rb, uint8_t* data, size_t lenght);

///
/// @brief Reads a byte of data from the ring buffer.
///
/// This function reads a byte of data from the ring buffer. If the buffer is empty, it returns -1.
///
/// @param rb Pointer to the ring buffer structure to read from.
/// @param data Pointer to a variable to store the read data.
/// @return 0 on success, or -1 if the buffer is empty.
///
int ring_buffer_read_byte(ring_buffer_t* rb, uint8_t* data);

///
/// @brief Reads 16 bits of data from the ring buffer.
///
/// This function reads 16 bits of data from the ring buffer. If the buffer is empty, it returns -1.
///
/// @param rb Pointer to the ring buffer structure to read from.
/// @param data Pointer to a variable to store the read data.
/// @return 0 on success, or -1 if the buffer is empty.
///
int ring_buffer_read_16_bits(ring_buffer_t* rb, uint16_t* data);

///
/// @brief Reads `lenght` bytes of data from the ring buffer.
///
/// This function reads several bytes of data from the ring buffer (while the buffer is not empty).
/// If the buffer is empty, it returns -1.
///
/// @param rb Pointer to the ring buffer structure to read from.
/// @param data Pointer to a variable to store the read data.
/// @param lenght Number of bytes to read.
/// @return 0 on success, or -1 if the buffer is empty.
///
int ring_buffer_read_bytes(ring_buffer_t* rb, uint8_t* data, size_t lenght);

///
/// @brief Sets the tail of the ring buffer relative to the head by a specified length.
///
/// This function sets the tail position in the ring buffer based on the current head position,
/// moving the tail backwards by the specified length. The tail will wrap around if necessary
/// to remain within the buffer's capacity.
///
/// @param rb Pointer to the ring buffer structure.
/// @param length The number of positions to regress the tail relative to the head.
/// @return 1 on success.
///
void ring_buffer_set_tail_relative_to_head(ring_buffer_t* rb, size_t lenght);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
