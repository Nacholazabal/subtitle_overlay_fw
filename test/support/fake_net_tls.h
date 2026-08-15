#pragma once

///
/// @file fake_net_tls.h
/// @brief Scriptable in-memory stand-in for the TLS transport
///
/// `net_tls_t` is an opaque handle, so CMock cannot generate a mock for it.
/// A hand-written double is a better fit anyway: it lets a host test drive the
/// real handshake, `session_start` and frame decoding paths of the WebSocket
/// client without a socket, a server or OpenSSL.
///

#include <stddef.h>
#include <stdint.h>

#define FAKE_NET_TLS_BUFFER (16384U)

/// @brief Reset every scripted response and captured byte.
void fake_net_tls_reset(void);

/// @brief Make the next ::net_tls_open calls fail, simulating an unreachable peer.
void fake_net_tls_fail_open(uint8_t fail);

/// @brief Queue bytes the client will read, as if the server had sent them.
void fake_net_tls_push_rx(void const* data, size_t size);

/// @brief Queue a complete unmasked server frame with the given opcode.
void fake_net_tls_push_frame(uint8_t opcode, void const* payload, size_t size);

/// @brief Queue one server frame with an explicit FIN bit, to test reassembly.
void fake_net_tls_push_fragment(uint8_t opcode, uint8_t fin, void const* payload, size_t size);

/// @brief Queue one server text message.
void fake_net_tls_push_text(char const* text);

/// @brief Bytes the client has written so far.
uint8_t const* fake_net_tls_tx(size_t* size);

/// @brief Number of times ::net_tls_open was called.
uint32_t fake_net_tls_open_count(void);

/// @brief Number of times ::net_tls_close was called.
uint32_t fake_net_tls_close_count(void);

/// @brief Configuration handed to the most recent ::net_tls_open call.
void fake_net_tls_last_config(char* host, size_t host_size, uint16_t* port, uint8_t* use_tls);

/// @brief Discard captured transmit bytes, keeping the connection open.
void fake_net_tls_clear_tx(void);

/// @brief Answer the client's Upgrade request the way a real server would.
///
/// The accept value depends on the random nonce the client just generated, so
/// only the transport can produce a correct reply. With this enabled the
/// response is injected ahead of anything already queued, which lets a test
/// script `session_ready` and transcripts up front.
void fake_net_tls_auto_handshake(uint8_t enable);
