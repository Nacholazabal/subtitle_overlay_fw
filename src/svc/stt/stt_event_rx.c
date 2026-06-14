/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file stt_event_rx.c
/// @brief Nonblocking TCP NDJSON receiver for STT transcript events
///

// === Headers files inclusions ==================================================================================== //

#include "stt_event_rx.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "errorno.h"
#include "log.h"

// === Macros definitions ========================================================================================== //

#define STT_EVENT_RX_LISTEN_BACKLOG  (1)
#define STT_EVENT_RX_PORT_STR_LEN    (16U)
#define STT_EVENT_RX_JSON_KEY_MAX    (32U)
#define STT_EVENT_RX_BOOL_TRUE_LEN   (4U)
#define STT_EVENT_RX_BOOL_FALSE_LEN  (5U)
#define STT_EVENT_RX_MS_PER_SEC      (1000.0)
#define STT_EVENT_RX_ROUND_HALF      (0.5)

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static void copy_env_string(char* dst, size_t dst_size, char const* value);
static char const* json_value(char const* line, char const* key);
static int json_get_u32(char const* line, char const* key, uint32_t* value);
static int json_get_bool(char const* line, char const* key, uint8_t* value);
static int json_get_double(char const* line, char const* key, double* value);
static int json_get_string(char const* line, char const* key, char* dst, size_t dst_size);
static int set_nonblocking(int fd);
static int bind_server(char const* host, uint32_t port);
static void close_fd(int* fd);
static int accept_client(stt_event_rx_t* rx);
static int process_byte(stt_event_rx_t* rx,
                        char ch,
                        subtitle_text_evt_t* events,
                        uint32_t max_events,
                        uint32_t* event_count);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Copy a non-empty environment string into a fixed-size destination.
 * @param dst Destination string.
 * @param dst_size Destination capacity.
 * @param value Optional source string.
 * @return None.
 */
static void copy_env_string(char* const dst, size_t dst_size, char const* const value)
{
    if ((value != NULL) && (value[0] != '\0'))
    {
        snprintf(dst, dst_size, "%s", value);
    }
}

/**
 * @brief Return the JSON value cursor for a top-level key in a simple object.
 * @param line NDJSON line.
 * @param key Key to find.
 * @return Cursor after the key colon, or NULL when missing.
 */
static char const* json_value(char const* const line, char const* const key)
{
    char pattern[STT_EVENT_RX_JSON_KEY_MAX];
    char const* cursor;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(line, pattern);
    if (cursor == NULL)
    {
        return NULL;
    }

    cursor = strchr(cursor + strlen(pattern), ':');
    if (cursor == NULL)
    {
        return NULL;
    }

    cursor++;
    while (isspace((unsigned char)*cursor))
    {
        cursor++;
    }

    return cursor;
}

/**
 * @brief Parse an unsigned 32-bit integer JSON field.
 * @param line NDJSON line.
 * @param key Key to parse.
 * @param value Parsed value destination.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int json_get_u32(char const* const line, char const* const key, uint32_t* const value)
{
    char const* const cursor = json_value(line, key);
    char* end = NULL;
    unsigned long parsed;

    if ((cursor == NULL) || (value == NULL))
    {
        return -EINVAL;
    }

    parsed = strtoul(cursor, &end, 10);
    if (end == cursor)
    {
        return -EINVAL;
    }

    *value = (uint32_t)parsed;
    return 0;
}

/**
 * @brief Parse a boolean JSON field.
 * @param line NDJSON line.
 * @param key Key to parse.
 * @param value Parsed value destination.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int json_get_bool(char const* const line, char const* const key, uint8_t* const value)
{
    char const* const cursor = json_value(line, key);

    if ((cursor == NULL) || (value == NULL))
    {
        return -EINVAL;
    }

    if (strncmp(cursor, "true", STT_EVENT_RX_BOOL_TRUE_LEN) == 0)
    {
        *value = 1U;
        return 0;
    }

    if (strncmp(cursor, "false", STT_EVENT_RX_BOOL_FALSE_LEN) == 0)
    {
        *value = 0U;
        return 0;
    }

    if (*cursor == '1')
    {
        *value = 1U;
        return 0;
    }

    if (*cursor == '0')
    {
        *value = 0U;
        return 0;
    }

    return -EINVAL;
}

/**
 * @brief Parse a floating-point JSON field.
 * @param line NDJSON line.
 * @param key Key to parse.
 * @param value Parsed value destination.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int json_get_double(char const* const line, char const* const key, double* const value)
{
    char const* const cursor = json_value(line, key);
    char* end = NULL;
    double parsed;

    if ((cursor == NULL) || (value == NULL))
    {
        return -EINVAL;
    }

    parsed = strtod(cursor, &end);
    if (end == cursor)
    {
        return -EINVAL;
    }

    *value = parsed;
    return 0;
}

/**
 * @brief Parse a simple JSON string field with minimal escape handling.
 * @param line NDJSON line.
 * @param key Key to parse.
 * @param dst Destination string.
 * @param dst_size Destination capacity.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
static int json_get_string(char const* const line,
                           char const* const key,
                           char* const dst,
                           size_t dst_size)
{
    char const* cursor = json_value(line, key);
    size_t out = 0U;
    uint8_t original_non_empty = 0U;
    uint8_t truncated = 0U;

    if ((cursor == NULL) || (dst == NULL) || (dst_size == 0U) || (*cursor != '"'))
    {
        return -EINVAL;
    }

    cursor++;
    while ((*cursor != '\0') && (*cursor != '"'))
    {
        char ch = *cursor++;

        original_non_empty = 1U;
        if (ch == '\\')
        {
            ch = *cursor++;
            switch (ch)
            {
            case 'n':
            case 't':
                ch = ' ';
                break;

            case '"':
            case '\\':
            case '/':
                break;

            default:
                ch = ' ';
                break;
            }
        }

        if ((out + 1U) >= dst_size)
        {
            truncated = 1U;
            continue;
        }

        dst[out++] = ch;
    }

    if (*cursor != '"')
    {
        return -EINVAL;
    }

    dst[out] = '\0';
    if (truncated != 0U)
    {
        LOG_WARNING("stt-rx: truncated JSON string field '%s'", key);
    }

    return (original_non_empty != 0U) ? 0 : -EINVAL;
}

/**
 * @brief Configure one socket file descriptor as nonblocking.
 * @param fd Socket fd.
 * @return 0 on success, or -EIO on failure.
 */
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
 * @brief Bind and listen on the configured TCP server endpoint.
 * @param host Host/interface string.
 * @param port TCP port.
 * @return Socket fd on success, or -EIO on failure.
 */
static int bind_server(char const* const host, uint32_t port)
{
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    struct addrinfo* it;
    char port_str[STT_EVENT_RX_PORT_STR_LEN];
    int fd = -1;
    int opt = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    snprintf(port_str, sizeof(port_str), "%lu", (unsigned long)port);
    if (getaddrinfo(host, port_str, &hints, &result) != 0)
    {
        return -EIO;
    }

    for (it = result; it != NULL; it = it->ai_next)
    {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0)
        {
            continue;
        }

        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if ((set_nonblocking(fd) == 0) && (bind(fd, it->ai_addr, it->ai_addrlen) == 0)
            && (listen(fd, STT_EVENT_RX_LISTEN_BACKLOG) == 0))
        {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return (fd >= 0) ? fd : -EIO;
}

/**
 * @brief Close a socket fd if it is open.
 * @param fd File descriptor pointer.
 * @return None.
 */
static void close_fd(int* const fd)
{
    if ((fd != NULL) && (*fd >= 0))
    {
        close(*fd);
        *fd = -1;
    }
}

/**
 * @brief Accept one pending STT client connection if available.
 * @param rx Receiver instance.
 * @return 0 on connected/no pending client, or a negative errorno_e value on failure.
 */
static int accept_client(stt_event_rx_t* const rx)
{
    int fd;

    if (rx->client_fd >= 0)
    {
        return 0;
    }

    fd = accept(rx->server_fd, NULL, NULL);
    if (fd < 0)
    {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
        {
            return 0;
        }
        return -EIO;
    }

    if (set_nonblocking(fd) != 0)
    {
        close(fd);
        return -EIO;
    }

    rx->client_fd = fd;
    rx->client_connected = 1U;
    rx->line_used = 0U;
    rx->discarding_line = 0U;
    LOG_INFO("stt-rx: client connected");
    return 0;
}

/**
 * @brief Process one received byte into line state and parsed events.
 * @param rx Receiver instance.
 * @param ch Received byte.
 * @param events Event output array.
 * @param max_events Event output array capacity.
 * @param event_count Current event count, updated on parsed events.
 * @return 0 on success, or a negative errorno_e value on invalid input.
 */
static int process_byte(stt_event_rx_t* const rx,
                        char ch,
                        subtitle_text_evt_t* const events,
                        uint32_t max_events,
                        uint32_t* const event_count)
{
    int status;

    if (ch == '\r')
    {
        return 0;
    }

    if (rx->discarding_line != 0U)
    {
        if (ch == '\n')
        {
            rx->line_used = 0U;
            rx->discarding_line = 0U;
            LOG_WARNING("stt-rx: discarded oversized line");
        }
        return 0;
    }

    if (ch != '\n')
    {
        if ((rx->line_used + 1U) >= sizeof(rx->line))
        {
            rx->line_used = 0U;
            rx->discarding_line = 1U;
            return 0;
        }

        rx->line[rx->line_used++] = ch;
        return 0;
    }

    rx->line[rx->line_used] = '\0';
    rx->line_used = 0U;

    if (*event_count >= max_events)
    {
        return 0;
    }

    status = stt_event_rx_parse_line(rx->line, &events[*event_count]);
    if (status == 0)
    {
        (*event_count)++;
    }
    else
    {
        LOG_WARNING("stt-rx: invalid transcript event");
    }

    return 0;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Fill a receiver configuration from defaults and environment variables.
 * @param config Configuration to initialize.
 * @return None.
 */
void stt_event_rx_default_config(stt_event_rx_config_t* const config)
{
    char const* env_port;

    memset(config, 0, sizeof(*config));
    snprintf(config->host, sizeof(config->host), "%s", STT_EVENT_RX_DEFAULT_HOST);
    config->port = STT_EVENT_RX_DEFAULT_PORT;

    copy_env_string(config->host, sizeof(config->host), getenv("SUBTITLE_STT_RX_HOST"));

    env_port = getenv("SUBTITLE_STT_RX_PORT");
    if ((env_port != NULL) && (env_port[0] != '\0'))
    {
        config->port = (uint32_t)strtoul(env_port, NULL, 10);
    }
}

/**
 * @brief Initialize the nonblocking TCP NDJSON receiver.
 * @param rx Receiver instance.
 * @param config Receiver configuration.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int stt_event_rx_init(stt_event_rx_t* const rx, stt_event_rx_config_t const* const config)
{
    if ((rx == NULL) || (config == NULL) || (config->host[0] == '\0') || (config->port == 0U))
    {
        return -EINVAL;
    }

    memset(rx, 0, sizeof(*rx));
    rx->config = *config;
    rx->server_fd = -1;
    rx->client_fd = -1;

    rx->server_fd = bind_server(rx->config.host, rx->config.port);
    if (rx->server_fd < 0)
    {
        return -EIO;
    }

    rx->initialized = 1U;
    LOG_INFO("stt-rx: listening on %s:%lu",
             rx->config.host,
             (unsigned long)rx->config.port);
    return 0;
}

/**
 * @brief Poll nonblocking TCP input and parse completed STT events.
 * @param rx Receiver instance.
 * @param events Output event array.
 * @param max_events Output event array capacity.
 * @param event_count Number of parsed events.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int stt_event_rx_poll(stt_event_rx_t* const rx,
                      subtitle_text_evt_t* const events,
                      uint32_t max_events,
                      uint32_t* const event_count)
{
    uint32_t bytes_processed = 0U;
    int status;

    if ((rx == NULL) || (events == NULL) || (max_events == 0U) || (event_count == NULL))
    {
        return -EINVAL;
    }

    if (rx->initialized == 0U)
    {
        return -ESTATE;
    }

    *event_count = 0U;

    status = accept_client(rx);
    if (status != 0)
    {
        return status;
    }

    while ((rx->client_fd >= 0) && (bytes_processed < STT_EVENT_RX_MAX_BYTES_PER_POLL)
           && (*event_count < max_events))
    {
        char ch;
        ssize_t const received = recv(rx->client_fd, &ch, sizeof(ch), 0);

        if (received > 0)
        {
            bytes_processed++;
            status = process_byte(rx, ch, events, max_events, event_count);
            if (status != 0)
            {
                return status;
            }
            continue;
        }

        if (received == 0)
        {
            LOG_INFO("stt-rx: client disconnected");
            close_fd(&rx->client_fd);
            rx->client_connected = 0U;
            rx->line_used = 0U;
            rx->discarding_line = 0U;
            break;
        }

        if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
        {
            break;
        }

        LOG_WARNING("stt-rx: recv failed");
        close_fd(&rx->client_fd);
        rx->client_connected = 0U;
        rx->line_used = 0U;
        rx->discarding_line = 0U;
        break;
    }

    return 0;
}

/**
 * @brief Clean up receiver sockets and state.
 * @param rx Receiver instance.
 * @return None.
 */
void stt_event_rx_cleanup(stt_event_rx_t* const rx)
{
    if (rx == NULL)
    {
        return;
    }

    close_fd(&rx->client_fd);
    close_fd(&rx->server_fd);
    memset(rx, 0, sizeof(*rx));
}

/**
 * @brief Parse one STT NDJSON line into a subtitle text event payload.
 * @param line JSON line.
 * @param event Parsed payload destination.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int stt_event_rx_parse_line(char const* const line, subtitle_text_evt_t* const event)
{
    double start_sec = 0.0;
    double end_sec = 0.0;
    int status;

    if ((line == NULL) || (event == NULL))
    {
        return -EINVAL;
    }

    memset(event, 0, sizeof(*event));

    status = json_get_u32(line, "seq", &event->seq);
    if (status != 0)
    {
        return status;
    }

    status = json_get_bool(line, "is_final", &event->is_final);
    if (status != 0)
    {
        char type[16];
        status = json_get_string(line, "type", type, sizeof(type));
        if (status != 0)
        {
            return status;
        }
        event->is_final = (strcmp(type, "final") == 0) ? 1U : 0U;
    }

    status = json_get_double(line, "start_sec", &start_sec);
    if (status != 0)
    {
        return status;
    }

    status = json_get_double(line, "end_sec", &end_sec);
    if (status != 0)
    {
        return status;
    }

    if ((start_sec < 0.0) || (end_sec < start_sec))
    {
        return -EINVAL;
    }

    event->start_ms = (uint32_t)((start_sec * STT_EVENT_RX_MS_PER_SEC) + STT_EVENT_RX_ROUND_HALF);
    event->end_ms = (uint32_t)((end_sec * STT_EVENT_RX_MS_PER_SEC) + STT_EVENT_RX_ROUND_HALF);

    return json_get_string(line, "text", event->text, sizeof(event->text));
}

// === End of documentation ======================================================================================== //
