/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file net_tls.h
/// @brief Outbound TCP/TLS stream with full certificate and hostname validation
///
/// One outbound connection at a time, owned by a single thread. The handle is
/// opaque so no OpenSSL type reaches the rest of the firmware, which also keeps
/// this header mockable in host unit tests.
///
/// Peer verification is **not optional**: there is no argument, environment
/// variable or build flag that skips certificate or hostname checking. A
/// certificate that fails to validate fails the connection.
///
/// ::net_tls_config_t::use_tls exists only to reach a plaintext `ws://` endpoint
/// during LAN bring-up. That is a different URL scheme, not a weakened `wss://`:
/// when it is clear, TLS runs with full validation.
///
/// Target constraint: the board ships **OpenSSL 1.0.2o**, so the implementation
/// stays within that API surface (no `TLS_client_method`, no
/// `SSL_CTX_set_min_proto_version`, no `SSL_set1_host`, no TLS 1.3).
///

// === Headers files inclusions ==================================================================================== //

#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

/// Capacity a caller should give ::net_tls_last_error.
#define NET_TLS_ERROR_MAX (192U)

// === Public data type declarations =============================================================================== //

/// @brief Opaque connection handle; storage is owned by the module.
typedef struct net_tls net_tls_t;

/// @brief Everything needed to open one verified connection.
typedef struct
{
    char const* host;    ///< Host name, used for DNS, SNI and certificate matching.
    uint16_t port;       ///< TCP port.
    char const* ca_file; ///< CA bundle path, or NULL for the system default paths.
    char const* ca_dir;  ///< Hashed CA directory, or NULL.
    uint32_t connect_timeout_ms;
    uint32_t handshake_timeout_ms;
    uint8_t use_tls; ///< 0 selects plaintext, for `ws://` LAN bring-up only.
} net_tls_config_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/**
 * @brief Resolve, connect and, when configured, complete a verified TLS handshake.
 *
 * Blocking with bounded timeouts; call it only from the worker thread that owns
 * the connection, never from a QP/C state handler.
 *
 * @param config Connection settings.
 * @return Connection handle, or NULL on failure. Use ::net_tls_last_error for
 *         a description, including the X.509 verification result when relevant.
 */
net_tls_t* net_tls_open(net_tls_config_t const* config);

/**
 * @brief Send exactly @p size bytes.
 * @param conn Connection handle.
 * @param data Source buffer.
 * @param size Byte count.
 * @param timeout_ms Deadline for the whole transfer.
 * @return @p size on success, -ETIMEDOUT when the deadline passed with bytes
 *         still pending, or another negative errno-style value.
 */
int net_tls_send(net_tls_t* conn, void const* data, size_t size, uint32_t timeout_ms);

/**
 * @brief Read up to @p size bytes.
 * @param conn Connection handle.
 * @param data Destination buffer.
 * @param size Destination capacity.
 * @param timeout_ms Deadline; 0 polls without waiting.
 * @return Bytes read, 0 when the peer closed the stream, -EAGAIN when the
 *         deadline passed with nothing to read, or another negative value.
 */
int net_tls_recv(net_tls_t* conn, void* data, size_t size, uint32_t timeout_ms);

/**
 * @brief Report bytes already decrypted and waiting in the TLS buffer.
 *
 * A poll on the socket cannot see these, so a reader that ignores them can
 * stall with a complete message sitting in memory.
 *
 * @param conn Connection handle.
 * @return Pending byte count, or 0.
 */
size_t net_tls_pending(net_tls_t* conn);

/**
 * @brief Copy a description of the most recent failure.
 * @param conn Connection handle, or NULL for the last open failure.
 * @param out Destination, NUL-terminated on return.
 * @param out_size Destination capacity; ::NET_TLS_ERROR_MAX is enough.
 * @return None.
 */
void net_tls_last_error(net_tls_t const* conn, char* out, size_t out_size);

/**
 * @brief Shut down and release the connection; safe with NULL.
 * @param conn Connection handle.
 * @return None.
 */
void net_tls_close(net_tls_t* conn);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
