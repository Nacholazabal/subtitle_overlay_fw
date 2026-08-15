#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "stt_ws_client.h"
#include "stt_ws_frame.h"

#include "fake_net_tls.h"

TEST_SOURCE_FILE("fake_net_tls.c")
TEST_SOURCE_FILE("number_parse.c")
TEST_SOURCE_FILE("stt_json.c")
TEST_SOURCE_FILE("stt_session_json.c")
TEST_SOURCE_FILE("stt_ws_frame.c")
TEST_SOURCE_FILE("stt_transcript_parse.c")

static stt_ws_client_t client;
static stt_ws_client_config_t config;
static subtitle_text_evt_t events[STT_WS_EVENT_RING_DEPTH];

static char const* const WS_ENV[] = {
    "SUBTITLE_STT_WS_URL",
    "SUBTITLE_STT_WS_CA_FILE",
    "SUBTITLE_STT_WS_CA_DIR",
    "SUBTITLE_STT_WS_CONNECT_TIMEOUT_MS",
    "SUBTITLE_STT_WS_HANDSHAKE_TIMEOUT_MS",
    "SUBTITLE_STT_WS_IDLE_TIMEOUT_MS",
    "SUBTITLE_STT_WS_PING_MS",
    "SUBTITLE_STT_WS_BACKOFF_MIN_MS",
    "SUBTITLE_STT_WS_BACKOFF_MAX_MS",
    "SUBTITLE_STT_MIN_EPOCH",
    "SUBTITLE_STT_NEMOTRON_LATENCY_MS",
    "SUBTITLE_STT_NEMOTRON_STOP_HISTORY_EOU_MS",
    "SUBTITLE_STT_NEMOTRON_RESIDUE_TOKENS_AT_END",
    "SUBTITLE_STT_NEMOTRON_TARGET_LANG",
};

static void clear_env(void)
{
    size_t i;

    for (i = 0U; i < (sizeof(WS_ENV) / sizeof(WS_ENV[0])); i++)
    {
        unsetenv(WS_ENV[i]);
    }
}

/// @brief A client that believes it holds a live session, without any I/O.
static void init_ready_client(void)
{
    memset(&config, 0, sizeof(config));
    snprintf(config.host, sizeof(config.host), "%s", "example.test");
    snprintf(config.path, sizeof(config.path), "%s", "/stt/stream");
    config.port = 443U;
    config.use_tls = 1U;
    config.backoff_min_ms = 500U;
    config.backoff_max_ms = 30000U;
    config.connect_timeout_ms = 1000U;
    config.handshake_timeout_ms = 1000U;
    config.idle_timeout_ms = 0U;    // disabled: these tests do not advance time
    config.ping_interval_ms = 0U;   // disabled: keeps the transmit stream clean
    config.session.sample_rate_hz = 48000U;
    config.session.channels = 1U;
    config.session.format = STT_SESSION_FORMAT_S16_LE;
    config.session.bytes_per_chunk = 1920U;

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_init(&client, &config));
}

/// @brief Push a transcript line straight into the ring the QP/C thread drains.
static void queue_transcript(uint32_t seq, char const* text, uint8_t is_final)
{
    char line[STT_WS_LINE_MAX];
    int const length = snprintf(line, sizeof(line),
                                "{\"type\":\"transcript\",\"seq\":%lu,\"is_final\":%s,"
                                "\"start_sec\":0.0,\"end_sec\":1.0,\"text\":\"%s\","
                                "\"att_context_size\":[56,6]}",
                                (unsigned long)seq, (is_final != 0U) ? "true" : "false", text);
    uint32_t tail;

    TEST_ASSERT_GREATER_THAN_INT(0, length);
    tail = ((uint32_t)client.ring_head + (uint32_t)client.ring_count) % STT_WS_EVENT_RING_DEPTH;
    memcpy(client.ring[tail].line, line, (size_t)length + 1U);
    client.ring[tail].length = (uint16_t)length;
    client.ring[tail].is_final = is_final;
    client.ring[tail].session_generation = client.session_generation;
    client.ring_count++;
}

void setUp(void)
{
    clear_env();
    fake_net_tls_reset();
    memset(&client, 0, sizeof(client));
    memset(&config, 0, sizeof(config));
    memset(events, 0, sizeof(events));
}

void tearDown(void)
{
    if (client.initialized != 0U)
    {
        stt_ws_client_cleanup(&client);
    }
    clear_env();
}

// === URL parsing ================================================================================================= //

void test_stt_ws_client_parses_the_production_wss_url(void)
{
    TEST_ASSERT_EQUAL_INT(0,
                          stt_ws_client_parse_url(
                              "wss://passage-capacity-wistful.ngrok-free.dev/stt/stream", &config));
    TEST_ASSERT_EQUAL_STRING("passage-capacity-wistful.ngrok-free.dev", config.host);
    TEST_ASSERT_EQUAL_UINT16(443U, config.port);
    TEST_ASSERT_EQUAL_STRING("/stt/stream", config.path);
    TEST_ASSERT_EQUAL_UINT8(1U, config.use_tls);
}

void test_stt_ws_client_parses_plaintext_and_explicit_ports(void)
{
    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_parse_url("ws://192.168.1.20:8765/stt/stream", &config));
    TEST_ASSERT_EQUAL_STRING("192.168.1.20", config.host);
    TEST_ASSERT_EQUAL_UINT16(8765U, config.port);
    TEST_ASSERT_EQUAL_UINT8(0U, config.use_tls);

    // No path means the root target.
    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_parse_url("wss://host.test", &config));
    TEST_ASSERT_EQUAL_STRING("/", config.path);
    TEST_ASSERT_EQUAL_UINT16(443U, config.port);
}

void test_stt_ws_client_rejects_urls_it_cannot_honour(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_parse_url("https://host/stt", &config));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_parse_url("wss://", &config));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_parse_url("wss://host:0/x", &config));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_parse_url("wss://host:70000/x", &config));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_parse_url("host/stt", &config));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_parse_url(NULL, &config));
}

// === Configuration =============================================================================================== //

void test_stt_ws_client_refuses_to_start_without_a_configured_url(void)
{
    // No compiled-in endpoint: an unconfigured board must stay down and say so.
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_default_config(&config));
    TEST_ASSERT_EQUAL_STRING("", config.host);
}

void test_stt_ws_client_defaults_match_the_chosen_operating_point(void)
{
    setenv("SUBTITLE_STT_WS_URL", "wss://host.test/stt/stream", 1);

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_default_config(&config));
    TEST_ASSERT_EQUAL_UINT32(48000U, config.session.sample_rate_hz);
    TEST_ASSERT_EQUAL_UINT32(1U, config.session.channels);
    TEST_ASSERT_EQUAL_UINT32(20U, config.session.chunk_ms);
    TEST_ASSERT_EQUAL_UINT32(960U, config.session.samples_per_chunk);
    TEST_ASSERT_EQUAL_UINT32(1920U, config.session.bytes_per_chunk);
    // The configuration selected by the thesis.
    TEST_ASSERT_EQUAL_UINT32(560U, config.session.latency_ms);
    TEST_ASSERT_EQUAL_UINT32(600U, config.session.stop_history_eou_ms);
    TEST_ASSERT_EQUAL_UINT32(2U, config.session.residue_tokens_at_end);
    TEST_ASSERT_EQUAL_STRING("es-ES", config.session.target_lang);
    TEST_ASSERT_EQUAL_STRING("/etc/ssl/certs/ca-certificates.crt", config.ca_file);
}

void test_stt_ws_client_applies_environment_overrides(void)
{
    setenv("SUBTITLE_STT_WS_URL", "ws://10.0.0.5:9000/x", 1);
    setenv("SUBTITLE_STT_WS_CA_FILE", "/tmp/ca.pem", 1);
    setenv("SUBTITLE_STT_WS_IDLE_TIMEOUT_MS", "12000", 1);
    setenv("SUBTITLE_STT_NEMOTRON_LATENCY_MS", "320", 1);
    setenv("SUBTITLE_STT_NEMOTRON_TARGET_LANG", "en-US", 1);

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_default_config(&config));
    TEST_ASSERT_EQUAL_UINT16(9000U, config.port);
    TEST_ASSERT_EQUAL_STRING("/tmp/ca.pem", config.ca_file);
    TEST_ASSERT_EQUAL_UINT32(12000U, config.idle_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(320U, config.session.latency_ms);
    TEST_ASSERT_EQUAL_STRING("en-US", config.session.target_lang);
}

void test_stt_ws_client_keeps_defaults_when_an_override_is_invalid(void)
{
    setenv("SUBTITLE_STT_WS_URL", "wss://host.test/stt/stream", 1);
    setenv("SUBTITLE_STT_WS_IDLE_TIMEOUT_MS", "not-a-number", 1);
    setenv("SUBTITLE_STT_NEMOTRON_LATENCY_MS", "999999", 1);

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_default_config(&config));
    TEST_ASSERT_EQUAL_UINT32(45000U, config.idle_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(560U, config.session.latency_ms);
}

void test_stt_ws_client_keeps_the_backoff_window_ordered(void)
{
    setenv("SUBTITLE_STT_WS_URL", "wss://host.test/stt/stream", 1);
    setenv("SUBTITLE_STT_WS_BACKOFF_MIN_MS", "20000", 1);
    setenv("SUBTITLE_STT_WS_BACKOFF_MAX_MS", "1000", 1);

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_default_config(&config));
    TEST_ASSERT_TRUE(config.backoff_max_ms >= config.backoff_min_ms);
}

// === Transcript delivery ========================================================================================= //

void test_stt_ws_client_delivers_buffered_transcripts_in_order(void)
{
    uint32_t count = 0U;

    init_ready_client();
    queue_transcript(0U, "hola", 0U);
    queue_transcript(1U, "hola mundo", 1U);

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_poll_events(&client, events, STT_WS_EVENT_RING_DEPTH,
                                                       &count));
    TEST_ASSERT_EQUAL_UINT32(2U, count);
    TEST_ASSERT_EQUAL_UINT32(0U, events[0].seq);
    TEST_ASSERT_EQUAL_UINT8(0U, events[0].is_final);
    TEST_ASSERT_EQUAL_STRING("hola", events[0].text);
    TEST_ASSERT_EQUAL_UINT32(1U, events[1].seq);
    TEST_ASSERT_EQUAL_UINT8(1U, events[1].is_final);
    TEST_ASSERT_EQUAL_STRING("hola mundo", events[1].text);
}

void test_stt_ws_client_rejects_duplicate_and_out_of_order_sequences(void)
{
    stt_ws_client_stats_t stats;
    uint32_t count = 0U;

    init_ready_client();
    queue_transcript(5U, "cinco", 1U);
    queue_transcript(5U, "cinco otra vez", 1U);
    queue_transcript(3U, "tarde", 1U);
    queue_transcript(6U, "seis", 1U);

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_poll_events(&client, events, STT_WS_EVENT_RING_DEPTH,
                                                       &count));
    TEST_ASSERT_EQUAL_UINT32(2U, count);
    TEST_ASSERT_EQUAL_UINT32(5U, events[0].seq);
    TEST_ASSERT_EQUAL_UINT32(6U, events[1].seq);

    stt_ws_client_stats(&client, &stats);
    TEST_ASSERT_EQUAL_UINT32(2U, stats.events_rejected_old_seq);
}

void test_stt_ws_client_accepts_seq_zero_again_after_a_reconnect(void)
{
    uint32_t count = 0U;

    init_ready_client();
    queue_transcript(0U, "sesion uno", 1U);
    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_poll_events(&client, events, STT_WS_EVENT_RING_DEPTH,
                                                       &count));
    TEST_ASSERT_EQUAL_UINT32(1U, count);

    // The server restarts its counter at 0 for every session, so a reconnect
    // must clear the guard or the whole next session would be discarded.
    client.session_generation++;

    queue_transcript(0U, "sesion dos", 1U);
    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_poll_events(&client, events, STT_WS_EVENT_RING_DEPTH,
                                                       &count));
    TEST_ASSERT_EQUAL_UINT32(1U, count);
    TEST_ASSERT_EQUAL_STRING("sesion dos", events[0].text);
}

void test_stt_ws_client_discards_an_event_copied_from_a_dead_session(void)
{
    stt_ws_client_stats_t stats;
    uint32_t count = 0U;

    init_ready_client();
    queue_transcript(7U, "sesion vieja", 1U);
    client.session_generation++;

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_poll_events(&client, events, STT_WS_EVENT_RING_DEPTH,
                                                       &count));
    TEST_ASSERT_EQUAL_UINT32(0U, count);
    stt_ws_client_stats(&client, &stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.events_dropped_ring);
}

void test_stt_ws_client_counts_unparsable_lines_instead_of_forwarding_them(void)
{
    stt_ws_client_stats_t stats;
    uint32_t count = 0U;

    init_ready_client();
    snprintf(client.ring[0].line, sizeof(client.ring[0].line), "%s", "{\"type\":\"transcript\"");
    client.ring[0].length = (uint16_t)strlen(client.ring[0].line);
    client.ring_count = 1U;

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_poll_events(&client, events, STT_WS_EVENT_RING_DEPTH,
                                                       &count));
    TEST_ASSERT_EQUAL_UINT32(0U, count);
    stt_ws_client_stats(&client, &stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.protocol_errors);
}

void test_stt_ws_client_honours_the_caller_event_budget(void)
{
    uint32_t count = 0U;

    init_ready_client();
    queue_transcript(0U, "a", 0U);
    queue_transcript(1U, "b", 0U);
    queue_transcript(2U, "c", 0U);

    // SttAO passes a fixed-size stack array; the client must not overrun it.
    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_poll_events(&client, events, 2U, &count));
    TEST_ASSERT_EQUAL_UINT32(2U, count);
    TEST_ASSERT_EQUAL_UINT32(1U, client.ring_count);

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_poll_events(&client, events, 2U, &count));
    TEST_ASSERT_EQUAL_UINT32(1U, count);
    TEST_ASSERT_EQUAL_UINT32(2U, events[0].seq);
}

void test_stt_ws_client_poll_rejects_invalid_arguments(void)
{
    uint32_t count = 0U;

    init_ready_client();
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_poll_events(NULL, events, 1U, &count));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_poll_events(&client, NULL, 1U, &count));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_poll_events(&client, events, 0U, &count));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_poll_events(&client, events, 1U, NULL));
}

// === Delivery accounting ========================================================================================= //

void test_stt_ws_client_keeps_delivery_outcomes_as_counters(void)
{
    stt_ws_client_stats_t stats;

    // With the PC bridge gone nothing is acknowledged over the wire, so the
    // three outcomes survive as local observability instead.
    init_ready_client();
    stt_ws_client_report_delivery(&client, STT_EVENT_RX_DELIVERY_ACCEPTED);
    stt_ws_client_report_delivery(&client, STT_EVENT_RX_DELIVERY_ACCEPTED);
    stt_ws_client_report_delivery(&client, STT_EVENT_RX_DELIVERY_DROPPED_EVENT_POOL);
    stt_ws_client_report_delivery(&client, STT_EVENT_RX_DELIVERY_DROPPED_SUBTITLE_QUEUE);

    stt_ws_client_stats(&client, &stats);
    TEST_ASSERT_EQUAL_UINT32(2U, stats.deliveries_accepted);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.deliveries_dropped_pool);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.deliveries_dropped_queue);
}

void test_stt_ws_client_ignores_reports_on_an_uninitialized_client(void)
{
    stt_ws_client_t fresh;

    memset(&fresh, 0, sizeof(fresh));
    stt_ws_client_report_delivery(&fresh, STT_EVENT_RX_DELIVERY_ACCEPTED);
    stt_ws_client_report_delivery(NULL, STT_EVENT_RX_DELIVERY_ACCEPTED);
    TEST_ASSERT_EQUAL_UINT32(0U, fresh.stats.deliveries_accepted);
}

// === State reporting ============================================================================================= //

void test_stt_ws_client_names_every_state(void)
{
    TEST_ASSERT_EQUAL_STRING("idle", stt_ws_client_state_name(STT_WS_STATE_IDLE));
    TEST_ASSERT_EQUAL_STRING("wait_clock", stt_ws_client_state_name(STT_WS_STATE_WAIT_CLOCK));
    TEST_ASSERT_EQUAL_STRING("connecting", stt_ws_client_state_name(STT_WS_STATE_CONNECTING));
    TEST_ASSERT_EQUAL_STRING("handshaking", stt_ws_client_state_name(STT_WS_STATE_HANDSHAKING));
    TEST_ASSERT_EQUAL_STRING("starting", stt_ws_client_state_name(STT_WS_STATE_STARTING));
    TEST_ASSERT_EQUAL_STRING("ready", stt_ws_client_state_name(STT_WS_STATE_READY));
    TEST_ASSERT_EQUAL_STRING("backoff", stt_ws_client_state_name(STT_WS_STATE_BACKOFF));
}

void test_stt_ws_client_init_validates_its_configuration(void)
{
    memset(&config, 0, sizeof(config));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_init(&client, &config));

    snprintf(config.host, sizeof(config.host), "%s", "host.test");
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_init(&client, &config)); // port still 0

    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_init(NULL, &config));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_init(&client, NULL));
}

// === Clock guard ================================================================================================= //

void test_stt_ws_client_defers_tls_until_the_clock_is_plausible(void)
{
    stt_ws_client_stats_t stats;

    init_ready_client();
    // The board has no RTC and boots at its rootfs build date. Without this
    // guard every retry would burn a connection on CERT_NOT_YET_VALID.
    client.config.min_epoch = 4102444800L; // 2100-01-01, always in the future
    client.state = STT_WS_STATE_IDLE;
    client.next_attempt_ms = 0U;

    TEST_ASSERT_EQUAL_INT(-EAGAIN, stt_ws_client_service(&client));
    TEST_ASSERT_EQUAL_INT(STT_WS_STATE_WAIT_CLOCK, stt_ws_client_state(&client));
    // Nothing was dialled: the guard runs before any transport call.
    TEST_ASSERT_EQUAL_UINT32(0U, fake_net_tls_open_count());

    stt_ws_client_stats(&client, &stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.clock_deferrals);
}

void test_stt_ws_client_skips_the_clock_guard_for_plaintext_endpoints(void)
{
    init_ready_client();
    client.config.use_tls = 0U;
    client.config.min_epoch = 4102444800L;
    client.state = STT_WS_STATE_IDLE;
    client.next_attempt_ms = 0U;

    // No certificate is involved over ws://, so a wrong clock is not a reason
    // to refuse to connect. Connecting is expected to fail here instead.
    fake_net_tls_fail_open(1U);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, stt_ws_client_service(&client));
    TEST_ASSERT_EQUAL_INT(STT_WS_STATE_BACKOFF, stt_ws_client_state(&client));
}

// === Backoff ===================================================================================================== //

void test_stt_ws_client_backs_off_and_grows_the_delay_after_failures(void)
{
    stt_ws_client_stats_t stats;
    uint64_t first_wait;
    uint64_t second_wait;

    init_ready_client();
    client.config.use_tls = 0U;
    client.state = STT_WS_STATE_IDLE;
    fake_net_tls_fail_open(1U);

    TEST_ASSERT_EQUAL_INT(-EAGAIN, stt_ws_client_service(&client));
    first_wait = client.backoff_ms;

    client.next_attempt_ms = 0U;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, stt_ws_client_service(&client));
    second_wait = client.backoff_ms;

    TEST_ASSERT_EQUAL_UINT64(config.backoff_min_ms, first_wait);
    TEST_ASSERT_TRUE(second_wait > first_wait);

    stt_ws_client_stats(&client, &stats);
    TEST_ASSERT_EQUAL_UINT32(2U, stats.connect_failures);
}

void test_stt_ws_client_waits_for_the_scheduled_retry_instant(void)
{
    init_ready_client();
    client.config.use_tls = 0U;
    client.state = STT_WS_STATE_IDLE;
    fake_net_tls_fail_open(1U);

    TEST_ASSERT_EQUAL_INT(-EAGAIN, stt_ws_client_service(&client));

    TEST_ASSERT_EQUAL_UINT32(1U, fake_net_tls_open_count());

    // Still inside the backoff window: no second attempt yet.
    TEST_ASSERT_EQUAL_INT(-EAGAIN, stt_ws_client_service(&client));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_net_tls_open_count());
}

void test_stt_ws_client_caps_the_backoff_at_the_configured_maximum(void)
{
    unsigned int i;

    init_ready_client();
    client.config.use_tls = 0U;
    client.config.backoff_max_ms = 4000U;
    client.state = STT_WS_STATE_IDLE;
    fake_net_tls_fail_open(1U);

    for (i = 0U; i < 12U; i++)
    {
        client.next_attempt_ms = 0U;
        (void)stt_ws_client_service(&client);
    }

    TEST_ASSERT_TRUE(client.backoff_ms <= client.config.backoff_max_ms);
}

// === Audio path ================================================================================================== //

void test_stt_ws_client_drops_audio_while_the_link_is_down(void)
{
    stt_ws_client_stats_t stats;
    uint8_t pcm[1920];

    init_ready_client();
    client.config.use_tls = 0U;
    client.state = STT_WS_STATE_IDLE;
    memset(pcm, 0, sizeof(pcm));
    fake_net_tls_fail_open(1U);

    // Audio is real-time: a chunk that cannot leave now is worth less than a
    // fast reconnection, so it is dropped and counted, never queued forever.
    TEST_ASSERT_NOT_EQUAL_INT(0, stt_ws_client_send_audio(&client, pcm, sizeof(pcm), 1234U, 0U));

    stt_ws_client_stats(&client, &stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.chunks_dropped_tx);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.chunks_sent);
}

void test_stt_ws_client_send_audio_validates_its_arguments(void)
{
    uint8_t pcm[4];
    static uint8_t oversized[4096];

    init_ready_client();
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_send_audio(NULL, pcm, sizeof(pcm), 0U, 0U));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_send_audio(&client, NULL, sizeof(pcm), 0U, 0U));
    TEST_ASSERT_EQUAL_INT(-EINVAL, stt_ws_client_send_audio(&client, pcm, 0U, 0U, 0U));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          stt_ws_client_send_audio(&client, oversized, sizeof(oversized), 0U, 0U));
}

void test_stt_ws_client_submit_audio_is_bounded_and_drops_the_oldest_chunk(void)
{
    stt_ws_client_stats_t stats;
    uint8_t pcm[4];
    uint32_t i;

    init_ready_client();
    memset(pcm, 0, sizeof(pcm));
    TEST_ASSERT_EQUAL_INT(-EAGAIN,
                          stt_ws_client_submit_audio(&client, pcm, sizeof(pcm), 10U, 0U));

    client.worker_started = 1U; // exercise only the nonblocking handoff, no I/O thread
    for (i = 0U; i < (STT_WS_AUDIO_QUEUE_DEPTH + 2U); i++)
    {
        pcm[0] = (uint8_t)i;
        TEST_ASSERT_EQUAL_INT(0,
                              stt_ws_client_submit_audio(&client,
                                                         pcm,
                                                         sizeof(pcm),
                                                         (uint64_t)i,
                                                         3U));
    }

    TEST_ASSERT_EQUAL_UINT8(STT_WS_AUDIO_QUEUE_DEPTH, client.audio_count);
    TEST_ASSERT_EQUAL_UINT8(2U, client.audio_queue[client.audio_head].payload[0]);
    TEST_ASSERT_EQUAL_UINT32(3U, client.audio_queue[client.audio_head].dropped);
    stt_ws_client_stats(&client, &stats);
    TEST_ASSERT_EQUAL_UINT32(2U, stats.chunks_dropped_tx);
    client.worker_started = 0U;
}

// === Full session over the fake transport ======================================================================== //

/// @brief Drive a client from IDLE to READY against the scripted server.
static void bring_session_up(void)
{
    init_ready_client();
    client.config.use_tls = 0U; // the clock guard is a TLS-only concern
    client.state = STT_WS_STATE_IDLE;
    client.next_attempt_ms = 0U;

    fake_net_tls_auto_handshake(1U);
    fake_net_tls_push_text("{\"type\":\"session_ready\",\"version\":1,"
                           "\"sample_rate_hz\":48000,\"run_engine\":\"nemotron_3_5_nemo\","
                           "\"run_config\":{\"config_latency_ms\":560}}");

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_service(&client));
    TEST_ASSERT_EQUAL_INT(STT_WS_STATE_READY, stt_ws_client_state(&client));
}

void test_stt_ws_client_worker_has_an_asynchronous_stop_lifecycle(void)
{
    uint32_t attempts;

    init_ready_client();
    client.config.use_tls = 0U;
    fake_net_tls_auto_handshake(1U);
    fake_net_tls_push_text("{\"type\":\"session_ready\",\"version\":1,"
                           "\"sample_rate_hz\":48000,\"run_engine\":\"fake\","
                           "\"run_config\":{}}");

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_start(&client));
    TEST_ASSERT_EQUAL_UINT8(0U, stt_ws_client_stop_complete(&client));

    for (attempts = 0U; attempts < 200U; attempts++)
    {
        if (stt_ws_client_state(&client) == STT_WS_STATE_READY)
        {
            break;
        }
        (void)usleep(1000U);
    }
    TEST_ASSERT_EQUAL_INT(STT_WS_STATE_READY, stt_ws_client_state(&client));

    stt_ws_client_request_stop(&client);
    for (attempts = 0U; attempts < 200U; attempts++)
    {
        if (stt_ws_client_stop_complete(&client) != 0U)
        {
            break;
        }
        (void)usleep(1000U);
    }
    TEST_ASSERT_EQUAL_UINT8(1U, stt_ws_client_stop_complete(&client));
    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_finish_stop(&client));
    TEST_ASSERT_EQUAL_UINT8(0U, client.worker_started);
}

void test_stt_ws_client_completes_the_handshake_and_opening_message(void)
{
    uint8_t const* sent;
    size_t sent_len = 0U;

    bring_session_up();

    sent = fake_net_tls_tx(&sent_len);
    TEST_ASSERT_NOT_NULL(strstr((char const*)sent, "GET /stt/stream HTTP/1.1\r\n"));
    TEST_ASSERT_NOT_NULL(strstr((char const*)sent, "Host: example.test:443\r\n"));
    TEST_ASSERT_NOT_NULL(strstr((char const*)sent, "Upgrade: websocket\r\n"));
    TEST_ASSERT_NOT_NULL(strstr((char const*)sent, "Sec-WebSocket-Version: 13\r\n"));
    // session_start rides in a masked text frame, so it is not readable as
    // plain text in the stream; the session reaching READY is the proof that
    // the server-side parse of it succeeded.
    TEST_ASSERT_EQUAL_UINT32(1U, fake_net_tls_open_count());
}

void test_stt_ws_client_receives_a_transcript_over_a_live_session(void)
{
    uint32_t count = 0U;

    bring_session_up();
    fake_net_tls_push_text("{\"type\":\"transcript\",\"seq\":0,\"is_final\":true,"
                           "\"start_sec\":0.0,\"end_sec\":1.5,\"text\":\"hola mundo\","
                           "\"att_context_size\":[56,6],\"final_reason\":\"model_eou\"}");

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_service(&client));
    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_poll_events(&client, events, STT_WS_EVENT_RING_DEPTH,
                                                       &count));
    TEST_ASSERT_EQUAL_UINT32(1U, count);
    TEST_ASSERT_EQUAL_UINT32(0U, events[0].seq);
    TEST_ASSERT_EQUAL_UINT8(1U, events[0].is_final);
    TEST_ASSERT_EQUAL_UINT32(1500U, events[0].end_ms);
    TEST_ASSERT_EQUAL_STRING("hola mundo", events[0].text);
}

void test_stt_ws_client_answers_a_server_ping_with_a_pong(void)
{
    uint8_t const* sent;
    size_t sent_len = 0U;

    bring_session_up();
    fake_net_tls_clear_tx();

    // uvicorn pings every 20 s and closes after 20 s without a pong, so this
    // is what keeps an idle session alive between utterances.
    fake_net_tls_push_frame(0x9U, "hb", 2U);
    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_service(&client));

    sent = fake_net_tls_tx(&sent_len);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(8U, (uint32_t)sent_len);
    TEST_ASSERT_EQUAL_HEX8(0x8AU, sent[0]);          // FIN + pong
    TEST_ASSERT_EQUAL_HEX8(0x82U, sent[1] & 0xFEU);  // masked, 2-byte payload
}

void test_stt_ws_client_sends_audio_with_the_protocol_chunk_header(void)
{
    stt_ws_client_stats_t stats;
    uint8_t pcm[1920];
    uint8_t const* sent;
    size_t sent_len = 0U;
    size_t payload;
    uint8_t mask[4];
    uint8_t header[20];
    size_t i;

    bring_session_up();
    fake_net_tls_clear_tx();
    memset(pcm, 0x5A, sizeof(pcm));

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_send_audio(&client, pcm, sizeof(pcm), 42U, 7U));

    sent = fake_net_tls_tx(&sent_len);
    // Binary frame, masked, 16-bit length: 2 + 2 + 4 header bytes.
    TEST_ASSERT_EQUAL_HEX8(0x82U, sent[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFEU, sent[1]);
    payload = ((size_t)sent[2] << 8U) | (size_t)sent[3];
    TEST_ASSERT_EQUAL_size_t(20U + sizeof(pcm), payload);
    memcpy(mask, &sent[4], sizeof(mask));

    // Unmask the 20-byte header and check it against protocol.py's "!QQI".
    for (i = 0U; i < sizeof(header); i++)
    {
        header[i] = (uint8_t)(sent[8U + i] ^ mask[i % 4U]);
    }
    for (i = 0U; i < 8U; i++)
    {
        TEST_ASSERT_EQUAL_HEX8(0x00U, header[i]); // seq 0, big endian
    }
    TEST_ASSERT_EQUAL_HEX8(42U, header[15]);  // timestamp_ns low byte
    TEST_ASSERT_EQUAL_HEX8(7U, header[19]);   // dropped counter low byte

    stt_ws_client_stats(&client, &stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.chunks_sent);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)sizeof(pcm), stats.bytes_sent);
}

void test_stt_ws_client_reconnects_after_the_server_closes(void)
{
    stt_ws_client_stats_t stats;

    bring_session_up();
    fake_net_tls_push_frame(0x8U, NULL, 0U); // server CLOSE

    TEST_ASSERT_EQUAL_INT(-EAGAIN, stt_ws_client_service(&client));
    TEST_ASSERT_EQUAL_INT(STT_WS_STATE_BACKOFF, stt_ws_client_state(&client));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_net_tls_close_count());

    stt_ws_client_stats(&client, &stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.reconnects);
}

void test_stt_ws_client_treats_a_busy_server_as_retryable(void)
{
    stt_ws_client_stats_t stats;

    bring_session_up();
    // One GPU session at a time; a reconnect that lands too early gets this.
    fake_net_tls_push_text("{\"type\":\"error\",\"message\":\"server busy\",\"busy\":true}");

    TEST_ASSERT_EQUAL_INT(-EAGAIN, stt_ws_client_service(&client));
    TEST_ASSERT_EQUAL_INT(STT_WS_STATE_BACKOFF, stt_ws_client_state(&client));

    stt_ws_client_stats(&client, &stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.busy_rejections);
}

void test_stt_ws_client_refuses_a_session_ready_with_a_newer_protocol_version(void)
{
    init_ready_client();
    client.config.use_tls = 0U;
    client.state = STT_WS_STATE_IDLE;
    client.next_attempt_ms = 0U;
    client.config.handshake_timeout_ms = 50U;

    fake_net_tls_auto_handshake(1U);
    // A newer server could change the audio framing; refusing beats guessing.
    fake_net_tls_push_text("{\"type\":\"session_ready\",\"version\":2}");

    TEST_ASSERT_EQUAL_INT(-EAGAIN, stt_ws_client_service(&client));
    TEST_ASSERT_NOT_EQUAL_INT(STT_WS_STATE_READY, stt_ws_client_state(&client));
}

void test_stt_ws_client_reassembles_a_fragmented_transcript(void)
{
    uint32_t count = 0U;
    char const* const head = "{\"type\":\"transcript\",\"seq\":0,\"is_final\":true,"
                             "\"start_sec\":0.0,";
    char const* const tail = "\"end_sec\":1.0,\"text\":\"partido\"}";

    bring_session_up();
    // A server is free to split a message; dropping the session over that
    // would look like a flapping link rather than a protocol detail.
    fake_net_tls_push_fragment(0x1U, 0U, head, strlen(head));
    fake_net_tls_push_fragment(0x0U, 1U, tail, strlen(tail));

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_service(&client));
    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_poll_events(&client, events, STT_WS_EVENT_RING_DEPTH,
                                                       &count));
    TEST_ASSERT_EQUAL_UINT32(1U, count);
    TEST_ASSERT_EQUAL_STRING("partido", events[0].text);
    TEST_ASSERT_EQUAL_INT(STT_WS_STATE_READY, stt_ws_client_state(&client));
}

void test_stt_ws_client_ignores_a_continuation_without_a_start(void)
{
    stt_ws_client_stats_t stats;

    bring_session_up();
    fake_net_tls_push_fragment(0x0U, 1U, "orphan", 6U);

    TEST_ASSERT_EQUAL_INT(0, stt_ws_client_service(&client));
    TEST_ASSERT_EQUAL_INT(STT_WS_STATE_READY, stt_ws_client_state(&client));
    stt_ws_client_stats(&client, &stats);
    TEST_ASSERT_EQUAL_UINT32(1U, stats.protocol_errors);
}
