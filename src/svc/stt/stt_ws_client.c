/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file stt_ws_client.c
/// @brief Outbound WebSocket session carrying audio out and transcripts back
///

// === Headers files inclusions ==================================================================================== //

#include "stt_ws_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "number_parse.h"
#include "stt_ws_frame.h"

// === Macros definitions ========================================================================================== //

#define STT_WS_AUDIO_HEADER_BYTES (20U)
#define STT_WS_TX_MAX             (STT_WS_FRAME_HEADER_MAX + STT_WS_AUDIO_HEADER_BYTES + 2048U)
#define STT_WS_READ_SLICE         (2048U)
#define STT_WS_SEND_TIMEOUT_MS    (2000U)
#define STT_WS_HEALTHY_SESSION_MS (60000U)
#define STT_WS_PORT_MAX           (65535U)
#define STT_WS_WORKER_WAIT_MS     (100U)

#define STT_WS_DEFAULT_CONNECT_TIMEOUT_MS   (5000U)
#define STT_WS_DEFAULT_HANDSHAKE_TIMEOUT_MS (10000U)
#define STT_WS_DEFAULT_IDLE_TIMEOUT_MS      (45000U)
#define STT_WS_DEFAULT_PING_INTERVAL_MS     (15000U)
#define STT_WS_DEFAULT_BACKOFF_MIN_MS       (500U)
#define STT_WS_DEFAULT_BACKOFF_MAX_MS       (30000U)
#define STT_WS_DEFAULT_CA_FILE              "/etc/ssl/certs/ca-certificates.crt"

#define STT_WS_DEFAULT_SAMPLE_RATE_HZ  (48000U)
#define STT_WS_DEFAULT_CHUNK_MS        (20U)
#define STT_WS_DEFAULT_LATENCY_MS      (560U)
#define STT_WS_DEFAULT_STOP_HISTORY_MS (600U)
#define STT_WS_DEFAULT_RESIDUE_TOKENS  (2U)
#define STT_WS_DEFAULT_TARGET_LANG     "es-ES"

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static uint64_t now_ms(void);
static uint32_t random_u32(void);
static void fill_random(uint8_t* buffer, size_t size);
static void put_u64_be(uint8_t* out, uint64_t value);
static void put_u32_be(uint8_t* out, uint32_t value);
static void env_string(char* dst, size_t dst_size, char const* name);
static uint32_t env_u32(char const* name, uint32_t fallback, uint32_t min_value, uint32_t max_value);
static void enter_backoff(stt_ws_client_t* client, char const* reason);
static void drop_session(stt_ws_client_t* client, char const* reason);
static void client_set_state(stt_ws_client_t* client, stt_ws_state_e state);
static void client_stats_inc(stt_ws_client_t* client, uint32_t* counter);
static uint8_t client_stop_requested(stt_ws_client_t* client);
static void worker_wait(stt_ws_client_t* client, uint32_t timeout_ms);
static int audio_pop(stt_ws_client_t* client, stt_ws_audio_chunk_t* chunk);
static void audio_discard_pending(stt_ws_client_t* client);
static void* worker_main(void* arg);
static int send_frame(stt_ws_client_t* client,
                      stt_ws_opcode_e opcode,
                      void const* payload,
                      size_t payload_len);
static int open_connection(stt_ws_client_t* client);
static int run_upgrade(stt_ws_client_t* client);
static int start_session(stt_ws_client_t* client);
static int ensure_connected(stt_ws_client_t* client);
static void push_event(stt_ws_client_t* client, char const* line, size_t length);
static int handle_text_message(stt_ws_client_t* client, char const* text);
static int handle_frame(stt_ws_client_t* client, stt_ws_frame_t const* frame);
static int pump_rx(stt_ws_client_t* client, uint32_t wait_ms);
static void maybe_ping(stt_ws_client_t* client);
static void shared_init(void);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //

/// One outbound session shared by the capture handoff, its network worker and QP/C.
static stt_ws_client_t shared_instance;
static pthread_once_t shared_once = PTHREAD_ONCE_INIT;
static uint8_t shared_ready;

// === Private function implementation ============================================================================= //

/** @brief Monotonic milliseconds; unaffected by an NTP step of the wall clock. */
static uint64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0U;
    }

    return ((uint64_t)ts.tv_sec * 1000U) + ((uint64_t)ts.tv_nsec / 1000000U);
}

/** @brief Fill @p buffer from the kernel entropy pool, with a weak fallback. */
static void fill_random(uint8_t* const buffer, size_t size)
{
    int const fd = open("/dev/urandom", O_RDONLY);
    size_t used = 0U;

    if (fd >= 0)
    {
        while (used < size)
        {
            ssize_t const got = read(fd, &buffer[used], size - used);

            if (got <= 0)
            {
                break;
            }
            used += (size_t)got;
        }
        close(fd);
    }

    // Masking keys are an integrity aid against confused proxies, not a secret,
    // so a clock-derived fallback is acceptable if /dev/urandom is unavailable.
    if (used < size)
    {
        uint64_t seed = now_ms() ^ ((uint64_t)(uintptr_t)buffer);

        while (used < size)
        {
            seed = (seed * 6364136223846793005ULL) + 1442695040888963407ULL;
            buffer[used++] = (uint8_t)(seed >> 33U);
        }
    }
}

/**
 * @brief One random 32-bit value, used for masking keys and backoff jitter.
 *
 * Seeded once from `/dev/urandom` and advanced in memory afterwards: a masking
 * key is an integrity aid against confused intermediaries, not a secret, and
 * opening the device for every 20 ms frame would be pure syscall overhead.
 */
static uint32_t random_u32(void)
{
    static uint64_t state = 0U;

    if (state == 0U)
    {
        uint8_t seed[8];

        fill_random(seed, sizeof(seed));
        state = ((uint64_t)seed[0] << 56U) | ((uint64_t)seed[1] << 48U)
                | ((uint64_t)seed[2] << 40U) | ((uint64_t)seed[3] << 32U)
                | ((uint64_t)seed[4] << 24U) | ((uint64_t)seed[5] << 16U)
                | ((uint64_t)seed[6] << 8U) | (uint64_t)seed[7];
        if (state == 0U)
        {
            state = 0x9E3779B97F4A7C15ULL;
        }
    }

    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return (uint32_t)(state >> 32U);
}

static void put_u64_be(uint8_t* const out, uint64_t value)
{
    unsigned int i;

    for (i = 0U; i < 8U; i++)
    {
        out[i] = (uint8_t)(value >> (56U - (i * 8U)));
    }
}

static void put_u32_be(uint8_t* const out, uint32_t value)
{
    unsigned int i;

    for (i = 0U; i < 4U; i++)
    {
        out[i] = (uint8_t)(value >> (24U - (i * 8U)));
    }
}

/** @brief Copy a non-empty environment string into a fixed-size field. */
static void env_string(char* const dst, size_t dst_size, char const* const name)
{
    char const* const value = getenv(name);

    if ((value != NULL) && (value[0] != '\0'))
    {
        snprintf(dst, dst_size, "%s", value);
    }
}

/** @brief Read a bounded unsigned environment override, keeping @p fallback when invalid. */
static uint32_t env_u32(char const* const name,
                        uint32_t fallback,
                        uint32_t min_value,
                        uint32_t max_value)
{
    char const* const value = getenv(name);
    uint32_t parsed;

    if ((value == NULL) || (value[0] == '\0'))
    {
        return fallback;
    }
    if (number_parse_u32(value, strlen(value), min_value, max_value, &parsed) != 0)
    {
        LOG_WARNING("stt-ws: ignoring invalid %s='%s'", name, value);
        return fallback;
    }

    return parsed;
}

static void client_set_state(stt_ws_client_t* const client, stt_ws_state_e const state)
{
    pthread_mutex_lock(&client->lock);
    client->state = state;
    pthread_mutex_unlock(&client->lock);
}

static void client_stats_inc(stt_ws_client_t* const client, uint32_t* const counter)
{
    pthread_mutex_lock(&client->lock);
    (*counter)++;
    pthread_mutex_unlock(&client->lock);
}

static uint8_t client_stop_requested(stt_ws_client_t* const client)
{
    uint8_t requested;

    pthread_mutex_lock(&client->lock);
    requested = client->stop_requested;
    pthread_mutex_unlock(&client->lock);
    return requested;
}

/** @brief Sleep until audio/stop arrives, bounded so reconnect timers progress. */
static void worker_wait(stt_ws_client_t* const client, uint32_t const timeout_ms)
{
    struct timespec deadline;

    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
    {
        return;
    }
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    deadline.tv_sec += (time_t)(timeout_ms / 1000U) + (deadline.tv_nsec / 1000000000L);
    deadline.tv_nsec %= 1000000000L;

    pthread_mutex_lock(&client->lock);
    if ((client->audio_count == 0U) && (client->stop_requested == 0U))
    {
        (void)pthread_cond_timedwait(&client->worker_cond, &client->lock, &deadline);
    }
    pthread_mutex_unlock(&client->lock);
}

static int audio_pop(stt_ws_client_t* const client, stt_ws_audio_chunk_t* const chunk)
{
    int status = -EAGAIN;

    pthread_mutex_lock(&client->lock);
    if (client->audio_count > 0U)
    {
        *chunk = client->audio_queue[client->audio_head];
        chunk->dropped += client->audio_dropped_total;
        client->audio_head = (uint8_t)((client->audio_head + 1U) % STT_WS_AUDIO_QUEUE_DEPTH);
        client->audio_count--;
        status = 0;
    }
    pthread_mutex_unlock(&client->lock);
    return status;
}

/** @brief Start every new model session at live audio, never a reconnect backlog. */
static void audio_discard_pending(stt_ws_client_t* const client)
{
    pthread_mutex_lock(&client->lock);
    client->stats.chunks_dropped_tx += client->audio_count;
    client->audio_dropped_total += client->audio_count;
    client->audio_head = 0U;
    client->audio_count = 0U;
    pthread_mutex_unlock(&client->lock);
}

/** @brief Schedule the next attempt with exponential backoff plus jitter. */
static void enter_backoff(stt_ws_client_t* const client, char const* const reason)
{
    uint32_t const min_ms = client->config.backoff_min_ms;
    uint32_t const max_ms = client->config.backoff_max_ms;
    uint32_t jitter;

    pthread_mutex_lock(&client->lock);
    if (client->backoff_ms == 0U)
    {
        client->backoff_ms = min_ms;
    }
    else
    {
        client->backoff_ms = (client->backoff_ms > (max_ms / 2U)) ? max_ms
                                                                 : (client->backoff_ms * 2U);
    }

    // Jitter keeps a board that reboots in a loop from hammering the same
    // instant, and spreads retries if the server restarts.
    jitter = (client->backoff_ms > 8U) ? (random_u32() % (client->backoff_ms / 4U)) : 0U;
    client->state = STT_WS_STATE_BACKOFF;
    client->next_attempt_ms = now_ms() + (uint64_t)client->backoff_ms + (uint64_t)jitter;
    pthread_mutex_unlock(&client->lock);

    LOG_WARNING("stt-ws: %s; retrying in %lu ms", reason,
                (unsigned long)(client->backoff_ms + jitter));
}

/** @brief Tear down the live session and schedule a reconnect. */
static void drop_session(stt_ws_client_t* const client, char const* const reason)
{
    if (client->conn != NULL)
    {
        net_tls_close(client->conn);
        client->conn = NULL;
    }
    pthread_mutex_lock(&client->lock);
    client->rx_used = 0U;
    client->msg_used = 0U;
    client->msg_active = 0U;
    client->msg_overflow = 0U;
    // Invalidate and discard every event from the dead session. Generation is
    // carried with each ring entry so even a batch copied by SttAO just before
    // this reset cannot poison the next session's seq=0.
    client->session_generation++;
    client->have_last_event_seq = 0U;
    client->last_event_seq = 0U;
    client->last_event_generation = client->session_generation;
    client->stats.events_dropped_ring += client->ring_count;
    client->ring_head = 0U;
    client->ring_count = 0U;

    if (client->state == STT_WS_STATE_READY)
    {
        client->stats.reconnects++;
    }
    pthread_mutex_unlock(&client->lock);
    enter_backoff(client, reason);
}

/** @brief Encode and send one masked client frame. */
static int send_frame(stt_ws_client_t* const client,
                      stt_ws_opcode_e opcode,
                      void const* const payload,
                      size_t payload_len)
{
    uint8_t buffer[STT_WS_TX_MAX];
    int const encoded = stt_ws_frame_encode(buffer, sizeof(buffer), opcode, payload, payload_len,
                                            random_u32());
    int sent;

    if (encoded < 0)
    {
        return encoded;
    }

    sent = net_tls_send(client->conn, buffer, (size_t)encoded, STT_WS_SEND_TIMEOUT_MS);
    if (sent != encoded)
    {
        return (sent < 0) ? sent : -EIO;
    }

    client->last_tx_ms = now_ms();
    return 0;
}

/** @brief Open the TCP/TLS transport. */
static int open_connection(stt_ws_client_t* const client)
{
    net_tls_config_t transport;

    memset(&transport, 0, sizeof(transport));
    transport.host = client->config.host;
    transport.port = client->config.port;
    transport.ca_file = (client->config.ca_file[0] != '\0') ? client->config.ca_file : NULL;
    transport.ca_dir = (client->config.ca_dir[0] != '\0') ? client->config.ca_dir : NULL;
    transport.connect_timeout_ms = client->config.connect_timeout_ms;
    transport.handshake_timeout_ms = client->config.handshake_timeout_ms;
    transport.use_tls = client->config.use_tls;

    client->conn = net_tls_open(&transport);
    if (client->conn == NULL)
    {
        char detail[NET_TLS_ERROR_MAX];

        net_tls_last_error(NULL, detail, sizeof(detail));
        if (client->config.use_tls != 0U)
        {
            client_stats_inc(client, &client->stats.tls_failures);
        }
        client_stats_inc(client, &client->stats.connect_failures);
        LOG_ERROR("stt-ws: connection to %s:%u failed: %s", client->config.host,
                  (unsigned)client->config.port, detail);
        return -ECONNREFUSED;
    }

    return 0;
}

/** @brief Perform the HTTP Upgrade exchange. */
static int run_upgrade(stt_ws_client_t* const client)
{
    char request[512];
    char expected_accept[STT_WS_ACCEPT_SIZE];
    char host_header[STT_WS_HOST_MAX + 8U];
    uint8_t nonce[STT_WS_KEY_NONCE_BYTES];
    char response[1024];
    size_t used = 0U;
    size_t header_len = 0U;
    int length;

    fill_random(nonce, sizeof(nonce));
    // Default ports stay implicit, as HTTP requires.
    if (((client->config.use_tls != 0U) && (client->config.port == 443U))
        || ((client->config.use_tls == 0U) && (client->config.port == 80U)))
    {
        snprintf(host_header, sizeof(host_header), "%s", client->config.host);
    }
    else
    {
        snprintf(host_header, sizeof(host_header), "%s:%u", client->config.host,
                 (unsigned)client->config.port);
    }

    length = stt_ws_handshake_build_request(request, sizeof(request), host_header,
                                            client->config.path, nonce, expected_accept,
                                            sizeof(expected_accept));
    if (length < 0)
    {
        return length;
    }
    if (net_tls_send(client->conn, request, (size_t)length, client->config.handshake_timeout_ms)
        != length)
    {
        return -EIO;
    }

    for (;;)
    {
        int status;
        int received;

        status = stt_ws_handshake_validate_response(response, used, expected_accept, &header_len);
        if (status == 0)
        {
            break;
        }
        if (status != -EAGAIN)
        {
            client_stats_inc(client, &client->stats.handshake_failures);
            LOG_ERROR("stt-ws: server refused the WebSocket upgrade");
            return status;
        }
        if (used >= sizeof(response))
        {
            client_stats_inc(client, &client->stats.handshake_failures);
            return -EPROTO;
        }

        received = net_tls_recv(client->conn, &response[used], sizeof(response) - used,
                                client->config.handshake_timeout_ms);
        if (received <= 0)
        {
            client_stats_inc(client, &client->stats.handshake_failures);
            return (received == 0) ? -ECONNRESET : received;
        }
        used += (size_t)received;
    }

    // Bytes after the header block already belong to the first frame.
    if (used > header_len)
    {
        client->rx_used = used - header_len;
        memcpy(client->rx, &response[header_len], client->rx_used);
    }

    return 0;
}

/** @brief Send `session_start` and wait for `session_ready`. */
static int start_session(stt_ws_client_t* const client)
{
    char start[512];
    int const length = stt_session_json_build_start(start, sizeof(start), &client->config.session);
    uint64_t const deadline = now_ms() + (uint64_t)client->config.handshake_timeout_ms;

    if (length < 0)
    {
        LOG_ERROR("stt-ws: cannot build session_start (code %d)", length);
        return length;
    }
    if (send_frame(client, STT_WS_OPCODE_TEXT, start, (size_t)length) != 0)
    {
        return -EIO;
    }

    while (now_ms() < deadline)
    {
        int const status = pump_rx(client, 50U);

        if (status != 0)
        {
            return status;
        }
        if (stt_ws_client_state(client) == STT_WS_STATE_READY)
        {
            return 0;
        }
        if (client_stop_requested(client) != 0U)
        {
            return -ECANCELED;
        }
    }

    LOG_ERROR("stt-ws: timed out waiting for session_ready");
    return -ETIMEDOUT;
}

/** @brief Bring the session up if it is not, honouring the backoff schedule. */
static int ensure_connected(stt_ws_client_t* const client)
{
    if (stt_ws_client_state(client) == STT_WS_STATE_READY)
    {
        return 0;
    }
    if (now_ms() < client->next_attempt_ms)
    {
        return -EAGAIN;
    }

    // The board has no RTC and boots at its rootfs build date. Attempting TLS
    // then yields "certificate is not yet valid" on every retry, so wait for a
    // plausible clock and say so instead of burning connections.
    if ((client->config.use_tls != 0U) && (client->config.min_epoch > 0)
        && ((int64_t)time(NULL) < client->config.min_epoch))
    {
        pthread_mutex_lock(&client->lock);
        client->stats.clock_deferrals++;
        client->state = STT_WS_STATE_WAIT_CLOCK;
        client->next_attempt_ms = now_ms() + client->config.backoff_max_ms;
        pthread_mutex_unlock(&client->lock);
        LOG_WARNING("stt-ws: system clock is not set yet; deferring TLS");
        return -EAGAIN;
    }

    client_set_state(client, STT_WS_STATE_CONNECTING);
    if (open_connection(client) != 0)
    {
        enter_backoff(client, "connect failed");
        return -EAGAIN;
    }
    if (client_stop_requested(client) != 0U)
    {
        net_tls_close(client->conn);
        client->conn = NULL;
        client_set_state(client, STT_WS_STATE_IDLE);
        return -ECANCELED;
    }

    client_set_state(client, STT_WS_STATE_HANDSHAKING);
    if (run_upgrade(client) != 0)
    {
        drop_session(client, "websocket upgrade failed");
        return -EAGAIN;
    }
    if (client_stop_requested(client) != 0U)
    {
        net_tls_close(client->conn);
        client->conn = NULL;
        client_set_state(client, STT_WS_STATE_IDLE);
        return -ECANCELED;
    }

    client_set_state(client, STT_WS_STATE_STARTING);
    client->last_rx_ms = now_ms();
    if (start_session(client) != 0)
    {
        drop_session(client, "session_start failed");
        return -EAGAIN;
    }

    client_stats_inc(client, &client->stats.sessions);
    client->session_started_ms = now_ms();
    LOG_INFO("stt-ws: session ready with %s://%s:%u%s",
             (client->config.use_tls != 0U) ? "wss" : "ws", client->config.host,
             (unsigned)client->config.port, client->config.path);
    return 0;
}

/** @brief Buffer one transcript line for the QP/C thread, shedding partials first. */
static void push_event(stt_ws_client_t* const client, char const* const line, size_t length)
{
    uint8_t const is_final = (strstr(line, "\"is_final\":true") != NULL) ? 1U : 0U;
    uint32_t tail;

    if (length >= STT_WS_LINE_MAX)
    {
        client_stats_inc(client, &client->stats.protocol_errors);
        return;
    }

    pthread_mutex_lock(&client->lock);

    if (client->ring_count == STT_WS_EVENT_RING_DEPTH)
    {
        // Full: shed the oldest *partial* first. A partial is superseded by the
        // next one anyway, while a final is a line the viewer would never see.
        uint32_t victim = STT_WS_EVENT_RING_DEPTH;
        uint32_t i;

        for (i = 0U; i < STT_WS_EVENT_RING_DEPTH; i++)
        {
            uint32_t const slot = ((uint32_t)client->ring_head + i) % STT_WS_EVENT_RING_DEPTH;

            if (client->ring[slot].is_final == 0U)
            {
                victim = slot;
                break;
            }
        }

        if (victim == STT_WS_EVENT_RING_DEPTH)
        {
            victim = client->ring_head; // only finals queued: drop the oldest
        }

        // Close the gap so the ring stays contiguous from ring_head.
        while (victim != client->ring_head)
        {
            uint32_t const previous =
                (victim + STT_WS_EVENT_RING_DEPTH - 1U) % STT_WS_EVENT_RING_DEPTH;

            client->ring[victim] = client->ring[previous];
            victim = previous;
        }
        client->ring_head = (uint8_t)((client->ring_head + 1U) % STT_WS_EVENT_RING_DEPTH);
        client->ring_count--;
        client->stats.events_dropped_ring++;
    }

    tail = ((uint32_t)client->ring_head + (uint32_t)client->ring_count) % STT_WS_EVENT_RING_DEPTH;
    memcpy(client->ring[tail].line, line, length);
    client->ring[tail].line[length] = '\0';
    client->ring[tail].length = (uint16_t)length;
    client->ring[tail].is_final = is_final;
    client->ring[tail].session_generation = client->session_generation;
    client->ring_count++;

    client->stats.transcripts_received++;
    if (is_final != 0U)
    {
        client->stats.transcripts_final++;
    }
    else
    {
        client->stats.transcripts_partial++;
    }

    pthread_mutex_unlock(&client->lock);
}

/** @brief Dispatch one complete server text message. */
static int handle_text_message(stt_ws_client_t* const client, char const* const text)
{
    switch (stt_session_json_message_type(text))
    {
    case STT_SESSION_MSG_TRANSCRIPT:
        push_event(client, text, strlen(text));
        break;

    case STT_SESSION_MSG_SESSION_READY:
    {
        stt_session_ready_t ready;

        if (stt_session_json_parse_ready(text, &ready) != 0)
        {
            client_stats_inc(client, &client->stats.protocol_errors);
            LOG_ERROR("stt-ws: unusable session_ready (wrong protocol version?)");
            return -EPROTO;
        }
        pthread_mutex_lock(&client->lock);
        client->state = STT_WS_STATE_READY;
        client->backoff_ms = 0U;
        client->audio_seq = 0U;
        pthread_mutex_unlock(&client->lock);
        audio_discard_pending(client);
        // The negotiated configuration is the one the run must be judged by.
        LOG_INFO("stt-ws: engine=%s effective run_config=%s%s", ready.run_engine, ready.run_config,
                 (ready.run_config_truncated != 0U) ? " (truncated)" : "");
        break;
    }

    case STT_SESSION_MSG_ERROR:
    {
        stt_session_error_t error;

        (void)stt_session_json_parse_error(text, &error);
        if (error.busy != 0U)
        {
            client_stats_inc(client, &client->stats.busy_rejections);
            LOG_WARNING("stt-ws: server busy with another session");
        }
        else
        {
            LOG_ERROR("stt-ws: server error: %s", error.message);
        }
        return -ECONNABORTED;
    }

    case STT_SESSION_MSG_SESSION_SUMMARY:
        LOG_INFO("stt-ws: session summary received");
        break;

    case STT_SESSION_MSG_PONG:
        break;

    default:
        client_stats_inc(client, &client->stats.protocol_errors);
        break;
    }

    return 0;
}

/** @brief Act on one decoded frame, reassembling split messages. */
static int handle_frame(stt_ws_client_t* const client, stt_ws_frame_t const* const frame)
{
    size_t const length = (size_t)frame->payload_len;

    switch (frame->opcode)
    {
    case STT_WS_OPCODE_PING:
        // uvicorn pings every 20 s and closes after 20 s without a pong, so
        // answering is what keeps an idle session alive.
        return send_frame(client, STT_WS_OPCODE_PONG, frame->payload, length);

    case STT_WS_OPCODE_PONG:
        return 0;

    case STT_WS_OPCODE_CLOSE:
        return -ECONNRESET;

    case STT_WS_OPCODE_BINARY:
        // The server never sends binary; ignoring beats desynchronising.
        client_stats_inc(client, &client->stats.protocol_errors);
        return 0;

    case STT_WS_OPCODE_TEXT:
    case STT_WS_OPCODE_CONTINUATION:
    default:
        break;
    }

    if (frame->opcode == STT_WS_OPCODE_TEXT)
    {
        client->msg_used = 0U;
        client->msg_overflow = 0U;
        client->msg_active = 1U;
    }
    else if (client->msg_active == 0U)
    {
        client_stats_inc(client, &client->stats.protocol_errors);
        return 0; // continuation without a start
    }
    else
    {
        // Continuing an in-progress message.
    }

    if ((client->msg_used + length) >= sizeof(client->msg))
    {
        client->msg_overflow = 1U;
    }
    else
    {
        memcpy(&client->msg[client->msg_used], frame->payload, length);
        client->msg_used += length;
    }

    if (frame->fin == 0U)
    {
        return 0;
    }

    client->msg_active = 0U;
    if (client->msg_overflow != 0U)
    {
        client_stats_inc(client, &client->stats.protocol_errors);
        client->msg_used = 0U;
        return 0;
    }

    client->msg[client->msg_used] = '\0';
    client->msg_used = 0U;
    return handle_text_message(client, client->msg);
}

/**
 * @brief Read whatever is available and process complete frames.
 * @return 0 on success, or a negative errno-style value on a fatal session error.
 */
static int pump_rx(stt_ws_client_t* const client, uint32_t const wait_ms)
{
    for (;;)
    {
        stt_ws_frame_t frame;
        int status;
        int received;
        size_t consumed;

        status = stt_ws_frame_decode(client->rx, client->rx_used, STT_WS_RX_MAX_PAYLOAD, &frame);
        if (status == 0)
        {
            consumed = frame.header_len + (size_t)frame.payload_len;
            status = handle_frame(client, &frame);
            memmove(client->rx, &client->rx[consumed], client->rx_used - consumed);
            client->rx_used -= consumed;
            client->last_rx_ms = now_ms();
            if (status != 0)
            {
                return status;
            }
            continue;
        }
        if (status != -EAGAIN)
        {
            client_stats_inc(client, &client->stats.protocol_errors);
            return status;
        }
        if (client->rx_used >= sizeof(client->rx))
        {
            client_stats_inc(client, &client->stats.protocol_errors);
            return -EMSGSIZE;
        }

        // Only wait when the transport already holds decrypted bytes; otherwise
        // return immediately so the caller keeps streaming audio on time.
        received = net_tls_recv(client->conn, &client->rx[client->rx_used],
                                sizeof(client->rx) - client->rx_used,
                                (net_tls_pending(client->conn) > 0U) ? 1U : wait_ms);
        if (received == -EAGAIN)
        {
            return 0;
        }
        if (received == 0)
        {
            return -ECONNRESET;
        }
        if (received < 0)
        {
            return received;
        }
        client->rx_used += (size_t)received;
    }
}

/** @brief Send an application ping when the link has been quiet. */
static void maybe_ping(stt_ws_client_t* const client)
{
    static char const ping[] = "{\"type\":\"ping\"}";
    uint64_t const now = now_ms();

    if ((client->config.ping_interval_ms == 0U)
        || ((now - client->last_tx_ms) < (uint64_t)client->config.ping_interval_ms))
    {
        return;
    }

    (void)send_frame(client, STT_WS_OPCODE_TEXT, ping, sizeof(ping) - 1U);
}

/** @brief One-time construction of the shared client from the environment. */
static void shared_init(void)
{
    stt_ws_client_config_t config;

    if (stt_ws_client_default_config(&config) != 0)
    {
        return;
    }
    shared_ready = (stt_ws_client_init(&shared_instance, &config) == 0) ? 1U : 0U;
}

// === Public function implementation ============================================================================== //

/**
 * @brief The one client instance shared by the audio sender and by `SttAO`.
 * @return The shared client, or NULL when configuration failed.
 */
stt_ws_client_t* stt_ws_client_shared(void)
{
    (void)pthread_once(&shared_once, shared_init);

    return (shared_ready != 0U) ? &shared_instance : NULL;
}

/**
 * @brief Parse a `ws://` or `wss://` URL into host, port and path.
 * @param url URL text.
 * @param config Destination.
 * @return 0 on success, or -EINVAL when malformed.
 */
int stt_ws_client_parse_url(char const* const url, stt_ws_client_config_t* const config)
{
    char const* cursor;
    char const* host_end;
    char const* path_start;
    size_t host_len;
    uint8_t use_tls;
    uint32_t port;

    if ((url == NULL) || (config == NULL))
    {
        return -EINVAL;
    }

    if (strncmp(url, "wss://", 6U) == 0)
    {
        use_tls = 1U;
        port = 443U;
        cursor = &url[6];
    }
    else if (strncmp(url, "ws://", 5U) == 0)
    {
        use_tls = 0U;
        port = 80U;
        cursor = &url[5];
    }
    else
    {
        return -EINVAL;
    }

    path_start = strchr(cursor, '/');
    host_end = (path_start != NULL) ? path_start : (cursor + strlen(cursor));

    {
        char const* const colon = memchr(cursor, ':', (size_t)(host_end - cursor));

        if (colon != NULL)
        {
            if (number_parse_u32(colon + 1, (size_t)(host_end - (colon + 1)), 1U, STT_WS_PORT_MAX,
                                 &port)
                != 0)
            {
                return -EINVAL;
            }
            host_len = (size_t)(colon - cursor);
        }
        else
        {
            host_len = (size_t)(host_end - cursor);
        }
    }

    if ((host_len == 0U) || (host_len >= sizeof(config->host)))
    {
        return -EINVAL;
    }

    memcpy(config->host, cursor, host_len);
    config->host[host_len] = '\0';
    config->port = (uint16_t)port;
    config->use_tls = use_tls;
    snprintf(config->path, sizeof(config->path), "%s",
             (path_start != NULL) ? path_start : "/");

    return 0;
}

/**
 * @brief Fill a configuration from defaults and environment overrides.
 * @param config Destination configuration.
 * @return 0 on success, or -EINVAL when the URL is missing or unparsable.
 */
int stt_ws_client_default_config(stt_ws_client_config_t* const config)
{
    if (config == NULL)
    {
        return -EINVAL;
    }

    memset(config, 0, sizeof(*config));
    snprintf(config->ca_file, sizeof(config->ca_file), "%s", STT_WS_DEFAULT_CA_FILE);
    config->connect_timeout_ms = STT_WS_DEFAULT_CONNECT_TIMEOUT_MS;
    config->handshake_timeout_ms = STT_WS_DEFAULT_HANDSHAKE_TIMEOUT_MS;
    config->idle_timeout_ms = STT_WS_DEFAULT_IDLE_TIMEOUT_MS;
    config->ping_interval_ms = STT_WS_DEFAULT_PING_INTERVAL_MS;
    config->backoff_min_ms = STT_WS_DEFAULT_BACKOFF_MIN_MS;
    config->backoff_max_ms = STT_WS_DEFAULT_BACKOFF_MAX_MS;
    config->min_epoch = STT_WS_DEFAULT_MIN_EPOCH;

    config->session.sample_rate_hz = STT_WS_DEFAULT_SAMPLE_RATE_HZ;
    config->session.channels = 1U;
    config->session.format = STT_SESSION_FORMAT_S16_LE;
    config->session.chunk_ms = STT_WS_DEFAULT_CHUNK_MS;
    config->session.samples_per_chunk =
        (STT_WS_DEFAULT_SAMPLE_RATE_HZ * STT_WS_DEFAULT_CHUNK_MS) / 1000U;
    config->session.bytes_per_chunk = config->session.samples_per_chunk * 2U;
    config->session.latency_ms = STT_WS_DEFAULT_LATENCY_MS;
    config->session.stop_history_eou_ms = STT_WS_DEFAULT_STOP_HISTORY_MS;
    config->session.residue_tokens_at_end = STT_WS_DEFAULT_RESIDUE_TOKENS;
    snprintf(config->session.target_lang, sizeof(config->session.target_lang), "%s",
             STT_WS_DEFAULT_TARGET_LANG);

    env_string(config->url, sizeof(config->url), "SUBTITLE_STT_WS_URL");
    env_string(config->ca_file, sizeof(config->ca_file), "SUBTITLE_STT_WS_CA_FILE");
    env_string(config->ca_dir, sizeof(config->ca_dir), "SUBTITLE_STT_WS_CA_DIR");
    env_string(config->session.target_lang, sizeof(config->session.target_lang),
               "SUBTITLE_STT_NEMOTRON_TARGET_LANG");

    config->connect_timeout_ms =
        env_u32("SUBTITLE_STT_WS_CONNECT_TIMEOUT_MS", config->connect_timeout_ms, 100U, 60000U);
    config->handshake_timeout_ms =
        env_u32("SUBTITLE_STT_WS_HANDSHAKE_TIMEOUT_MS", config->handshake_timeout_ms, 100U, 60000U);
    config->idle_timeout_ms =
        env_u32("SUBTITLE_STT_WS_IDLE_TIMEOUT_MS", config->idle_timeout_ms, 1000U, 600000U);
    config->ping_interval_ms =
        env_u32("SUBTITLE_STT_WS_PING_MS", config->ping_interval_ms, 0U, 600000U);
    config->backoff_min_ms =
        env_u32("SUBTITLE_STT_WS_BACKOFF_MIN_MS", config->backoff_min_ms, 100U, 60000U);
    config->backoff_max_ms =
        env_u32("SUBTITLE_STT_WS_BACKOFF_MAX_MS", config->backoff_max_ms, 1000U, 600000U);
    config->min_epoch = (int64_t)env_u32("SUBTITLE_STT_MIN_EPOCH",
                                         (uint32_t)STT_WS_DEFAULT_MIN_EPOCH, 0U, UINT32_MAX);
    config->session.latency_ms =
        env_u32("SUBTITLE_STT_NEMOTRON_LATENCY_MS", config->session.latency_ms, 0U, 10000U);
    config->session.stop_history_eou_ms = env_u32("SUBTITLE_STT_NEMOTRON_STOP_HISTORY_EOU_MS",
                                                  config->session.stop_history_eou_ms, 0U, 10000U);
    config->session.residue_tokens_at_end =
        env_u32("SUBTITLE_STT_NEMOTRON_RESIDUE_TOKENS_AT_END",
                config->session.residue_tokens_at_end, 0U, 100U);

    if (config->backoff_max_ms < config->backoff_min_ms)
    {
        config->backoff_max_ms = config->backoff_min_ms;
    }

    // No compiled-in endpoint: an unconfigured board must say so, not dial out.
    if (config->url[0] == '\0')
    {
        LOG_ERROR("stt-ws: SUBTITLE_STT_WS_URL is not set; the STT link stays down");
        return -EINVAL;
    }

    return stt_ws_client_parse_url(config->url, config);
}

/**
 * @brief Initialize the client without touching the network.
 * @param client Client instance.
 * @param config Runtime configuration.
 * @return 0 on success, or a negative errno-style value.
 */
int stt_ws_client_init(stt_ws_client_t* const client, stt_ws_client_config_t const* const config)
{
    if ((client == NULL) || (config == NULL) || (config->host[0] == '\0') || (config->port == 0U))
    {
        return -EINVAL;
    }

    memset(client, 0, sizeof(*client));
    client->config = *config;
    client->state = STT_WS_STATE_IDLE;
    if (pthread_mutex_init(&client->lock, NULL) != 0)
    {
        return -EIO;
    }
    if (pthread_cond_init(&client->worker_cond, NULL) != 0)
    {
        (void)pthread_mutex_destroy(&client->lock);
        return -EIO;
    }
    client->session_generation = 1U;
    client->last_event_generation = 1U;
    client->initialized = 1U;

    if (config->use_tls == 0U)
    {
        LOG_WARNING("stt-ws: ws:// selected, audio and transcripts travel unencrypted");
    }
    LOG_INFO("stt-ws: target=%s ca=%s connect=%lums handshake=%lums idle=%lums ping=%lums "
             "backoff=%lu..%lums",
             config->url, (config->ca_file[0] != '\0') ? config->ca_file : "(system)",
             (unsigned long)config->connect_timeout_ms,
             (unsigned long)config->handshake_timeout_ms, (unsigned long)config->idle_timeout_ms,
             (unsigned long)config->ping_interval_ms, (unsigned long)config->backoff_min_ms,
             (unsigned long)config->backoff_max_ms);
    LOG_INFO("stt-ws: audio %luHz mono S16_LE %lums chunks; backend lang=%s lookahead=%lums "
             "eou=%lums residue=%lu",
             (unsigned long)config->session.sample_rate_hz,
             (unsigned long)config->session.chunk_ms, config->session.target_lang,
             (unsigned long)config->session.latency_ms,
             (unsigned long)config->session.stop_history_eou_ms,
             (unsigned long)config->session.residue_tokens_at_end);

    return 0;
}

/** @brief Network owner: service one live session and consume only recent PCM. */
static void* worker_main(void* const arg)
{
    stt_ws_client_t* const client = (stt_ws_client_t*)arg;

    LOG_INFO("stt-ws: network worker started");
    while (client_stop_requested(client) == 0U)
    {
        stt_ws_audio_chunk_t chunk;

        if (stt_ws_client_service(client) != 0)
        {
            worker_wait(client, STT_WS_WORKER_WAIT_MS);
            continue;
        }
        if (audio_pop(client, &chunk) != 0)
        {
            worker_wait(client, STT_WS_WORKER_WAIT_MS);
            continue;
        }

        (void)stt_ws_client_send_audio(client,
                                       chunk.payload,
                                       chunk.size,
                                       chunk.timestamp_ns,
                                       chunk.dropped);
    }

    if (client->conn != NULL)
    {
        static char const session_end[] = "{\"type\":\"session_end\"}";

        // The worker owns the transport, including its best-effort teardown.
        (void)send_frame(client, STT_WS_OPCODE_TEXT, session_end, sizeof(session_end) - 1U);
        (void)send_frame(client, STT_WS_OPCODE_CLOSE, NULL, 0U);
        net_tls_close(client->conn);
        client->conn = NULL;
    }

    pthread_mutex_lock(&client->lock);
    client->state = STT_WS_STATE_IDLE;
    client->worker_done = 1U;
    pthread_cond_broadcast(&client->worker_cond);
    pthread_mutex_unlock(&client->lock);
    LOG_INFO("stt-ws: network worker stopped");
    return NULL;
}

int stt_ws_client_start(stt_ws_client_t* const client)
{
    int status;

    if ((client == NULL) || (client->initialized == 0U))
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&client->lock);
    if (client->worker_started != 0U)
    {
        pthread_mutex_unlock(&client->lock);
        return -EALREADY;
    }
    client->worker_started = 1U;
    client->worker_done = 0U;
    client->stop_requested = 0U;
    pthread_mutex_unlock(&client->lock);

    status = pthread_create(&client->worker_thread, NULL, worker_main, client);
    if (status != 0)
    {
        pthread_mutex_lock(&client->lock);
        client->worker_started = 0U;
        client->worker_done = 1U;
        pthread_mutex_unlock(&client->lock);
        return -EIO;
    }
    return 0;
}

int stt_ws_client_submit_audio(stt_ws_client_t* const client,
                               void const* const pcm,
                               size_t const size,
                               uint64_t const timestamp_ns,
                               uint32_t const dropped)
{
    uint32_t tail;

    if ((client == NULL) || (client->initialized == 0U) || (pcm == NULL) || (size == 0U)
        || (size > STT_WS_AUDIO_MAX_BYTES))
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&client->lock);
    if ((client->worker_started == 0U) || (client->stop_requested != 0U))
    {
        pthread_mutex_unlock(&client->lock);
        return -EAGAIN;
    }
    if (client->audio_count == STT_WS_AUDIO_QUEUE_DEPTH)
    {
        client->audio_head = (uint8_t)((client->audio_head + 1U) % STT_WS_AUDIO_QUEUE_DEPTH);
        client->audio_count--;
        client->audio_dropped_total++;
        client->stats.chunks_dropped_tx++;
    }

    tail = ((uint32_t)client->audio_head + (uint32_t)client->audio_count)
           % STT_WS_AUDIO_QUEUE_DEPTH;
    memcpy(client->audio_queue[tail].payload, pcm, size);
    client->audio_queue[tail].size = size;
    client->audio_queue[tail].timestamp_ns = timestamp_ns;
    // Internal queue drops can happen after this chunk was enqueued, so
    // audio_pop() adds the current internal total immediately before sending.
    client->audio_queue[tail].dropped = dropped;
    client->audio_count++;
    pthread_cond_signal(&client->worker_cond);
    pthread_mutex_unlock(&client->lock);
    return 0;
}

void stt_ws_client_request_stop(stt_ws_client_t* const client)
{
    if ((client == NULL) || (client->initialized == 0U))
    {
        return;
    }

    pthread_mutex_lock(&client->lock);
    client->stop_requested = 1U;
    pthread_cond_broadcast(&client->worker_cond);
    pthread_mutex_unlock(&client->lock);
}

uint8_t stt_ws_client_stop_complete(stt_ws_client_t* const client)
{
    uint8_t complete;

    if ((client == NULL) || (client->initialized == 0U))
    {
        return 1U;
    }
    pthread_mutex_lock(&client->lock);
    complete = ((client->worker_started == 0U) || (client->worker_done != 0U)) ? 1U : 0U;
    pthread_mutex_unlock(&client->lock);
    return complete;
}

int stt_ws_client_finish_stop(stt_ws_client_t* const client)
{
    pthread_t worker;

    if ((client == NULL) || (client->initialized == 0U))
    {
        return -EINVAL;
    }
    pthread_mutex_lock(&client->lock);
    if (client->worker_started == 0U)
    {
        pthread_mutex_unlock(&client->lock);
        return 0;
    }
    if (client->worker_done == 0U)
    {
        pthread_mutex_unlock(&client->lock);
        return -EAGAIN;
    }
    worker = client->worker_thread;
    pthread_mutex_unlock(&client->lock);

    (void)pthread_join(worker, NULL);
    pthread_mutex_lock(&client->lock);
    client->worker_started = 0U;
    pthread_mutex_unlock(&client->lock);
    return 0;
}

/**
 * @brief Advance the connection state machine, without sending audio.
 * @param client Client instance.
 * @return 0 when the session is ready, or a negative errno-style value.
 */
int stt_ws_client_service(stt_ws_client_t* const client)
{
    int status;

    if ((client == NULL) || (client->initialized == 0U))
    {
        return -EINVAL;
    }

    status = ensure_connected(client);
    if (status != 0)
    {
        return status;
    }

    status = pump_rx(client, 0U);
    if (status != 0)
    {
        drop_session(client, "receive path failed");
        return -EAGAIN;
    }

    // A link that stops answering is worse than a closed one: it accepts audio
    // that goes nowhere. Treat prolonged silence as a dead session.
    if ((client->config.idle_timeout_ms != 0U)
        && ((now_ms() - client->last_rx_ms) > (uint64_t)client->config.idle_timeout_ms))
    {
        drop_session(client, "no server traffic within the idle timeout");
        return -EAGAIN;
    }

    if ((now_ms() - client->session_started_ms) > STT_WS_HEALTHY_SESSION_MS)
    {
        pthread_mutex_lock(&client->lock);
        client->backoff_ms = 0U; // a healthy session earns a fast first retry
        pthread_mutex_unlock(&client->lock);
    }

    maybe_ping(client);
    return 0;
}

/**
 * @brief Send one PCM chunk, connecting first when needed.
 * @return 0 when sent, or a negative errno-style value when dropped.
 */
int stt_ws_client_send_audio(stt_ws_client_t* const client,
                             void const* const pcm,
                             size_t size,
                             uint64_t timestamp_ns,
                             uint32_t dropped)
{
    uint8_t payload[STT_WS_AUDIO_HEADER_BYTES + 2048U];
    int status;

    if ((client == NULL) || (client->initialized == 0U) || (pcm == NULL) || (size == 0U)
        || (size > (sizeof(payload) - STT_WS_AUDIO_HEADER_BYTES)))
    {
        return -EINVAL;
    }

    status = stt_ws_client_service(client);
    if (status != 0)
    {
        client_stats_inc(client, &client->stats.chunks_dropped_tx);
        return status;
    }

    // Header layout mirrors protocol.py CHUNK_HEADER "!QQI".
    put_u64_be(&payload[0], (uint64_t)client->audio_seq);
    put_u64_be(&payload[8], timestamp_ns);
    put_u32_be(&payload[16], dropped);
    memcpy(&payload[STT_WS_AUDIO_HEADER_BYTES], pcm, size);

    if (send_frame(client, STT_WS_OPCODE_BINARY, payload, STT_WS_AUDIO_HEADER_BYTES + size) != 0)
    {
        client_stats_inc(client, &client->stats.chunks_dropped_tx);
        drop_session(client, "audio send failed");
        return -EIO;
    }

    pthread_mutex_lock(&client->lock);
    client->audio_seq++;
    client->stats.chunks_sent++;
    client->stats.bytes_sent += (uint64_t)size;
    pthread_mutex_unlock(&client->lock);
    return 0;
}

/**
 * @brief Drain buffered transcripts into caller-owned events.
 * @return 0 on success, or a negative errno-style value.
 */
int stt_ws_client_poll_events(stt_ws_client_t* const client,
                              subtitle_text_evt_t* const events,
                              uint32_t max_events,
                              uint32_t* const event_count)
{
    stt_ws_event_t pending[STT_WS_EVENT_RING_DEPTH];
    uint32_t taken = 0U;
    uint32_t i;

    if ((client == NULL) || (client->initialized == 0U) || (events == NULL) || (max_events == 0U)
        || (event_count == NULL))
    {
        return -EINVAL;
    }

    *event_count = 0U;

    // Copy under the lock, parse outside it: the network worker must never wait
    // on JSON parsing, and this runs on the single QP/C thread.
    pthread_mutex_lock(&client->lock);
    while ((client->ring_count > 0U) && (taken < max_events) && (taken < STT_WS_EVENT_RING_DEPTH))
    {
        pending[taken] = client->ring[client->ring_head];
        client->ring_head = (uint8_t)((client->ring_head + 1U) % STT_WS_EVENT_RING_DEPTH);
        client->ring_count--;
        taken++;
    }
    pthread_mutex_unlock(&client->lock);

    for (i = 0U; i < taken; i++)
    {
        subtitle_text_evt_t* const target = &events[*event_count];

        if (stt_transcript_parse_line(pending[i].line, target) != 0)
        {
            client_stats_inc(client, &client->stats.protocol_errors);
            continue;
        }
        pthread_mutex_lock(&client->lock);
        if (pending[i].session_generation != client->session_generation)
        {
            client->stats.events_dropped_ring++;
            pthread_mutex_unlock(&client->lock);
            continue;
        }
        if (client->last_event_generation != pending[i].session_generation)
        {
            client->last_event_generation = pending[i].session_generation;
            client->have_last_event_seq = 0U;
            client->last_event_seq = 0U;
        }
        // Sequence ordering is scoped to one WebSocket session. The generation
        // check above prevents a copied event from a dead session poisoning the
        // new session's seq=0.
        if ((client->have_last_event_seq != 0U) && (target->seq <= client->last_event_seq))
        {
            client->stats.events_rejected_old_seq++;
            {
                uint32_t const last = client->last_event_seq;

                pthread_mutex_unlock(&client->lock);
                LOG_WARNING("stt-ws: rejecting out-of-order transcript seq=%lu last=%lu",
                            (unsigned long)target->seq, (unsigned long)last);
            }
            continue;
        }

        client->last_event_seq = target->seq;
        client->have_last_event_seq = 1U;
        pthread_mutex_unlock(&client->lock);
        (*event_count)++;
    }

    return 0;
}

/**
 * @brief Record what happened to a transcript after `SttAO` forwarded it.
 * @return None.
 */
void stt_ws_client_report_delivery(stt_ws_client_t* const client,
                                   stt_event_rx_delivery_status_t status)
{
    if ((client == NULL) || (client->initialized == 0U))
    {
        return;
    }

    pthread_mutex_lock(&client->lock);
    switch (status)
    {
    case STT_EVENT_RX_DELIVERY_ACCEPTED:
        client->stats.deliveries_accepted++;
        break;
    case STT_EVENT_RX_DELIVERY_DROPPED_EVENT_POOL:
        client->stats.deliveries_dropped_pool++;
        break;
    case STT_EVENT_RX_DELIVERY_DROPPED_SUBTITLE_QUEUE:
        client->stats.deliveries_dropped_queue++;
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&client->lock);
}

/** @brief Copy a consistent snapshot of the counters. */
void stt_ws_client_stats(stt_ws_client_t* const client, stt_ws_client_stats_t* const stats)
{
    if ((client == NULL) || (stats == NULL) || (client->initialized == 0U))
    {
        return;
    }

    pthread_mutex_lock(&client->lock);
    *stats = client->stats;
    pthread_mutex_unlock(&client->lock);
}

/** @brief Current connection state, for logging. */
stt_ws_state_e stt_ws_client_state(stt_ws_client_t* const client)
{
    stt_ws_state_e state = STT_WS_STATE_IDLE;

    if ((client != NULL) && (client->initialized != 0U))
    {
        pthread_mutex_lock(&client->lock);
        state = client->state;
        pthread_mutex_unlock(&client->lock);
    }
    return state;
}

/** @brief Human-readable name of @p state. */
char const* stt_ws_client_state_name(stt_ws_state_e state)
{
    char const* name;

    switch (state)
    {
    case STT_WS_STATE_WAIT_CLOCK:
        name = "wait_clock";
        break;
    case STT_WS_STATE_CONNECTING:
        name = "connecting";
        break;
    case STT_WS_STATE_HANDSHAKING:
        name = "handshaking";
        break;
    case STT_WS_STATE_STARTING:
        name = "starting";
        break;
    case STT_WS_STATE_READY:
        name = "ready";
        break;
    case STT_WS_STATE_BACKOFF:
        name = "backoff";
        break;
    case STT_WS_STATE_IDLE:
    default:
        name = "idle";
        break;
    }

    return name;
}

/**
 * @brief Close the session and release resources.
 * @return None.
 */
void stt_ws_client_cleanup(stt_ws_client_t* const client)
{
    if ((client == NULL) || (client->initialized == 0U))
    {
        return;
    }

    stt_ws_client_request_stop(client);
    if (stt_ws_client_stop_complete(client) == 0U)
    {
        // This legacy cleanup API is synchronous only for callers that own a
        // non-worker client. Production uses request/complete/finish instead.
        return;
    }
    else
    {
        (void)stt_ws_client_finish_stop(client);
    }

    // Direct service/send_audio users (unit tests and the host probe) do not
    // start the worker, so cleanup remains their synchronous owner.
    if (client->conn != NULL)
    {
        static char const session_end[] = "{\"type\":\"session_end\"}";

        // Best effort: a peer that already vanished must not delay shutdown.
        (void)send_frame(client, STT_WS_OPCODE_TEXT, session_end, sizeof(session_end) - 1U);
        (void)send_frame(client, STT_WS_OPCODE_CLOSE, NULL, 0U);
        net_tls_close(client->conn);
        client->conn = NULL;
    }

    client->state = STT_WS_STATE_IDLE;
    client->initialized = 0U;
    pthread_cond_destroy(&client->worker_cond);
    pthread_mutex_destroy(&client->lock);
}

// === End of documentation ======================================================================================== //
