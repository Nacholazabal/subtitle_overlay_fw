///
/// @file fake_net_tls.c
/// @brief Scriptable in-memory stand-in for the TLS transport
///

#include "fake_net_tls.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "net_tls.h"
#include "stt_ws_frame.h"

/// The real header keeps this opaque; the double only needs a marker object.
struct net_tls
{
    uint8_t open;
};

static struct net_tls instance;
static uint8_t fail_open;
static uint8_t auto_handshake;
static uint8_t handshake_done;
static uint32_t open_count;
static uint32_t close_count;

static uint8_t rx[FAKE_NET_TLS_BUFFER];
static size_t rx_used;
static size_t rx_read;

static uint8_t tx[FAKE_NET_TLS_BUFFER];
static size_t tx_used;

static char last_host[128];
static uint16_t last_port;
static uint8_t last_use_tls;

void fake_net_tls_reset(void)
{
    memset(&instance, 0, sizeof(instance));
    fail_open = 0U;
    auto_handshake = 0U;
    handshake_done = 0U;
    open_count = 0U;
    close_count = 0U;
    rx_used = 0U;
    rx_read = 0U;
    tx_used = 0U;
    last_host[0] = '\0';
    last_port = 0U;
    last_use_tls = 0U;
}

void fake_net_tls_fail_open(uint8_t fail)
{
    fail_open = fail;
}

void fake_net_tls_auto_handshake(uint8_t enable)
{
    auto_handshake = enable;
    handshake_done = 0U;
}

/// @brief Insert bytes ahead of whatever is still unread.
static void prepend_rx(char const* const text)
{
    size_t const length = strlen(text);
    size_t const tail = rx_used - rx_read;

    if ((rx_used + length) > sizeof(rx))
    {
        return;
    }
    memmove(&rx[rx_read + length], &rx[rx_read], tail);
    memcpy(&rx[rx_read], text, length);
    rx_used += length;
}

/// @brief Reply to a complete Upgrade request with a correct 101 response.
static void answer_handshake(void)
{
    char response[512];
    char accept[STT_WS_ACCEPT_SIZE];
    char key[STT_WS_KEY_SIZE];
    char const* cursor;
    size_t i;

    tx[(tx_used < sizeof(tx)) ? tx_used : (sizeof(tx) - 1U)] = '\0';
    if (strstr((char const*)tx, "\r\n\r\n") == NULL)
    {
        return; // request still incomplete
    }
    cursor = strstr((char const*)tx, "Sec-WebSocket-Key: ");
    if (cursor == NULL)
    {
        return;
    }
    cursor += strlen("Sec-WebSocket-Key: ");
    for (i = 0U; (i + 1U) < sizeof(key); i++)
    {
        if ((cursor[i] == '\r') || (cursor[i] == '\n'))
        {
            break;
        }
        key[i] = cursor[i];
    }
    key[i] = '\0';

    if (stt_ws_handshake_accept_for_key(key, accept, sizeof(accept)) < 0)
    {
        return;
    }
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n"
             "\r\n",
             accept);
    prepend_rx(response);
    handshake_done = 1U;
}

void fake_net_tls_push_rx(void const* const data, size_t size)
{
    if ((rx_used + size) > sizeof(rx))
    {
        return;
    }
    memcpy(&rx[rx_used], data, size);
    rx_used += size;
}

void fake_net_tls_push_frame(uint8_t opcode, void const* const payload, size_t size)
{
    fake_net_tls_push_fragment(opcode, 1U, payload, size);
}

void fake_net_tls_push_fragment(uint8_t opcode,
                                uint8_t fin,
                                void const* const payload,
                                size_t size)
{
    uint8_t header[4];
    size_t header_len = 2U;

    // Server frames are never masked (RFC 6455 §5.1).
    header[0] = (uint8_t)(((fin != 0U) ? 0x80U : 0x00U) | opcode);
    if (size > 125U)
    {
        header[1] = 126U;
        header[2] = (uint8_t)(size >> 8U);
        header[3] = (uint8_t)(size & 0xFFU);
        header_len = 4U;
    }
    else
    {
        header[1] = (uint8_t)size;
    }

    fake_net_tls_push_rx(header, header_len);
    if ((payload != NULL) && (size > 0U))
    {
        fake_net_tls_push_rx(payload, size);
    }
}

void fake_net_tls_push_text(char const* const text)
{
    fake_net_tls_push_frame(0x1U, text, strlen(text));
}

uint8_t const* fake_net_tls_tx(size_t* const size)
{
    if (size != NULL)
    {
        *size = tx_used;
    }
    return tx;
}

void fake_net_tls_clear_tx(void)
{
    tx_used = 0U;
}

uint32_t fake_net_tls_open_count(void)
{
    return open_count;
}

uint32_t fake_net_tls_close_count(void)
{
    return close_count;
}

void fake_net_tls_last_config(char* const host,
                              size_t host_size,
                              uint16_t* const port,
                              uint8_t* const use_tls)
{
    if (host != NULL)
    {
        snprintf(host, host_size, "%s", last_host);
    }
    if (port != NULL)
    {
        *port = last_port;
    }
    if (use_tls != NULL)
    {
        *use_tls = last_use_tls;
    }
}

// === net_tls.h implementation ==================================================================================== //

net_tls_t* net_tls_open(net_tls_config_t const* const config)
{
    open_count++;
    if (config != NULL)
    {
        snprintf(last_host, sizeof(last_host), "%s", (config->host != NULL) ? config->host : "");
        last_port = config->port;
        last_use_tls = config->use_tls;
    }

    if (fail_open != 0U)
    {
        return NULL;
    }

    instance.open = 1U;
    return &instance;
}

int net_tls_send(net_tls_t* const conn, void const* const data, size_t size, uint32_t timeout_ms)
{
    (void)timeout_ms;

    if ((conn == NULL) || (conn->open == 0U))
    {
        return -EINVAL;
    }
    if ((tx_used + size) > sizeof(tx))
    {
        return -EIO;
    }

    memcpy(&tx[tx_used], data, size);
    tx_used += size;

    if ((auto_handshake != 0U) && (handshake_done == 0U))
    {
        answer_handshake();
    }

    return (int)size;
}

int net_tls_recv(net_tls_t* const conn, void* const data, size_t size, uint32_t timeout_ms)
{
    size_t available;

    (void)timeout_ms;

    if ((conn == NULL) || (conn->open == 0U))
    {
        return -EINVAL;
    }

    available = rx_used - rx_read;
    if (available == 0U)
    {
        return -EAGAIN;
    }
    if (available > size)
    {
        available = size;
    }

    memcpy(data, &rx[rx_read], available);
    rx_read += available;
    return (int)available;
}

size_t net_tls_pending(net_tls_t* const conn)
{
    if ((conn == NULL) || (conn->open == 0U))
    {
        return 0U;
    }
    return rx_used - rx_read;
}

void net_tls_last_error(net_tls_t const* const conn, char* const out, size_t out_size)
{
    (void)conn;
    if ((out != NULL) && (out_size > 0U))
    {
        snprintf(out, out_size, "%s", "fake transport failure");
    }
}

void net_tls_close(net_tls_t* const conn)
{
    if (conn != NULL)
    {
        conn->open = 0U;
    }
    close_count++;
}
