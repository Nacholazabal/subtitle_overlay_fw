/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file net_tls.c
/// @brief Outbound TCP/TLS stream with full certificate and hostname validation
///
/// Written against the OpenSSL 1.0.2 API that the board's PetaLinux 2018.3
/// rootfs provides. Newer conveniences are deliberately avoided:
///   * `SSLv23_client_method()` + `SSL_OP_NO_*`, not `TLS_client_method()` and
///     `SSL_CTX_set_min_proto_version()` (both 1.1.0+);
///   * `SSL_get0_param()` + `X509_VERIFY_PARAM_set1_host()`, not `SSL_set1_host()`
///     (1.1.0+);
///   * explicit library initialization, which 1.1.0 made implicit.
///

// === Headers files inclusions ==================================================================================== //

#include "net_tls.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

// === Macros definitions ========================================================================================== //

#define NET_TLS_POLL_SLICE_MS (100)
#define NET_TLS_PORT_STR_LEN  (8U)

// === Private data type declarations ============================================================================== //

struct net_tls
{
    int fd;
    SSL_CTX* ctx;
    SSL* ssl;
    uint8_t in_use;
    uint8_t use_tls;
    char error[NET_TLS_ERROR_MAX];
};

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static void set_error(struct net_tls* conn, char const* format, ...);
static uint64_t now_ms(void);
static int remaining_ms(uint64_t deadline);
static int set_nonblocking(int fd);
static int wait_ready(int fd, short events, uint64_t deadline);
static int connect_tcp(struct net_tls* conn, char const* host, uint16_t port, uint32_t timeout_ms);
static int build_context(struct net_tls* conn, net_tls_config_t const* config);
static int run_handshake(struct net_tls* conn, char const* host, uint32_t timeout_ms);
static void release(struct net_tls* conn);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //

/// The firmware opens exactly one outbound session, so a single slot avoids
/// dynamic allocation in the reconnect path.
static struct net_tls instance;
/// Preserved so ::net_tls_last_error can explain a failure that returned NULL.
static char open_error[NET_TLS_ERROR_MAX];

// === Private function implementation ============================================================================= //

/** @brief Record a human-readable failure description. */
static void set_error(struct net_tls* const conn, char const* const format, ...)
{
    char text[NET_TLS_ERROR_MAX];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    snprintf(open_error, sizeof(open_error), "%s", text);
    if (conn != NULL)
    {
        snprintf(conn->error, sizeof(conn->error), "%s", text);
    }
}

/** @brief Monotonic milliseconds, immune to a wall-clock step from NTP. */
static uint64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0U;
    }

    return ((uint64_t)ts.tv_sec * 1000U) + ((uint64_t)ts.tv_nsec / 1000000U);
}

/** @brief Milliseconds left before @p deadline, clamped at 0. */
static int remaining_ms(uint64_t deadline)
{
    uint64_t const now = now_ms();

    return (now >= deadline) ? 0 : (int)(deadline - now);
}

static int set_nonblocking(int fd)
{
    int const flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
    {
        return -EIO;
    }

    return (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) ? 0 : -EIO;
}

/**
 * @brief Wait until @p fd is ready, in slices so a caller can bound the wait.
 * @return 0 when ready, -ETIMEDOUT at the deadline, or -EIO.
 */
static int wait_ready(int fd, short events, uint64_t deadline)
{
    for (;;)
    {
        struct pollfd pfd;
        int left = remaining_ms(deadline);
        int status;

        if (left <= 0)
        {
            return -ETIMEDOUT;
        }
        if (left > NET_TLS_POLL_SLICE_MS)
        {
            left = NET_TLS_POLL_SLICE_MS;
        }

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = fd;
        pfd.events = events;
        status = poll(&pfd, 1U, left);
        if (status > 0)
        {
            return 0;
        }
        if ((status < 0) && (errno != EINTR))
        {
            return -EIO;
        }
    }
}

/**
 * @brief Resolve @p host and open a non-blocking TCP connection.
 * @return 0 on success, or a negative errno-style value.
 */
static int connect_tcp(struct net_tls* const conn,
                       char const* const host,
                       uint16_t port,
                       uint32_t timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    struct addrinfo* it;
    char port_str[NET_TLS_PORT_STR_LEN];
    uint64_t const deadline = now_ms() + (uint64_t)timeout_ms;
    int status;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    status = getaddrinfo(host, port_str, &hints, &result);
    if (status != 0)
    {
        set_error(conn, "dns: %s", gai_strerror(status));
        return -EHOSTUNREACH;
    }

    for (it = result; it != NULL; it = it->ai_next)
    {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        int socket_error = 0;
        socklen_t error_len = sizeof(socket_error);

        if (fd < 0)
        {
            continue;
        }
        if (set_nonblocking(fd) != 0)
        {
            close(fd);
            continue;
        }

        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0)
        {
            conn->fd = fd;
            break;
        }
        if ((errno == EINPROGRESS) && (wait_ready(fd, POLLOUT, deadline) == 0)
            && (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0)
            && (socket_error == 0))
        {
            conn->fd = fd;
            break;
        }

        close(fd);
    }

    freeaddrinfo(result);
    if (conn->fd < 0)
    {
        set_error(conn, "tcp connect to %s:%u failed", host, (unsigned)port);
        return -ECONNREFUSED;
    }

    return 0;
}

/**
 * @brief Create the SSL context with verification switched on.
 * @return 0 on success, or a negative errno-style value.
 */
static int build_context(struct net_tls* const conn, net_tls_config_t const* const config)
{
    static uint8_t initialized = 0U;
    long options;

    if (initialized == 0U)
    {
        // Implicit in OpenSSL 1.1.0+, still required by the board's 1.0.2.
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        initialized = 1U;
    }

    conn->ctx = SSL_CTX_new(SSLv23_client_method());
    if (conn->ctx == NULL)
    {
        set_error(conn, "tls: context allocation failed");
        return -ENOMEM;
    }

    // SSLv23_client_method() means "best available"; disable everything below
    // TLS 1.2 explicitly, which is how 1.0.2 expresses a minimum version.
    options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1
              | SSL_OP_NO_COMPRESSION;
    (void)SSL_CTX_set_options(conn->ctx, options);
    SSL_CTX_set_verify(conn->ctx, SSL_VERIFY_PEER, NULL);

    if ((config->ca_file != NULL) || (config->ca_dir != NULL))
    {
        if (SSL_CTX_load_verify_locations(conn->ctx, config->ca_file, config->ca_dir) != 1)
        {
            set_error(conn, "tls: cannot load CA store (file=%s dir=%s)",
                      (config->ca_file != NULL) ? config->ca_file : "-",
                      (config->ca_dir != NULL) ? config->ca_dir : "-");
            return -ENOENT;
        }
    }
    else if (SSL_CTX_set_default_verify_paths(conn->ctx) != 1)
    {
        set_error(conn, "tls: cannot load the default CA store");
        return -ENOENT;
    }
    else
    {
        // Default store loaded.
    }

    return 0;
}

/**
 * @brief Run the TLS handshake and confirm the peer certificate.
 * @return 0 on success, or a negative errno-style value.
 */
static int run_handshake(struct net_tls* const conn, char const* const host, uint32_t timeout_ms)
{
    uint64_t const deadline = now_ms() + (uint64_t)timeout_ms;
    X509_VERIFY_PARAM* param;
    long verify_result;

    conn->ssl = SSL_new(conn->ctx);
    if (conn->ssl == NULL)
    {
        set_error(conn, "tls: session allocation failed");
        return -ENOMEM;
    }
    if (SSL_set_fd(conn->ssl, conn->fd) != 1)
    {
        set_error(conn, "tls: cannot attach the socket");
        return -EIO;
    }

    // SNI: ngrok routes on it, and without it the edge cannot pick a certificate.
    if (SSL_set_tlsext_host_name(conn->ssl, host) != 1)
    {
        set_error(conn, "tls: cannot set SNI for %s", host);
        return -EIO;
    }

    // Hostname verification happens inside the handshake, so a mismatched
    // certificate fails here rather than being checked (or forgotten) later.
    param = SSL_get0_param(conn->ssl);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (X509_VERIFY_PARAM_set1_host(param, host, 0U) != 1)
    {
        set_error(conn, "tls: cannot pin the expected host %s", host);
        return -EIO;
    }

    for (;;)
    {
        int const status = SSL_connect(conn->ssl);
        int reason;

        if (status == 1)
        {
            break;
        }

        reason = SSL_get_error(conn->ssl, status);
        if ((reason == SSL_ERROR_WANT_READ) || (reason == SSL_ERROR_WANT_WRITE))
        {
            short const events = (reason == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;

            if (wait_ready(conn->fd, events, deadline) != 0)
            {
                set_error(conn, "tls: handshake timed out");
                return -ETIMEDOUT;
            }
            continue;
        }

        set_error(conn, "tls: handshake failed (ssl error %d, %s)", reason,
                  ERR_reason_error_string(ERR_get_error()));
        return -ECONNABORTED;
    }

    verify_result = SSL_get_verify_result(conn->ssl);
    if (verify_result != X509_V_OK)
    {
        // A wrong system clock lands here as "certificate is not yet valid",
        // which is the board's expected failure until NTP has run.
        set_error(conn, "tls: certificate rejected (%ld: %s)", verify_result,
                  X509_verify_cert_error_string(verify_result));
        return -EACCES;
    }

    {
        X509* const peer = SSL_get_peer_certificate(conn->ssl);

        if (peer == NULL)
        {
            set_error(conn, "tls: server presented no certificate");
            return -EACCES;
        }
        X509_free(peer);
    }

    return 0;
}

/** @brief Release every resource the connection owns. */
static void release(struct net_tls* const conn)
{
    if (conn->ssl != NULL)
    {
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }
    if (conn->ctx != NULL)
    {
        SSL_CTX_free(conn->ctx);
        conn->ctx = NULL;
    }
    if (conn->fd >= 0)
    {
        close(conn->fd);
        conn->fd = -1;
    }
}

// === Public function implementation ============================================================================== //

/**
 * @brief Resolve, connect and complete a verified TLS handshake.
 * @param config Connection settings.
 * @return Connection handle, or NULL on failure.
 */
net_tls_t* net_tls_open(net_tls_config_t const* const config)
{
    struct net_tls* const conn = &instance;

    if ((config == NULL) || (config->host == NULL) || (config->host[0] == '\0')
        || (config->port == 0U))
    {
        set_error(NULL, "invalid connection configuration");
        return NULL;
    }
    if (conn->in_use != 0U)
    {
        set_error(NULL, "a connection is already open");
        return NULL;
    }

    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->in_use = 1U;
    conn->use_tls = (config->use_tls != 0U) ? 1U : 0U;

    if (connect_tcp(conn, config->host, config->port, config->connect_timeout_ms) != 0)
    {
        release(conn);
        conn->in_use = 0U;
        return NULL;
    }

    if (conn->use_tls == 0U)
    {
        return conn;
    }

    if ((build_context(conn, config) != 0)
        || (run_handshake(conn, config->host, config->handshake_timeout_ms) != 0))
    {
        release(conn);
        conn->in_use = 0U;
        return NULL;
    }

    return conn;
}

/**
 * @brief Send exactly @p size bytes.
 * @return @p size on success, or a negative errno-style value.
 */
int net_tls_send(net_tls_t* const conn, void const* const data, size_t size, uint32_t timeout_ms)
{
    uint8_t const* cursor = (uint8_t const*)data;
    uint64_t const deadline = now_ms() + (uint64_t)timeout_ms;
    size_t remaining = size;

    if ((conn == NULL) || (conn->in_use == 0U) || ((data == NULL) && (size > 0U)))
    {
        return -EINVAL;
    }

    while (remaining > 0U)
    {
        int written;
        int reason;

        if (conn->use_tls == 0U)
        {
            ssize_t const sent = send(conn->fd, cursor, remaining, MSG_NOSIGNAL);

            if (sent > 0)
            {
                cursor += sent;
                remaining -= (size_t)sent;
                continue;
            }
            if ((sent < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)))
            {
                if (wait_ready(conn->fd, POLLOUT, deadline) != 0)
                {
                    return -ETIMEDOUT;
                }
                continue;
            }
            set_error(conn, "send failed: %s", strerror(errno));
            return -EIO;
        }

        written = SSL_write(conn->ssl, cursor, (int)remaining);
        if (written > 0)
        {
            cursor += written;
            remaining -= (size_t)written;
            continue;
        }

        reason = SSL_get_error(conn->ssl, written);
        if ((reason == SSL_ERROR_WANT_READ) || (reason == SSL_ERROR_WANT_WRITE))
        {
            short const events = (reason == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;

            if (wait_ready(conn->fd, events, deadline) != 0)
            {
                return -ETIMEDOUT;
            }
            continue;
        }

        set_error(conn, "tls send failed (ssl error %d)", reason);
        return -EIO;
    }

    return (int)size;
}

/**
 * @brief Read up to @p size bytes.
 * @return Bytes read, 0 at end of stream, -EAGAIN on timeout, or negative.
 */
int net_tls_recv(net_tls_t* const conn, void* const data, size_t size, uint32_t timeout_ms)
{
    uint64_t const deadline = now_ms() + (uint64_t)timeout_ms;

    if ((conn == NULL) || (conn->in_use == 0U) || (data == NULL) || (size == 0U))
    {
        return -EINVAL;
    }

    for (;;)
    {
        int received;
        int reason;

        if (conn->use_tls == 0U)
        {
            ssize_t const got = recv(conn->fd, data, size, 0);

            if (got > 0)
            {
                return (int)got;
            }
            if (got == 0)
            {
                return 0;
            }
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR))
            {
                if (wait_ready(conn->fd, POLLIN, deadline) != 0)
                {
                    return -EAGAIN;
                }
                continue;
            }
            set_error(conn, "recv failed: %s", strerror(errno));
            return -EIO;
        }

        received = SSL_read(conn->ssl, data, (int)size);
        if (received > 0)
        {
            return received;
        }

        reason = SSL_get_error(conn->ssl, received);
        if (reason == SSL_ERROR_ZERO_RETURN)
        {
            return 0;
        }
        if ((reason == SSL_ERROR_WANT_READ) || (reason == SSL_ERROR_WANT_WRITE))
        {
            short const events = (reason == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;

            if (wait_ready(conn->fd, events, deadline) != 0)
            {
                return -EAGAIN;
            }
            continue;
        }
        if ((reason == SSL_ERROR_SYSCALL) && (received == 0))
        {
            return 0; // peer closed without a shutdown alert
        }

        set_error(conn, "tls recv failed (ssl error %d)", reason);
        return -EIO;
    }
}

/**
 * @brief Report bytes already decrypted and waiting in the TLS buffer.
 * @return Pending byte count, or 0.
 */
size_t net_tls_pending(net_tls_t* const conn)
{
    if ((conn == NULL) || (conn->in_use == 0U) || (conn->use_tls == 0U) || (conn->ssl == NULL))
    {
        return 0U;
    }

    return (size_t)SSL_pending(conn->ssl);
}

/**
 * @brief Copy a description of the most recent failure.
 * @return None.
 */
void net_tls_last_error(net_tls_t const* const conn, char* const out, size_t out_size)
{
    if ((out == NULL) || (out_size == 0U))
    {
        return;
    }

    snprintf(out, out_size, "%s", (conn != NULL) ? conn->error : open_error);
}

/**
 * @brief Shut down and release the connection.
 * @return None.
 */
void net_tls_close(net_tls_t* const conn)
{
    if ((conn == NULL) || (conn->in_use == 0U))
    {
        return;
    }

    if ((conn->use_tls != 0U) && (conn->ssl != NULL))
    {
        // One try only: a peer that already vanished must not stall shutdown.
        (void)SSL_shutdown(conn->ssl);
    }
    release(conn);
    conn->in_use = 0U;
}

// === End of documentation ======================================================================================== //
