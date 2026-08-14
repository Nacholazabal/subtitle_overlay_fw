#include <errno.h>
#include <string.h>

#include "unity.h"
#include "stt_ws_frame.h"

// RFC 6455 §1.3 worked example: the key "dGhlIHNhbXBsZSBub25jZQ==" is the Base64
// of the 16 ASCII bytes "the sample nonce", and the server must answer with
// "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=". Pinning this vector proves the SHA-1 and
// Base64 implementations, not just that they are self-consistent.
static uint8_t const RFC_NONCE[STT_WS_KEY_NONCE_BYTES] = "the sample nonce";
#define RFC_KEY    "dGhlIHNhbXBsZSBub25jZQ=="
#define RFC_ACCEPT "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

static uint8_t frame_buffer[4096];
static char request[512];
static char accept[STT_WS_ACCEPT_SIZE];

void setUp(void)
{
    memset(frame_buffer, 0, sizeof(frame_buffer));
    memset(request, 0, sizeof(request));
    memset(accept, 0, sizeof(accept));
}

void tearDown(void)
{}

/// @brief Build one unmasked server frame, the only shape a server may send.
static size_t build_server_frame(uint8_t opcode,
                                 uint8_t fin,
                                 uint8_t const* payload,
                                 size_t payload_len)
{
    size_t header = 2U;

    frame_buffer[0] = (uint8_t)(((fin != 0U) ? 0x80U : 0x00U) | opcode);
    if (payload_len > 65535U)
    {
        unsigned int i;

        frame_buffer[1] = 127U;
        for (i = 0U; i < 8U; i++)
        {
            frame_buffer[2U + i] = (uint8_t)((uint64_t)payload_len >> (56U - (i * 8U)));
        }
        header += 8U;
    }
    else if (payload_len > 125U)
    {
        frame_buffer[1] = 126U;
        frame_buffer[2] = (uint8_t)(payload_len >> 8U);
        frame_buffer[3] = (uint8_t)(payload_len & 0xFFU);
        header += 2U;
    }
    else
    {
        frame_buffer[1] = (uint8_t)payload_len;
    }

    if ((payload != NULL) && (payload_len > 0U))
    {
        memcpy(&frame_buffer[header], payload, payload_len);
    }
    return header + payload_len;
}

// === Handshake =================================================================================================== //

void test_stt_ws_handshake_matches_the_rfc6455_worked_example(void)
{
    int const length = stt_ws_handshake_build_request(request,
                                                      sizeof(request),
                                                      "example.com:8765",
                                                      "/stt/stream",
                                                      RFC_NONCE,
                                                      accept,
                                                      sizeof(accept));

    TEST_ASSERT_GREATER_THAN_INT(0, length);
    TEST_ASSERT_EQUAL_INT((int)strlen(request), length);
    TEST_ASSERT_EQUAL_STRING(RFC_ACCEPT, accept);
    TEST_ASSERT_NOT_NULL(strstr(request, "GET /stt/stream HTTP/1.1\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(request, "Host: example.com:8765\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(request, "Upgrade: websocket\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(request, "Connection: Upgrade\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(request, "Sec-WebSocket-Version: 13\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(request, "Sec-WebSocket-Key: " RFC_KEY "\r\n"));
    // The request must end on the blank line that closes the header block.
    TEST_ASSERT_EQUAL_STRING("\r\n\r\n", &request[length - 4]);
}

void test_stt_ws_handshake_rejects_invalid_arguments_and_small_buffers(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_ws_handshake_build_request(request, sizeof(request), "", "/x",
                                                         RFC_NONCE, accept, sizeof(accept)));
    // A request target must be an absolute path.
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_ws_handshake_build_request(request, sizeof(request), "host", "x",
                                                         RFC_NONCE, accept, sizeof(accept)));
    TEST_ASSERT_EQUAL_INT(-ENOBUFS,
                          stt_ws_handshake_build_request(request, 32U, "host", "/x", RFC_NONCE,
                                                         accept, sizeof(accept)));
    TEST_ASSERT_EQUAL_INT(-ENOBUFS,
                          stt_ws_handshake_build_request(request, sizeof(request), "host", "/x",
                                                         RFC_NONCE, accept, 4U));
}

void test_stt_ws_handshake_accepts_a_valid_response_case_insensitively(void)
{
    // Real servers vary header casing; HTTP field names are case-insensitive.
    char const response[] = "HTTP/1.1 101 Switching Protocols\r\n"
                            "upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "SEC-WEBSOCKET-ACCEPT:  " RFC_ACCEPT "\r\n"
                            "\r\n";
    size_t header_len = 0U;

    TEST_ASSERT_EQUAL_INT(0,
                          stt_ws_handshake_validate_response(response, strlen(response),
                                                             RFC_ACCEPT, &header_len));
    TEST_ASSERT_EQUAL_size_t(strlen(response), header_len);
}

void test_stt_ws_handshake_reports_where_the_headers_end_so_frames_survive(void)
{
    char const response[] = "HTTP/1.1 101 Switching Protocols\r\n"
                            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                            "\r\n"
                            "FRAMEBYTES";
    size_t header_len = 0U;

    TEST_ASSERT_EQUAL_INT(0,
                          stt_ws_handshake_validate_response(response, strlen(response),
                                                             RFC_ACCEPT, &header_len));
    TEST_ASSERT_EQUAL_STRING("FRAMEBYTES", &response[header_len]);
}

void test_stt_ws_handshake_waits_for_the_complete_header_block(void)
{
    char const partial[] = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n";
    size_t header_len = 0U;

    TEST_ASSERT_EQUAL_INT(-EAGAIN,
                          stt_ws_handshake_validate_response(partial, strlen(partial), RFC_ACCEPT,
                                                             &header_len));
}

void test_stt_ws_handshake_rejects_wrong_status_and_wrong_accept(void)
{
    char const not_switching[] = "HTTP/1.1 200 OK\r\n"
                                 "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
    char const wrong_accept[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n";
    char const missing_accept[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                  "Upgrade: websocket\r\n\r\n";
    // A value that merely starts with the expected accept must not pass.
    char const trailing_junk[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                 "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=x\r\n\r\n";
    size_t header_len = 0U;

    TEST_ASSERT_EQUAL_INT(-EPROTO,
                          stt_ws_handshake_validate_response(not_switching, strlen(not_switching),
                                                             RFC_ACCEPT, &header_len));
    TEST_ASSERT_EQUAL_INT(-EPROTO,
                          stt_ws_handshake_validate_response(wrong_accept, strlen(wrong_accept),
                                                             RFC_ACCEPT, &header_len));
    TEST_ASSERT_EQUAL_INT(-EPROTO,
                          stt_ws_handshake_validate_response(missing_accept, strlen(missing_accept),
                                                             RFC_ACCEPT, &header_len));
    TEST_ASSERT_EQUAL_INT(-EPROTO,
                          stt_ws_handshake_validate_response(trailing_junk, strlen(trailing_junk),
                                                             RFC_ACCEPT, &header_len));
}

// === Encoding ==================================================================================================== //

void test_stt_ws_frame_encode_masks_a_short_text_payload(void)
{
    char const payload[] = "Hi";
    uint32_t const mask_key = 0x01020304U;
    int const written = stt_ws_frame_encode(frame_buffer, sizeof(frame_buffer),
                                            STT_WS_OPCODE_TEXT, payload, strlen(payload),
                                            mask_key);

    // 2 header bytes + 4 mask bytes + 2 payload bytes.
    TEST_ASSERT_EQUAL_INT(8, written);
    TEST_ASSERT_EQUAL_HEX8(0x81U, frame_buffer[0]); // FIN + text
    TEST_ASSERT_EQUAL_HEX8(0x82U, frame_buffer[1]); // MASK + length 2
    TEST_ASSERT_EQUAL_HEX8(0x01U, frame_buffer[2]);
    TEST_ASSERT_EQUAL_HEX8(0x02U, frame_buffer[3]);
    TEST_ASSERT_EQUAL_HEX8(0x03U, frame_buffer[4]);
    TEST_ASSERT_EQUAL_HEX8(0x04U, frame_buffer[5]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)('H' ^ 0x01U), frame_buffer[6]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)('i' ^ 0x02U), frame_buffer[7]);
}

void test_stt_ws_frame_encode_uses_the_16_bit_length_for_an_audio_chunk(void)
{
    // One 20 ms chunk: 20-byte protocol header + 1920 bytes of S16_LE PCM.
    uint8_t payload[1940];
    int written;

    memset(payload, 0xA5, sizeof(payload));
    written = stt_ws_frame_encode(frame_buffer, sizeof(frame_buffer), STT_WS_OPCODE_BINARY,
                                  payload, sizeof(payload), 0x00000000U);

    TEST_ASSERT_EQUAL_INT((int)(2 + 2 + 4 + sizeof(payload)), written);
    TEST_ASSERT_EQUAL_HEX8(0x82U, frame_buffer[0]); // FIN + binary
    TEST_ASSERT_EQUAL_HEX8(0xFEU, frame_buffer[1]); // MASK + 126
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(sizeof(payload) >> 8U), frame_buffer[2]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(sizeof(payload) & 0xFFU), frame_buffer[3]);
    // A zero mask leaves the payload readable, which isolates the length coding.
    TEST_ASSERT_EQUAL_HEX8(0xA5U, frame_buffer[8]);
}

void test_stt_ws_frame_encode_switches_to_the_64_bit_length_above_65535(void)
{
    static uint8_t payload[65536];
    static uint8_t big_buffer[65536 + STT_WS_FRAME_HEADER_MAX];
    int const written = stt_ws_frame_encode(big_buffer, sizeof(big_buffer), STT_WS_OPCODE_BINARY,
                                            payload, sizeof(payload), 0U);

    TEST_ASSERT_EQUAL_INT((int)(2 + 8 + 4 + sizeof(payload)), written);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, big_buffer[1]); // MASK + 127
    TEST_ASSERT_EQUAL_HEX8(0x00U, big_buffer[2]);
    TEST_ASSERT_EQUAL_HEX8(0x01U, big_buffer[7]); // 0x10000 >> 16
    TEST_ASSERT_EQUAL_HEX8(0x00U, big_buffer[9]);
}

void test_stt_ws_frame_encode_rejects_bad_arguments_and_oversized_control_frames(void)
{
    uint8_t oversized_control[126];

    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_ws_frame_encode(NULL, 16U, STT_WS_OPCODE_TEXT, "x", 1U, 0U));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_ws_frame_encode(frame_buffer, sizeof(frame_buffer),
                                              STT_WS_OPCODE_TEXT, NULL, 1U, 0U));
    // Control frames cap at 125 payload bytes (RFC 6455 §5.5).
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_ws_frame_encode(frame_buffer, sizeof(frame_buffer),
                                              STT_WS_OPCODE_PONG, oversized_control,
                                              sizeof(oversized_control), 0U));
    TEST_ASSERT_EQUAL_INT(-ENOBUFS,
                          stt_ws_frame_encode(frame_buffer, 4U, STT_WS_OPCODE_TEXT, "hello", 5U,
                                              0U));
}

void test_stt_ws_frame_encode_supports_an_empty_payload(void)
{
    int const written =
        stt_ws_frame_encode(frame_buffer, sizeof(frame_buffer), STT_WS_OPCODE_PONG, NULL, 0U, 0U);

    TEST_ASSERT_EQUAL_INT(6, written);
    TEST_ASSERT_EQUAL_HEX8(0x8AU, frame_buffer[0]); // FIN + pong
    TEST_ASSERT_EQUAL_HEX8(0x80U, frame_buffer[1]); // MASK + length 0
}

// === Decoding ==================================================================================================== //

void test_stt_ws_frame_decode_reads_a_server_text_frame(void)
{
    char const payload[] = "{\"type\":\"pong\"}";
    size_t const size = build_server_frame(0x1U, 1U, (uint8_t const*)payload, strlen(payload));
    stt_ws_frame_t frame;

    TEST_ASSERT_EQUAL_INT(0, stt_ws_frame_decode(frame_buffer, size, 8192U, &frame));
    TEST_ASSERT_EQUAL_INT(STT_WS_OPCODE_TEXT, frame.opcode);
    TEST_ASSERT_EQUAL_UINT8(1U, frame.fin);
    TEST_ASSERT_EQUAL_UINT64(strlen(payload), frame.payload_len);
    TEST_ASSERT_EQUAL_size_t(2U, frame.header_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, frame.payload, strlen(payload));
}

void test_stt_ws_frame_decode_reads_16_bit_lengths(void)
{
    static uint8_t payload[300];
    size_t const size = build_server_frame(0x1U, 1U, payload, sizeof(payload));
    stt_ws_frame_t frame;

    memset(payload, 'z', sizeof(payload));
    TEST_ASSERT_EQUAL_INT(0, stt_ws_frame_decode(frame_buffer, size, 8192U, &frame));
    TEST_ASSERT_EQUAL_UINT64(sizeof(payload), frame.payload_len);
    TEST_ASSERT_EQUAL_size_t(4U, frame.header_len);
}

void test_stt_ws_frame_decode_waits_for_header_and_payload_bytes(void)
{
    char const payload[] = "partial";
    size_t const size = build_server_frame(0x1U, 1U, (uint8_t const*)payload, strlen(payload));
    stt_ws_frame_t frame;

    TEST_ASSERT_EQUAL_INT(-EAGAIN, stt_ws_frame_decode(frame_buffer, 0U, 8192U, &frame));
    TEST_ASSERT_EQUAL_INT(-EAGAIN, stt_ws_frame_decode(frame_buffer, 1U, 8192U, &frame));
    TEST_ASSERT_EQUAL_INT(-EAGAIN, stt_ws_frame_decode(frame_buffer, size - 1U, 8192U, &frame));
    TEST_ASSERT_EQUAL_INT(0, stt_ws_frame_decode(frame_buffer, size, 8192U, &frame));
}

void test_stt_ws_frame_decode_rejects_masked_server_frames(void)
{
    stt_ws_frame_t frame;

    // RFC 6455 §5.1: the server must never mask. Accepting it would let a
    // proxy-injected frame through with a payload we would misread.
    (void)build_server_frame(0x1U, 1U, (uint8_t const*)"x", 1U);
    frame_buffer[1] |= 0x80U;

    TEST_ASSERT_EQUAL_INT(-EPROTO, stt_ws_frame_decode(frame_buffer, 8U, 8192U, &frame));
}

void test_stt_ws_frame_decode_rejects_reserved_bits_and_fragmented_control_frames(void)
{
    stt_ws_frame_t frame;

    (void)build_server_frame(0x1U, 1U, (uint8_t const*)"x", 1U);
    frame_buffer[0] |= 0x40U; // RSV1: an extension we never negotiated
    TEST_ASSERT_EQUAL_INT(-EPROTO, stt_ws_frame_decode(frame_buffer, 3U, 8192U, &frame));

    // A control frame with FIN clear is illegal.
    (void)build_server_frame(0x9U, 0U, (uint8_t const*)"x", 1U);
    TEST_ASSERT_EQUAL_INT(-EPROTO, stt_ws_frame_decode(frame_buffer, 3U, 8192U, &frame));
}

void test_stt_ws_frame_decode_rejects_control_frames_longer_than_125_bytes(void)
{
    static uint8_t payload[126];
    size_t const size = build_server_frame(0x8U, 1U, payload, sizeof(payload));
    stt_ws_frame_t frame;

    TEST_ASSERT_EQUAL_INT(-EPROTO, stt_ws_frame_decode(frame_buffer, size, 8192U, &frame));
}

void test_stt_ws_frame_decode_enforces_the_caller_payload_limit(void)
{
    static uint8_t payload[300];
    size_t const size = build_server_frame(0x1U, 1U, payload, sizeof(payload));
    stt_ws_frame_t frame;

    // Bounding this is what keeps a hostile server from steering the client
    // into an unbounded read.
    TEST_ASSERT_EQUAL_INT(-EMSGSIZE, stt_ws_frame_decode(frame_buffer, size, 128U, &frame));
    TEST_ASSERT_EQUAL_INT(0, stt_ws_frame_decode(frame_buffer, size, 300U, &frame));
}

void test_stt_ws_frame_decode_reports_continuation_frames_without_assembling_them(void)
{
    size_t const size = build_server_frame(0x0U, 0U, (uint8_t const*)"part", 4U);
    stt_ws_frame_t frame;

    TEST_ASSERT_EQUAL_INT(0, stt_ws_frame_decode(frame_buffer, size, 8192U, &frame));
    TEST_ASSERT_EQUAL_INT(STT_WS_OPCODE_CONTINUATION, frame.opcode);
    TEST_ASSERT_EQUAL_UINT8(0U, frame.fin);
}

void test_stt_ws_frame_decode_rejects_null_arguments(void)
{
    stt_ws_frame_t frame;

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_frame_decode(NULL, 4U, 8192U, &frame));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_frame_decode(frame_buffer, 4U, 8192U, NULL));
}
