///
/// @file ws_client_probe.c
/// @brief Host harness that drives the real WebSocket client against a server
///
/// Builds the production `stt_ws_client` with the production `net_tls` on the
/// host and streams synthetic PCM at it. Everything the board runs is exercised
/// except ALSA capture and the QP/C wiring: URL parsing, TLS or plaintext
/// transport, the RFC 6455 handshake, `session_start`, audio framing, transcript
/// decoding and the sequence guard.
///
/// Usage: ws_client_probe <ws-url> <chunks>
///

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"
#include "stt_ws_client.h"

#define PROBE_CHUNK_BYTES (1920U)
#define PROBE_CHUNK_NS    (20000000ULL)

static void probe_log(log_level_e severity, char const* msg)
{
    fprintf(stderr, "[%s] %s\n", log_level_to_str(severity), msg);
    fflush(stderr);
}

/// @brief Fill one chunk with a low-amplitude tone so it is not pure silence.
static void fill_tone(int16_t* const samples, size_t count, uint32_t phase)
{
    size_t i;

    for (i = 0U; i < count; i++)
    {
        samples[i] = (int16_t)((((phase + (uint32_t)i) % 100U) * 200U) - 10000U);
    }
}

int main(int argc, char** argv)
{
    stt_ws_client_config_t config;
    stt_ws_client_t client;
    stt_ws_client_stats_t stats;
    subtitle_text_evt_t events[8];
    uint8_t chunk[PROBE_CHUNK_BYTES];
    unsigned long chunks;
    unsigned long transcripts = 0U;
    unsigned long i;

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <ws-url> <chunks>\n", argv[0]);
        return 2;
    }
    chunks = strtoul(argv[2], NULL, 10);

    log_init();
    (void)log_subscribe(probe_log, LOG_LEVEL_INFO);

    setenv("SUBTITLE_STT_WS_URL", argv[1], 1);
    if (stt_ws_client_default_config(&config) != 0)
    {
        fprintf(stderr, "probe: configuration failed\n");
        return 1;
    }
    // Keep the probe brisk: it is talking to a local server.
    config.connect_timeout_ms = 3000U;
    config.handshake_timeout_ms = 3000U;
    config.backoff_min_ms = 100U;
    config.backoff_max_ms = 1000U;

    if (stt_ws_client_init(&client, &config) != 0)
    {
        fprintf(stderr, "probe: init failed\n");
        return 1;
    }

    for (i = 0U; i < chunks; i++)
    {
        struct timespec const pace = {0, 20000000L}; // 20 ms, like real capture
        uint32_t count = 0U;
        uint32_t k;

        fill_tone((int16_t*)chunk, sizeof(chunk) / sizeof(int16_t), (uint32_t)i);
        (void)stt_ws_client_send_audio(&client, chunk, sizeof(chunk),
                                       (uint64_t)i * PROBE_CHUNK_NS, 0U);

        if (stt_ws_client_poll_events(&client, events, 8U, &count) == 0)
        {
            for (k = 0U; k < count; k++)
            {
                printf("transcript seq=%lu final=%u start_ms=%lu end_ms=%lu text=%s\n",
                       (unsigned long)events[k].seq,
                       (unsigned)events[k].is_final,
                       (unsigned long)events[k].start_ms,
                       (unsigned long)events[k].end_ms,
                       events[k].text);
                transcripts++;
            }
        }

        nanosleep(&pace, NULL);
    }

    stt_ws_client_stats(&client, &stats);
    printf("{\"chunks_sent\":%lu,\"chunks_dropped\":%lu,\"sessions\":%lu,\"reconnects\":%lu,"
           "\"transcripts_received\":%lu,\"transcripts_delivered\":%lu,\"protocol_errors\":%lu,"
           "\"stale_events\":%lu,\"state\":\"%s\"}\n",
           (unsigned long)stats.chunks_sent,
           (unsigned long)stats.chunks_dropped_tx,
           (unsigned long)stats.sessions,
           (unsigned long)stats.reconnects,
           (unsigned long)stats.transcripts_received,
           transcripts,
           (unsigned long)stats.protocol_errors,
           (unsigned long)stats.events_rejected_old_seq,
           stt_ws_client_state_name(stt_ws_client_state(&client)));

    stt_ws_client_cleanup(&client);
    return 0;
}
