#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "app.h"
#include "errorno.h"
#include "stt_event_rx.h"

TEST_SOURCE_FILE("number_parse.c")

static subtitle_text_evt_t event;
static stt_event_rx_t live_rx;

static uint16_t reserve_loopback_port(void)
{
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);
    int const fd = socket(AF_INET, SOCK_STREAM, 0);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    TEST_ASSERT_EQUAL_INT(0, bind(fd, (struct sockaddr*)&address, sizeof(address)));
    TEST_ASSERT_EQUAL_INT(0, getsockname(fd, (struct sockaddr*)&address, &address_length));
    TEST_ASSERT_EQUAL_INT(0, close(fd));
    return ntohs(address.sin_port);
}

static void init_live_receiver(void)
{
    stt_event_rx_config_t config;

    memset(&config, 0, sizeof(config));
    snprintf(config.host, sizeof(config.host), "%s", "127.0.0.1");
    config.port = reserve_loopback_port();
    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_init(&live_rx, &config));
}

static int connect_live_client(void)
{
    struct sockaddr_in address;
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    int const fd = socket(AF_INET, SOCK_STREAM, 0);

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)live_rx.config.port);
    TEST_ASSERT_EQUAL_INT(0, connect(fd, (struct sockaddr*)&address, sizeof(address)));
    TEST_ASSERT_EQUAL_INT(
        0, setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    return fd;
}

static void poll_live_receiver(uint32_t* const event_count)
{
    subtitle_text_evt_t events[STT_EVENT_RX_MAX_EVENTS_PER_POLL];

    TEST_ASSERT_EQUAL_INT(
        0, stt_event_rx_poll(&live_rx, events, STT_EVENT_RX_MAX_EVENTS_PER_POLL, event_count));
    if (*event_count > 0U)
    {
        event = events[0];
    }
}

static void receive_line(int fd, char* const line, size_t capacity)
{
    size_t used = 0U;

    while ((used + 1U) < capacity)
    {
        ssize_t const received = recv(fd, &line[used], 1U, 0);
        TEST_ASSERT_EQUAL_INT(1, received);
        if (line[used++] == '\n')
        {
            line[used] = '\0';
            return;
        }
    }
    TEST_FAIL_MESSAGE("response line exceeded test buffer");
}

static void send_transcript(int fd, uint32_t seq)
{
    char line[160];
    int const length = snprintf(line,
                                sizeof(line),
                                "{\"seq\":%lu,\"is_final\":false,\"start_sec\":0,"
                                "\"end_sec\":1,\"text\":\"hola\"}\n",
                                (unsigned long)seq);

    TEST_ASSERT_GREATER_THAN_INT(0, length);
    TEST_ASSERT_EQUAL_INT(length, send(fd, line, (size_t)length, 0));
}

void setUp(void)
{
    memset(&event, 0, sizeof(event));
    memset(&live_rx, 0, sizeof(live_rx));
    unsetenv("SUBTITLE_STT_RX_HOST");
    unsetenv("SUBTITLE_STT_RX_PORT");
}

void tearDown(void)
{
    stt_event_rx_cleanup(&live_rx);
    unsetenv("SUBTITLE_STT_RX_HOST");
    unsetenv("SUBTITLE_STT_RX_PORT");
}

void test_stt_event_rx_allows_seq_zero_in_two_consecutive_sessions(void)
{
    char response[STT_EVENT_RX_RESPONSE_MAX];
    uint32_t count;
    int client;

    init_live_receiver();
    client = connect_live_client();
    poll_live_receiver(&count);
    TEST_ASSERT_EQUAL_UINT32(0U, count);
    receive_line(client, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"type\":\"session_ready\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"session_id\":1"));
    send_transcript(client, 0U);
    poll_live_receiver(&count);
    TEST_ASSERT_EQUAL_UINT32(1U, count);
    TEST_ASSERT_EQUAL_UINT32(0U, event.seq);
    TEST_ASSERT_EQUAL_INT(
        0, stt_event_rx_report_delivery(&live_rx, 0U, STT_EVENT_RX_DELIVERY_ACCEPTED));
    poll_live_receiver(&count);
    receive_line(client, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"status\":\"accepted\""));
    close(client);
    poll_live_receiver(&count);

    client = connect_live_client();
    poll_live_receiver(&count);
    receive_line(client, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"session_id\":2"));
    send_transcript(client, 0U);
    poll_live_receiver(&count);
    TEST_ASSERT_EQUAL_UINT32(1U, count);
    TEST_ASSERT_EQUAL_UINT32(0U, event.seq);
    close(client);
}

void test_stt_event_rx_rejects_duplicate_sequence_within_session(void)
{
    char response[STT_EVENT_RX_RESPONSE_MAX];
    uint32_t count;
    int client;

    init_live_receiver();
    client = connect_live_client();
    poll_live_receiver(&count);
    receive_line(client, response, sizeof(response));
    send_transcript(client, 7U);
    poll_live_receiver(&count);
    TEST_ASSERT_EQUAL_UINT32(1U, count);
    TEST_ASSERT_EQUAL_INT(
        0, stt_event_rx_report_delivery(&live_rx, 7U, STT_EVENT_RX_DELIVERY_ACCEPTED));
    poll_live_receiver(&count);
    receive_line(client, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"status\":\"accepted\""));

    send_transcript(client, 7U);
    poll_live_receiver(&count);
    TEST_ASSERT_EQUAL_UINT32(0U, count);
    receive_line(client, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"status\":\"rejected_old_seq\""));
    close(client);
}

void test_stt_event_rx_reports_each_delivery_status(void)
{
    static stt_event_rx_delivery_status_t const statuses[] = {
        STT_EVENT_RX_DELIVERY_ACCEPTED,
        STT_EVENT_RX_DELIVERY_DROPPED_EVENT_POOL,
        STT_EVENT_RX_DELIVERY_DROPPED_SUBTITLE_QUEUE,
    };
    static char const* const expected[] = {
        "accepted",
        "dropped_event_pool",
        "dropped_subtitle_queue",
    };
    char response[STT_EVENT_RX_RESPONSE_MAX];
    uint32_t count;
    uint32_t i;
    int client;

    init_live_receiver();
    client = connect_live_client();
    poll_live_receiver(&count);
    receive_line(client, response, sizeof(response));
    for (i = 0U; i < (uint32_t)(sizeof(statuses) / sizeof(statuses[0])); i++)
    {
        TEST_ASSERT_EQUAL_INT(0, stt_event_rx_report_delivery(&live_rx, i, statuses[i]));
        poll_live_receiver(&count);
        receive_line(client, response, sizeof(response));
        TEST_ASSERT_NOT_NULL(strstr(response, expected[i]));
    }
    close(client);
}

void test_stt_event_rx_response_queue_full_is_nonblocking(void)
{
    char response[STT_EVENT_RX_RESPONSE_MAX];
    uint32_t count;
    uint32_t i;
    int client;

    init_live_receiver();
    client = connect_live_client();
    poll_live_receiver(&count);
    receive_line(client, response, sizeof(response));
    for (i = 0U; i < STT_EVENT_RX_TX_QUEUE_DEPTH; i++)
    {
        TEST_ASSERT_EQUAL_INT(
            0, stt_event_rx_report_delivery(&live_rx, i, STT_EVENT_RX_DELIVERY_ACCEPTED));
    }
    TEST_ASSERT_EQUAL_INT(
        -EAGAIN,
        stt_event_rx_report_delivery(
            &live_rx, STT_EVENT_RX_TX_QUEUE_DEPTH, STT_EVENT_RX_DELIVERY_ACCEPTED));
    poll_live_receiver(&count);
    TEST_ASSERT_EQUAL_UINT8(1U, live_rx.client_connected);
    close(client);
}

void test_stt_event_rx_resumes_partially_sent_response(void)
{
    int sockets[2];
    char received[32];
    uint32_t count;

    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    TEST_ASSERT_NOT_EQUAL(-1, fcntl(sockets[0], F_SETFL, O_NONBLOCK));
    memset(&live_rx, 0, sizeof(live_rx));
    live_rx.initialized = 1U;
    live_rx.client_connected = 1U;
    live_rx.server_fd = -1;
    live_rx.client_fd = sockets[0];
    snprintf(live_rx.responses[0].data, sizeof(live_rx.responses[0].data), "%s", "ABCDEFGHIJ\n");
    live_rx.responses[0].length = 11U;
    live_rx.responses[0].sent = 4U;
    live_rx.response_count = 1U;

    poll_live_receiver(&count);
    TEST_ASSERT_EQUAL_INT(7, recv(sockets[1], received, sizeof(received) - 1U, 0));
    received[7] = '\0';
    TEST_ASSERT_EQUAL_STRING("EFGHIJ\n", received);
    TEST_ASSERT_EQUAL_UINT8(0U, live_rx.response_count);
    close(sockets[1]);
}

void test_stt_event_rx_send_disconnect_does_not_fail_poll(void)
{
    int sockets[2];
    uint32_t count;

    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    memset(&live_rx, 0, sizeof(live_rx));
    live_rx.initialized = 1U;
    live_rx.client_connected = 1U;
    live_rx.server_fd = -1;
    live_rx.client_fd = sockets[0];
    live_rx.session_id = 1U;
    close(sockets[1]);
    TEST_ASSERT_EQUAL_INT(
        0, stt_event_rx_report_delivery(&live_rx, 1U, STT_EVENT_RX_DELIVERY_ACCEPTED));

    poll_live_receiver(&count);

    TEST_ASSERT_EQUAL_INT(-1, live_rx.client_fd);
    TEST_ASSERT_EQUAL_UINT8(0U, live_rx.client_connected);
}

void test_stt_event_rx_socket_eagain_preserves_pending_responses(void)
{
    int sockets[2];
    uint32_t count;
    uint32_t cycle;
    uint32_t seq = 0U;
    uint8_t saw_backpressure = 0U;

    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    TEST_ASSERT_NOT_EQUAL(-1, fcntl(sockets[0], F_SETFL, O_NONBLOCK));
    memset(&live_rx, 0, sizeof(live_rx));
    live_rx.initialized = 1U;
    live_rx.client_connected = 1U;
    live_rx.server_fd = -1;
    live_rx.client_fd = sockets[0];
    live_rx.session_id = 1U;

    for (cycle = 0U; (cycle < 1000U) && (saw_backpressure == 0U); cycle++)
    {
        while (live_rx.response_count < STT_EVENT_RX_TX_QUEUE_DEPTH)
        {
            TEST_ASSERT_EQUAL_INT(
                0,
                stt_event_rx_report_delivery(
                    &live_rx, seq++, STT_EVENT_RX_DELIVERY_ACCEPTED));
        }
        poll_live_receiver(&count);
        if (live_rx.response_count > 0U)
        {
            saw_backpressure = 1U;
        }
    }

    TEST_ASSERT_EQUAL_UINT8(1U, saw_backpressure);
    TEST_ASSERT_EQUAL_UINT8(1U, live_rx.client_connected);
    close(sockets[1]);
}

void test_stt_event_rx_default_config_accepts_valid_and_ignores_invalid_port_overrides(void)
{
    stt_event_rx_config_t config;

    stt_event_rx_default_config(NULL);
    setenv("SUBTITLE_STT_RX_HOST", "127.0.0.1", 1);
    setenv("SUBTITLE_STT_RX_PORT", "6001", 1);
    stt_event_rx_default_config(&config);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", config.host);
    TEST_ASSERT_EQUAL_UINT32(6001U, config.port);

    setenv("SUBTITLE_STT_RX_PORT", "70000", 1);
    stt_event_rx_default_config(&config);
    TEST_ASSERT_EQUAL_UINT32(STT_EVENT_RX_DEFAULT_PORT, config.port);

    setenv("SUBTITLE_STT_RX_PORT", "5001junk", 1);
    stt_event_rx_default_config(&config);
    TEST_ASSERT_EQUAL_UINT32(STT_EVENT_RX_DEFAULT_PORT, config.port);
}

void test_stt_event_rx_init_rejects_port_outside_tcp_range(void)
{
    stt_event_rx_t rx;
    stt_event_rx_config_t config;

    memset(&rx, 0, sizeof(rx));
    memset(&config, 0, sizeof(config));
    snprintf(config.host, sizeof(config.host), "%s", "127.0.0.1");
    config.port = 65536U;

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_event_rx_init(&rx, &config));
}

void test_stt_event_rx_parse_line_accepts_valid_partial_with_boolean_final_flag(void)
{
    char const* const line =
        "{\"seq\":7,\"is_final\":false,\"start_sec\":1.25,\"end_sec\":1.75,\"text\":\"hola\"}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT32(7U, event.seq);
    TEST_ASSERT_EQUAL_UINT8(0U, event.is_final);
    TEST_ASSERT_EQUAL_UINT32(1250U, event.start_ms);
    TEST_ASSERT_EQUAL_UINT32(1750U, event.end_ms);
    TEST_ASSERT_EQUAL_STRING("hola", event.text);
}

void test_stt_event_rx_parse_line_accepts_valid_final_with_type_field(void)
{
    char const* const line =
        "{\"seq\":8,\"type\":\"final\",\"start_sec\":2.0,\"end_sec\":2.5,\"text\":\"listo\"}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT8(1U, event.is_final);
    TEST_ASSERT_EQUAL_STRING("listo", event.text);
}

void test_stt_event_rx_parse_line_accepts_partial_type_field(void)
{
    char const* const line =
        "{\"seq\":9,\"type\":\"partial\",\"start_sec\":0.0,\"end_sec\":0.1,\"text\":\"va\"}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT8(0U, event.is_final);
}

void test_stt_event_rx_parse_line_truncates_long_text_but_keeps_event(void)
{
    char line[512];
    char long_text[256];

    memset(long_text, 'a', sizeof(long_text) - 1U);
    long_text[sizeof(long_text) - 1U] = '\0';
    snprintf(line,
             sizeof(line),
             "{\"seq\":10,\"is_final\":true,\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"%s\"}",
             long_text);

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT8(1U, event.is_final);
    TEST_ASSERT_EQUAL_UINT(SUBTITLE_TEXT_MAX_LEN - 1U, strlen(event.text));
    TEST_ASSERT_EQUAL_CHAR('a', event.text[0]);
    TEST_ASSERT_EQUAL_CHAR('\0', event.text[SUBTITLE_TEXT_MAX_LEN - 1U]);
}

void test_stt_event_rx_parse_line_rejects_empty_text(void)
{
    char const* const line =
        "{\"seq\":11,\"is_final\":false,\"start_sec\":0.0,\"end_sec\":0.1,\"text\":\"\"}";

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_event_rx_parse_line(line, &event));
}

void test_stt_event_rx_parse_line_rejects_end_before_start(void)
{
    char const* const line =
        "{\"seq\":12,\"is_final\":false,\"start_sec\":2.0,\"end_sec\":1.0,\"text\":\"x\"}";

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_event_rx_parse_line(line, &event));
}

void test_stt_event_rx_parse_line_rejects_missing_required_fields(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_event_rx_parse_line("{\"is_final\":false,\"start_sec\":0.0,"
                                                  "\"end_sec\":1.0,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                                  "\"end_sec\":1.0,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                                  "\"start_sec\":0.0,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                                  "\"start_sec\":0.0,\"end_sec\":1.0}",
                                                  &event));
}

void test_stt_event_rx_parse_line_handles_simple_escapes(void)
{
    char const* const line = "{\"seq\":13,\"is_final\":true,\"start_sec\":0.0,\"end_sec\":1.0,"
                             "\"text\":\"hola\\n\\\"mundo\\\"\\\\\"}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_STRING("hola \"mundo\"\\", event.text);
}

void test_stt_event_rx_parse_line_accepts_sender_fields_in_any_order(void)
{
    char const* const line = "{\"text\":\"hola\",\"dropped\":0,\"end_sec\":1.25,"
                             "\"type\":\"final\",\"seq\":21,\"chunk_start\":1,"
                             "\"start_sec\":1.0,\"is_final\":true}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT32(21U, event.seq);
    TEST_ASSERT_EQUAL_UINT8(1U, event.is_final);
    TEST_ASSERT_EQUAL_STRING("hola", event.text);
}

void test_stt_event_rx_parse_line_does_not_match_keys_inside_text(void)
{
    char const* const line = "{\"text\":\"say \\\"seq\\\":999 now\",\"seq\":22,"
                             "\"is_final\":false,\"start_sec\":0.0,\"end_sec\":0.2}";

    TEST_ASSERT_EQUAL_INT(0, stt_event_rx_parse_line(line, &event));
    TEST_ASSERT_EQUAL_UINT32(22U, event.seq);
    TEST_ASSERT_EQUAL_STRING("say \"seq\":999 now", event.text);
}

void test_stt_event_rx_parse_line_rejects_duplicates_and_inconsistent_final_fields(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_event_rx_parse_line("{\"seq\":1,\"seq\":2,\"is_final\":false,"
                                                  "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,\"type\":\"final\","
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}",
                                &event));
}

void test_stt_event_rx_parse_line_rejects_overflow_and_malformed_numeric_tokens(void)
{
    TEST_ASSERT_EQUAL_INT(-ERANGE,
                          stt_event_rx_parse_line("{\"seq\":4294967296,\"is_final\":false,"
                                                  "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_event_rx_parse_line("{\"seq\":1junk,\"is_final\":false,"
                                                  "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}",
                                                  &event));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":4294968,\"text\":\"x\"}",
                                &event));
}

void test_stt_event_rx_parse_line_rejects_malformed_escapes_and_trailing_garbage(void)
{
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"bad\\q\"}",
                                &event));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\"}junk",
                                &event));
}

void test_stt_event_rx_parse_line_rejects_nonfinite_time_and_trailing_comma(void)
{
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                "\"start_sec\":0,\"end_sec\":1e999,\"text\":\"x\"}",
                                &event));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_event_rx_parse_line("{\"seq\":1,\"is_final\":false,"
                                                  "\"start_sec\":0,\"end_sec\":1,\"text\":\"x\",}",
                                                  &event));
}
