/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file stt_ws_frame.h
/// @brief RFC 6455 client framing and opening handshake, free of any I/O
///
/// This module is deliberately pure: it never opens a socket, never reads the
/// clock and never draws entropy. Callers supply the masking key and the
/// handshake nonce, which keeps every byte of the wire format reproducible in a
/// host unit test. All transport concerns live in `net_tls` and `stt_ws_client`.
///
/// Client obligations implemented here (RFC 6455 §5.1-§5.3):
///  * every client-to-server frame is masked with the caller's 32-bit key;
///  * a server-to-client frame that arrives masked is a protocol error;
///  * payload lengths use the shortest of the 7-bit, 16-bit and 64-bit forms.
///

// === Headers files inclusions ==================================================================================== //

#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

/// Largest header a client frame can need: 2 bytes + 8 length bytes + 4 mask bytes.
#define STT_WS_FRAME_HEADER_MAX (14U)
/// Random bytes the caller must provide for `Sec-WebSocket-Key`.
#define STT_WS_KEY_NONCE_BYTES (16U)
/// Base64 of 16 bytes, plus the NUL terminator.
#define STT_WS_KEY_SIZE (25U)
/// Base64 of the 20-byte SHA-1 accept digest, plus the NUL terminator.
#define STT_WS_ACCEPT_SIZE (29U)

// === Public data type declarations =============================================================================== //

/// @brief WebSocket opcodes used by this client.
typedef enum
{
    STT_WS_OPCODE_CONTINUATION = 0x0,
    STT_WS_OPCODE_TEXT = 0x1,
    STT_WS_OPCODE_BINARY = 0x2,
    STT_WS_OPCODE_CLOSE = 0x8,
    STT_WS_OPCODE_PING = 0x9,
    STT_WS_OPCODE_PONG = 0xA,
} stt_ws_opcode_e;

/// @brief One decoded server frame; @p payload points into the caller's buffer.
typedef struct
{
    stt_ws_opcode_e opcode;
    uint8_t fin;
    uint64_t payload_len;
    size_t header_len;       ///< Bytes the header occupied.
    uint8_t const* payload;  ///< Borrowed pointer, valid while the input buffer lives.
} stt_ws_frame_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/**
 * @brief Encode one masked client frame.
 * @param out Destination buffer.
 * @param out_size Destination capacity.
 * @param opcode Frame opcode; this client never fragments, so FIN is always set.
 * @param payload Payload bytes, may be NULL when @p payload_len is 0.
 * @param payload_len Payload length in bytes.
 * @param mask_key Masking key; the caller must draw it from a random source.
 * @return Bytes written on success, or a negative errno-style value.
 */
int stt_ws_frame_encode(uint8_t* out,
                        size_t out_size,
                        stt_ws_opcode_e opcode,
                        void const* payload,
                        size_t payload_len,
                        uint32_t mask_key);

/**
 * @brief Decode one server frame from a receive buffer.
 * @param data Received bytes.
 * @param size Number of bytes available in @p data.
 * @param max_payload Largest payload accepted; larger frames are a protocol error.
 * @param frame Decoded frame destination, untouched unless 0 is returned.
 * @return 0 on success, -EAGAIN when more bytes are needed, -EMSGSIZE when the
 *         frame exceeds @p max_payload, or -EPROTO on a malformed frame.
 */
int stt_ws_frame_decode(uint8_t const* data,
                        size_t size,
                        size_t max_payload,
                        stt_ws_frame_t* frame);

/**
 * @brief Build the opening HTTP Upgrade request and the accept value to expect.
 * @param out Destination for the request text; NUL-terminated on success.
 * @param out_size Destination capacity.
 * @param host Value for the `Host` header, including `:port` when non-default.
 * @param path Request target, for example `/stt/stream`.
 * @param nonce ::STT_WS_KEY_NONCE_BYTES random bytes for `Sec-WebSocket-Key`.
 * @param expected_accept Destination for the `Sec-WebSocket-Accept` value the
 *        server must return; size must be at least ::STT_WS_ACCEPT_SIZE.
 * @param expected_accept_size Capacity of @p expected_accept.
 * @return Request length in bytes on success, or a negative errno-style value.
 */
int stt_ws_handshake_build_request(char* out,
                                   size_t out_size,
                                   char const* host,
                                   char const* path,
                                   uint8_t const nonce[STT_WS_KEY_NONCE_BYTES],
                                   char* expected_accept,
                                   size_t expected_accept_size);

/**
 * @brief Derive the `Sec-WebSocket-Accept` value a server must return for a key.
 *
 * Exposed so a test can act as the server side of the handshake without
 * duplicating the SHA-1 and Base64 steps.
 *
 * @param key `Sec-WebSocket-Key` value, NUL-terminated.
 * @param out Destination; size must be at least ::STT_WS_ACCEPT_SIZE.
 * @param out_size Capacity of @p out.
 * @return Accept length on success, or a negative errno-style value.
 */
int stt_ws_handshake_accept_for_key(char const* key, char* out, size_t out_size);

/**
 * @brief Validate a server handshake response.
 *
 * Requires status 101 and a `Sec-WebSocket-Accept` matching @p expected_accept.
 * Header names are matched case-insensitively, as HTTP requires.
 *
 * @param response Received bytes; need not be NUL-terminated.
 * @param length Number of bytes available in @p response.
 * @param expected_accept Value produced by ::stt_ws_handshake_build_request.
 * @param header_len Receives the byte count consumed by the response headers,
 *        so the caller can keep any frame bytes that followed them.
 * @return 0 on success, -EAGAIN while the header block is incomplete, or
 *         -EPROTO when the response is not a valid WebSocket upgrade.
 */
int stt_ws_handshake_validate_response(char const* response,
                                       size_t length,
                                       char const* expected_accept,
                                       size_t* header_len);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
