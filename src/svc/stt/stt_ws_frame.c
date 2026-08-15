/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file stt_ws_frame.c
/// @brief RFC 6455 client framing and opening handshake, free of any I/O
///

// === Headers files inclusions ==================================================================================== //

#include "stt_ws_frame.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

// === Macros definitions ========================================================================================== //

/// Magic GUID every server appends to the client key (RFC 6455 §1.3).
#define WS_ACCEPT_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define WS_FIN_BIT       (0x80U)
#define WS_MASK_BIT      (0x80U)
#define WS_OPCODE_MASK   (0x0FU)
#define WS_RSV_MASK      (0x70U)
#define WS_LEN_MASK      (0x7FU)
#define WS_LEN_16BIT     (126U)
#define WS_LEN_64BIT     (127U)
#define WS_MASK_BYTES    (4U)
#define WS_CONTROL_MAX   (125U)

#define SHA1_DIGEST_BYTES (20U)
#define SHA1_BLOCK_BYTES  (64U)

// === Private data type declarations ============================================================================== //

typedef struct
{
    uint32_t state[5];
    uint64_t bit_count;
    uint8_t block[SHA1_BLOCK_BYTES];
    size_t block_used;
} sha1_ctx_t;

// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static uint32_t sha1_rotate_left(uint32_t value, unsigned int bits);
static void sha1_compress(sha1_ctx_t* ctx, uint8_t const* block);
static void sha1_init(sha1_ctx_t* ctx);
static void sha1_update(sha1_ctx_t* ctx, void const* data, size_t size);
static void sha1_final(sha1_ctx_t* ctx, uint8_t digest[SHA1_DIGEST_BYTES]);
static int base64_encode(uint8_t const* data, size_t size, char* out, size_t out_size);
static size_t find_header_end(char const* response, size_t length);
static int response_status_is_101(char const* response, size_t length);
static int accept_header_matches(char const* headers, size_t length, char const* expected);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //

static char const base64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// === Private function implementation ============================================================================= //

/** @brief Rotate a 32-bit word left, as SHA-1 requires. */
static uint32_t sha1_rotate_left(uint32_t value, unsigned int bits)
{
    return (uint32_t)((value << bits) | (value >> (32U - bits)));
}

/**
 * @brief Mix one 64-byte block into the SHA-1 state.
 * @param ctx Hash context.
 * @param block Block to absorb.
 * @return None.
 */
static void sha1_compress(sha1_ctx_t* const ctx, uint8_t const* const block)
{
    uint32_t w[80];
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    size_t i;

    for (i = 0U; i < 16U; i++)
    {
        w[i] = ((uint32_t)block[i * 4U] << 24U) | ((uint32_t)block[(i * 4U) + 1U] << 16U)
               | ((uint32_t)block[(i * 4U) + 2U] << 8U) | (uint32_t)block[(i * 4U) + 3U];
    }
    for (i = 16U; i < 80U; i++)
    {
        w[i] = sha1_rotate_left(w[i - 3U] ^ w[i - 8U] ^ w[i - 14U] ^ w[i - 16U], 1U);
    }

    for (i = 0U; i < 80U; i++)
    {
        uint32_t f;
        uint32_t k;

        if (i < 20U)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999U;
        }
        else if (i < 40U)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        }
        else if (i < 60U)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCU;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }

        {
            uint32_t const temp = sha1_rotate_left(a, 5U) + f + e + k + w[i];
            e = d;
            d = c;
            c = sha1_rotate_left(b, 30U);
            b = a;
            a = temp;
        }
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

/** @brief Start a SHA-1 computation. */
static void sha1_init(sha1_ctx_t* const ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xEFCDAB89U;
    ctx->state[2] = 0x98BADCFEU;
    ctx->state[3] = 0x10325476U;
    ctx->state[4] = 0xC3D2E1F0U;
}

/** @brief Absorb more bytes into a SHA-1 computation. */
static void sha1_update(sha1_ctx_t* const ctx, void const* const data, size_t size)
{
    uint8_t const* cursor = (uint8_t const*)data;

    ctx->bit_count += (uint64_t)size * 8U;
    while (size > 0U)
    {
        size_t const room = SHA1_BLOCK_BYTES - ctx->block_used;
        size_t const take = (size < room) ? size : room;

        memcpy(&ctx->block[ctx->block_used], cursor, take);
        ctx->block_used += take;
        cursor += take;
        size -= take;

        if (ctx->block_used == SHA1_BLOCK_BYTES)
        {
            sha1_compress(ctx, ctx->block);
            ctx->block_used = 0U;
        }
    }
}

/** @brief Finish a SHA-1 computation and emit the digest. */
static void sha1_final(sha1_ctx_t* const ctx, uint8_t digest[SHA1_DIGEST_BYTES])
{
    uint64_t const bit_count = ctx->bit_count;
    uint8_t padding = 0x80U;
    uint8_t length_be[8];
    unsigned int i;

    sha1_update(ctx, &padding, 1U);
    padding = 0x00U;
    while (ctx->block_used != 56U)
    {
        sha1_update(ctx, &padding, 1U);
    }
    // sha1_update() advanced bit_count over the padding; restore the real length.
    for (i = 0U; i < 8U; i++)
    {
        length_be[i] = (uint8_t)(bit_count >> (56U - (i * 8U)));
    }
    memcpy(&ctx->block[56], length_be, sizeof(length_be));
    sha1_compress(ctx, ctx->block);
    ctx->block_used = 0U;

    for (i = 0U; i < SHA1_DIGEST_BYTES; i++)
    {
        digest[i] = (uint8_t)(ctx->state[i / 4U] >> (24U - ((i % 4U) * 8U)));
    }
}

/**
 * @brief Encode bytes as standard padded Base64.
 * @return Encoded length on success, or -ENOBUFS when @p out is too small.
 */
static int base64_encode(uint8_t const* const data, size_t size, char* const out, size_t out_size)
{
    size_t const encoded = ((size + 2U) / 3U) * 4U;
    size_t in = 0U;
    size_t written = 0U;

    if (out_size <= encoded)
    {
        return -ENOBUFS;
    }

    while (in < size)
    {
        uint32_t triple = (uint32_t)data[in] << 16U;
        size_t const remaining = size - in;

        if (remaining > 1U)
        {
            triple |= (uint32_t)data[in + 1U] << 8U;
        }
        if (remaining > 2U)
        {
            triple |= (uint32_t)data[in + 2U];
        }

        out[written++] = base64_alphabet[(triple >> 18U) & 0x3FU];
        out[written++] = base64_alphabet[(triple >> 12U) & 0x3FU];
        out[written++] = (char)((remaining > 1U) ? base64_alphabet[(triple >> 6U) & 0x3FU] : '=');
        out[written++] = (char)((remaining > 2U) ? base64_alphabet[triple & 0x3FU] : '=');
        in += 3U;
    }

    out[written] = '\0';
    return (int)written;
}


/**
 * @brief Locate the end of an HTTP header block.
 * @return Bytes up to and including the terminator, or 0 when it is absent.
 */
static size_t find_header_end(char const* const response, size_t length)
{
    size_t i;

    for (i = 0U; (i + 3U) < length; i++)
    {
        if ((response[i] == '\r') && (response[i + 1U] == '\n') && (response[i + 2U] == '\r')
            && (response[i + 3U] == '\n'))
        {
            return i + 4U;
        }
    }

    return 0U;
}

/** @brief Return 1 when the status line reports protocol switching. */
static int response_status_is_101(char const* const response, size_t length)
{
    static char const prefix[] = "HTTP/1.";
    size_t const prefix_len = sizeof(prefix) - 1U;

    // "HTTP/1.x 101" is the shortest acceptable status line.
    if ((length < (prefix_len + 5U)) || (memcmp(response, prefix, prefix_len) != 0))
    {
        return 0;
    }

    return (memcmp(&response[prefix_len + 1U], " 101", 4U) == 0) ? 1 : 0;
}

/** @brief Return 1 when a `Sec-WebSocket-Accept` header carries exactly @p expected. */
static int accept_header_matches(char const* const headers, size_t length, char const* const expected)
{
    static char const name[] = "sec-websocket-accept:";
    size_t const name_len = sizeof(name) - 1U;
    size_t const expected_len = strlen(expected);
    size_t i;

    for (i = 0U; (i + name_len) <= length; i++)
    {
        size_t j;
        size_t value;
        size_t value_end;
        uint8_t matched = 1U;

        if ((i != 0U) && (headers[i - 1U] != '\n'))
        {
            continue; // header names only start a line
        }
        for (j = 0U; j < name_len; j++)
        {
            if ((char)tolower((unsigned char)headers[i + j]) != name[j])
            {
                matched = 0U;
                break;
            }
        }
        if (matched == 0U)
        {
            continue;
        }

        value = i + name_len;
        while ((value < length) && ((headers[value] == ' ') || (headers[value] == '\t')))
        {
            value++;
        }
        value_end = value;
        while ((value_end < length) && (headers[value_end] != '\r') && (headers[value_end] != '\n'))
        {
            value_end++;
        }

        return (((value_end - value) == expected_len)
                && (memcmp(&headers[value], expected, expected_len) == 0))
                   ? 1
                   : 0;
    }

    return 0;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Encode one masked client frame.
 * @param out Destination buffer.
 * @param out_size Destination capacity.
 * @param opcode Frame opcode.
 * @param payload Payload bytes, may be NULL when @p payload_len is 0.
 * @param payload_len Payload length in bytes.
 * @param mask_key Masking key drawn by the caller.
 * @return Bytes written on success, or a negative errno-style value.
 */
int stt_ws_frame_encode(uint8_t* const out,
                        size_t out_size,
                        stt_ws_opcode_e opcode,
                        void const* const payload,
                        size_t payload_len,
                        uint32_t mask_key)
{
    uint8_t mask[WS_MASK_BYTES];
    uint8_t const* source = (uint8_t const*)payload;
    size_t header = 2U;
    size_t total;
    size_t i;

    if ((out == NULL) || ((payload == NULL) && (payload_len > 0U)))
    {
        return -EINVAL;
    }
    // Control frames may not be fragmented and cap at 125 payload bytes.
    if ((((uint32_t)opcode & 0x08U) != 0U) && (payload_len > WS_CONTROL_MAX))
    {
        return -EINVAL;
    }

    if (payload_len > UINT16_MAX)
    {
        header += 8U;
    }
    else if (payload_len > WS_CONTROL_MAX)
    {
        header += 2U;
    }
    header += WS_MASK_BYTES;

    total = header + payload_len;
    if (out_size < total)
    {
        return -ENOBUFS;
    }

    out[0] = (uint8_t)(WS_FIN_BIT | ((uint32_t)opcode & WS_OPCODE_MASK));
    if (payload_len > UINT16_MAX)
    {
        out[1] = (uint8_t)(WS_MASK_BIT | WS_LEN_64BIT);
        for (i = 0U; i < 8U; i++)
        {
            out[2U + i] = (uint8_t)((uint64_t)payload_len >> (56U - (i * 8U)));
        }
    }
    else if (payload_len > WS_CONTROL_MAX)
    {
        out[1] = (uint8_t)(WS_MASK_BIT | WS_LEN_16BIT);
        out[2] = (uint8_t)(payload_len >> 8U);
        out[3] = (uint8_t)(payload_len & 0xFFU);
    }
    else
    {
        out[1] = (uint8_t)(WS_MASK_BIT | (uint8_t)payload_len);
    }

    for (i = 0U; i < WS_MASK_BYTES; i++)
    {
        mask[i] = (uint8_t)(mask_key >> (24U - (i * 8U)));
        out[(header - WS_MASK_BYTES) + i] = mask[i];
    }

    for (i = 0U; i < payload_len; i++)
    {
        out[header + i] = (uint8_t)(source[i] ^ mask[i % WS_MASK_BYTES]);
    }

    return (int)total;
}

/**
 * @brief Decode one server frame from a receive buffer.
 * @param data Received bytes.
 * @param size Number of bytes available.
 * @param max_payload Largest payload accepted.
 * @param frame Decoded frame destination.
 * @return 0, -EAGAIN, -EMSGSIZE or -EPROTO.
 */
int stt_ws_frame_decode(uint8_t const* const data,
                        size_t size,
                        size_t max_payload,
                        stt_ws_frame_t* const frame)
{
    uint64_t payload_len;
    size_t header = 2U;
    uint8_t length_field;

    if ((data == NULL) || (frame == NULL))
    {
        return -EINVAL;
    }
    if (size < header)
    {
        return -EAGAIN;
    }
    // Reserved bits imply an extension this client never negotiated.
    if ((data[0] & WS_RSV_MASK) != 0U)
    {
        return -EPROTO;
    }
    // RFC 6455 §5.1: a server must never mask the frames it sends.
    if ((data[1] & WS_MASK_BIT) != 0U)
    {
        return -EPROTO;
    }

    length_field = (uint8_t)(data[1] & WS_LEN_MASK);
    if (length_field == WS_LEN_16BIT)
    {
        header += 2U;
        if (size < header)
        {
            return -EAGAIN;
        }
        payload_len = ((uint64_t)data[2] << 8U) | (uint64_t)data[3];
    }
    else if (length_field == WS_LEN_64BIT)
    {
        unsigned int i;

        header += 8U;
        if (size < header)
        {
            return -EAGAIN;
        }
        payload_len = 0U;
        for (i = 0U; i < 8U; i++)
        {
            payload_len = (payload_len << 8U) | (uint64_t)data[2U + i];
        }
        if ((payload_len >> 63U) != 0U)
        {
            return -EPROTO; // the high bit must be clear
        }
    }
    else
    {
        payload_len = (uint64_t)length_field;
    }

    {
        uint8_t const opcode = (uint8_t)(data[0] & WS_OPCODE_MASK);
        uint8_t const is_control = ((opcode & 0x08U) != 0U) ? 1U : 0U;

        if ((is_control != 0U)
            && ((payload_len > WS_CONTROL_MAX) || ((data[0] & WS_FIN_BIT) == 0U)))
        {
            return -EPROTO; // control frames are short and never fragmented
        }
        if (payload_len > (uint64_t)max_payload)
        {
            return -EMSGSIZE;
        }
        if ((uint64_t)(size - header) < payload_len)
        {
            return -EAGAIN;
        }

        frame->opcode = (stt_ws_opcode_e)opcode;
        frame->fin = ((data[0] & WS_FIN_BIT) != 0U) ? 1U : 0U;
        frame->payload_len = payload_len;
        frame->header_len = header;
        frame->payload = &data[header];
    }

    return 0;
}

/**
 * @brief Derive the `Sec-WebSocket-Accept` value a server must return for a key.
 * @return Accept length on success, or a negative errno-style value.
 */
int stt_ws_handshake_accept_for_key(char const* const key, char* const out, size_t out_size)
{
    sha1_ctx_t ctx;
    uint8_t digest[SHA1_DIGEST_BYTES];

    if ((key == NULL) || (out == NULL))
    {
        return -EINVAL;
    }

    sha1_init(&ctx);
    sha1_update(&ctx, key, strlen(key));
    sha1_update(&ctx, WS_ACCEPT_GUID, strlen(WS_ACCEPT_GUID));
    sha1_final(&ctx, digest);

    return base64_encode(digest, sizeof(digest), out, out_size);
}

/**
 * @brief Build the opening HTTP Upgrade request and the accept value to expect.
 * @return Request length on success, or a negative errno-style value.
 */
int stt_ws_handshake_build_request(char* const out,
                                   size_t out_size,
                                   char const* const host,
                                   char const* const path,
                                   uint8_t const nonce[STT_WS_KEY_NONCE_BYTES],
                                   char* const expected_accept,
                                   size_t expected_accept_size)
{
    char key[STT_WS_KEY_SIZE];
    int length;

    if ((out == NULL) || (host == NULL) || (path == NULL) || (nonce == NULL)
        || (expected_accept == NULL) || (host[0] == '\0') || (path[0] != '/'))
    {
        return -EINVAL;
    }

    if (base64_encode(nonce, STT_WS_KEY_NONCE_BYTES, key, sizeof(key)) < 0)
    {
        return -ENOBUFS;
    }
    if (stt_ws_handshake_accept_for_key(key, expected_accept, expected_accept_size) < 0)
    {
        return -ENOBUFS;
    }

    length = snprintf(out,
                      out_size,
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: %s\r\n"
                      "Sec-WebSocket-Version: 13\r\n"
                      "User-Agent: subtitle-overlay-fw/1\r\n"
                      "\r\n",
                      path,
                      host,
                      key);

    if ((length < 0) || ((size_t)length >= out_size))
    {
        return -ENOBUFS;
    }

    return length;
}

/**
 * @brief Validate a server handshake response.
 * @return 0, -EAGAIN or -EPROTO.
 */
int stt_ws_handshake_validate_response(char const* const response,
                                       size_t length,
                                       char const* const expected_accept,
                                       size_t* const header_len)
{
    size_t end;

    if ((response == NULL) || (expected_accept == NULL) || (header_len == NULL))
    {
        return -EINVAL;
    }

    end = find_header_end(response, length);
    if (end == 0U)
    {
        return -EAGAIN;
    }
    if (response_status_is_101(response, end) == 0)
    {
        return -EPROTO;
    }
    if (accept_header_matches(response, end, expected_accept) == 0)
    {
        return -EPROTO;
    }

    *header_len = end;
    return 0;
}

// === End of documentation ======================================================================================== //
