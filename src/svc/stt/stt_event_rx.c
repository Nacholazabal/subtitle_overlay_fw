/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

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
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "errorno.h"
#include "log.h"
#include "number_parse.h"

// === Macros definitions ========================================================================================== //

#define STT_EVENT_RX_LISTEN_BACKLOG (1)
#define STT_EVENT_RX_PORT_STR_LEN   (16U)
#define STT_EVENT_RX_JSON_KEY_MAX   (32U)
#define STT_EVENT_RX_JSON_TOKEN_MAX (64U)
#define STT_EVENT_RX_BOOL_TRUE_LEN  (4U)
#define STT_EVENT_RX_BOOL_FALSE_LEN (5U)
#define STT_EVENT_RX_MS_PER_SEC     (1000.0)
#define STT_EVENT_RX_ROUND_HALF     (0.5)
#define STT_EVENT_RX_TCP_PORT_MAX   (65535U)

#define STT_FIELD_SEQ       (1U << 0U)
#define STT_FIELD_FINAL     (1U << 1U)
#define STT_FIELD_TYPE      (1U << 2U)
#define STT_FIELD_START     (1U << 3U)
#define STT_FIELD_END       (1U << 4U)
#define STT_FIELD_TEXT      (1U << 5U)
#define STT_REQUIRED_FIELDS (STT_FIELD_SEQ | STT_FIELD_START | STT_FIELD_END | STT_FIELD_TEXT)

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static void copy_env_string(char* dst, size_t dst_size, char const* value);
static void skip_whitespace(char const** cursor);
static int json_parse_string(char const** cursor,
                             char* dst,
                             size_t dst_size,
                             uint8_t require_non_empty,
                             uint8_t* truncated);
static int json_scalar_span(char const** cursor, char const** start, size_t* length);
static int json_parse_u32(char const** cursor, uint32_t* value);
static int json_parse_bool(char const** cursor, uint8_t* value);
static int json_parse_double(char const** cursor, double* value);
static int json_skip_value(char const** cursor);
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

/** @brief Advance over JSON whitespace. */
static void skip_whitespace(char const** const cursor)
{
    while (isspace((unsigned char)**cursor))
    {
        (*cursor)++;
    }
}

/**
 * @brief Parse one JSON string, decoding only escapes used by the trusted sender contract.
 * @return 0 on success or -EINVAL for malformed/unsupported escapes.
 */
static int json_parse_string(char const** const cursor,
                             char* const dst,
                             size_t dst_size,
                             uint8_t require_non_empty,
                             uint8_t* const truncated)
{
    size_t out = 0U;
    uint8_t non_empty = 0U;
    uint8_t was_truncated = 0U;

    if ((cursor == NULL) || (*cursor == NULL) || (**cursor != '"')
        || ((dst != NULL) && (dst_size == 0U)))
    {
        return -EINVAL;
    }

    (*cursor)++;
    while ((**cursor != '\0') && (**cursor != '"'))
    {
        unsigned char const raw = (unsigned char)**cursor;
        char ch;

        if (raw < 0x20U)
        {
            return -EINVAL;
        }

        ch = **cursor;
        (*cursor)++;
        non_empty = 1U;
        if (ch == '\\')
        {
            ch = **cursor;
            if (ch == '\0')
            {
                return -EINVAL;
            }
            (*cursor)++;

            switch (ch)
            {
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                ch = ' ';
                break;

            case '"':
            case '\\':
            case '/':
                break;

            default:
                return -EINVAL;
            }
        }

        if (dst != NULL)
        {
            if ((out + 1U) < dst_size)
            {
                dst[out++] = ch;
            }
            else
            {
                was_truncated = 1U;
            }
        }
    }

    if (**cursor != '"')
    {
        return -EINVAL;
    }
    (*cursor)++;

    if (dst != NULL)
    {
        dst[out] = '\0';
    }
    if (truncated != NULL)
    {
        *truncated = was_truncated;
    }

    return ((require_non_empty == 0U) || (non_empty != 0U)) ? 0 : -EINVAL;
}

/** @brief Return and consume a non-string scalar token span. */
static int json_scalar_span(char const** const cursor,
                            char const** const start,
                            size_t* const length)
{
    char const* end;

    if ((cursor == NULL) || (*cursor == NULL) || (start == NULL) || (length == NULL))
    {
        return -EINVAL;
    }

    *start = *cursor;
    end = *cursor;
    while ((*end != '\0') && (*end != ',') && (*end != '}') && (isspace((unsigned char)*end) == 0))
    {
        end++;
    }

    *length = (size_t)(end - *start);
    if (*length == 0U)
    {
        return -EINVAL;
    }

    *cursor = end;
    return 0;
}

/** @brief Parse a uint32 JSON scalar. */
static int json_parse_u32(char const** const cursor, uint32_t* const value)
{
    char const* start;
    size_t length;
    int status = json_scalar_span(cursor, &start, &length);

    return (status == 0) ? number_parse_u32(start, length, 0U, UINT32_MAX, value) : status;
}

/** @brief Parse a boolean JSON scalar, retaining 0/1 compatibility. */
static int json_parse_bool(char const** const cursor, uint8_t* const value)
{
    char const* start;
    size_t length;
    int status = json_scalar_span(cursor, &start, &length);

    if ((status == 0) && (length == STT_EVENT_RX_BOOL_TRUE_LEN)
        && (memcmp(start, "true", length) == 0))
    {
        *value = 1U;
        return 0;
    }
    if ((status == 0) && (length == STT_EVENT_RX_BOOL_FALSE_LEN)
        && (memcmp(start, "false", length) == 0))
    {
        *value = 0U;
        return 0;
    }
    if ((status == 0) && (length == 1U) && ((start[0] == '0') || (start[0] == '1')))
    {
        *value = (start[0] == '1') ? 1U : 0U;
        return 0;
    }

    return -EINVAL;
}

/** @brief Parse a finite floating-point JSON scalar with exact token consumption. */
static int json_parse_double(char const** const cursor, double* const value)
{
    char const* start;
    size_t length;
    char token[STT_EVENT_RX_JSON_TOKEN_MAX];
    char* end = NULL;
    double parsed;
    int status = json_scalar_span(cursor, &start, &length);

    if ((status != 0) || (value == NULL) || (length >= sizeof(token))
        || ((start[0] != '-') && ((start[0] < '0') || (start[0] > '9'))))
    {
        return -EINVAL;
    }

    memcpy(token, start, length);
    token[length] = '\0';
    errno = 0;
    parsed = strtod(token, &end);
    if ((end == token) || (*end != '\0') || (errno == ERANGE) || (isfinite(parsed) == 0))
    {
        return -EINVAL;
    }

    *value = parsed;
    return 0;
}

/** @brief Skip one valid flat-object scalar value. */
static int json_skip_value(char const** const cursor)
{
    char const* start;
    size_t length;
    double ignored;

    if (**cursor == '"')
    {
        return json_parse_string(cursor, NULL, 0U, 0U, NULL);
    }

    if (json_scalar_span(cursor, &start, &length) != 0)
    {
        return -EINVAL;
    }
    if (((length == 4U)
         && ((memcmp(start, "true", length) == 0) || (memcmp(start, "null", length) == 0)))
        || ((length == 5U) && (memcmp(start, "false", length) == 0)))
    {
        return 0;
    }

    *cursor = start;
    return json_parse_double(cursor, &ignored);
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
 * @return 0 on connected/no pending client, or a negative errno-style value on failure.
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
 * @return 0 on success, or a negative errno-style value on invalid input.
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
    uint32_t parsed_port;

    if (config == NULL)
    {
        return;
    }

    memset(config, 0, sizeof(*config));
    snprintf(config->host, sizeof(config->host), "%s", STT_EVENT_RX_DEFAULT_HOST);
    config->port = STT_EVENT_RX_DEFAULT_PORT;

    copy_env_string(config->host, sizeof(config->host), getenv("SUBTITLE_STT_RX_HOST"));

    env_port = getenv("SUBTITLE_STT_RX_PORT");
    if ((env_port != NULL) && (env_port[0] != '\0'))
    {
        if (number_parse_u32(env_port,
                             strlen(env_port),
                             1U,
                             STT_EVENT_RX_TCP_PORT_MAX,
                             &parsed_port)
            == 0)
        {
            config->port = parsed_port;
        }
        else
        {
            LOG_WARNING("stt-rx: ignoring invalid SUBTITLE_STT_RX_PORT='%s'", env_port);
        }
    }
}

/**
 * @brief Initialize the nonblocking TCP NDJSON receiver.
 * @param rx Receiver instance.
 * @param config Receiver configuration.
 * @return 0 on success, or a negative errno-style value on failure.
 */
int stt_event_rx_init(stt_event_rx_t* const rx, stt_event_rx_config_t const* const config)
{
    if ((rx == NULL) || (config == NULL) || (config->host[0] == '\0') || (config->port == 0U)
        || (config->port > STT_EVENT_RX_TCP_PORT_MAX))
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
    LOG_INFO("stt-rx: listening on %s:%lu", rx->config.host, (unsigned long)rx->config.port);
    return 0;
}

/**
 * @brief Poll nonblocking TCP input and parse completed STT events.
 * @param rx Receiver instance.
 * @param events Output event array.
 * @param max_events Output event array capacity.
 * @param event_count Number of parsed events.
 * @return 0 on success, or a negative errno-style value on failure.
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
        return -APP_ESTATE;
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
 * @return 0 on success, or a negative errno-style value on failure.
 */
int stt_event_rx_parse_line(char const* const line, subtitle_text_evt_t* const event)
{
    char const* cursor = line;
    char key[STT_EVENT_RX_JSON_KEY_MAX];
    char type[16];
    double start_sec = 0.0;
    double end_sec = 0.0;
    uint32_t seen = 0U;
    uint8_t final_value = 0U;
    uint8_t type_value = 0U;
    uint8_t key_truncated;
    uint8_t text_truncated;
    uint8_t object_done = 0U;

    if ((line == NULL) || (event == NULL))
    {
        return -EINVAL;
    }

    memset(event, 0, sizeof(*event));
    type[0] = '\0';

    skip_whitespace(&cursor);
    if (*cursor != '{')
    {
        return -EINVAL;
    }
    cursor++;

    while (object_done == 0U)
    {
        uint32_t field = 0U;
        int status;

        skip_whitespace(&cursor);
        if (*cursor == '}')
        {
            cursor++;
            break;
        }

        key_truncated = 0U;
        status = json_parse_string(&cursor, key, sizeof(key), 0U, &key_truncated);
        if (status != 0)
        {
            return status;
        }

        skip_whitespace(&cursor);
        if (*cursor != ':')
        {
            return -EINVAL;
        }
        cursor++;
        skip_whitespace(&cursor);

        if (key_truncated == 0U)
        {
            if (strcmp(key, "seq") == 0)
            {
                field = STT_FIELD_SEQ;
            }
            else if (strcmp(key, "is_final") == 0)
            {
                field = STT_FIELD_FINAL;
            }
            else if (strcmp(key, "type") == 0)
            {
                field = STT_FIELD_TYPE;
            }
            else if (strcmp(key, "start_sec") == 0)
            {
                field = STT_FIELD_START;
            }
            else if (strcmp(key, "end_sec") == 0)
            {
                field = STT_FIELD_END;
            }
            else if (strcmp(key, "text") == 0)
            {
                field = STT_FIELD_TEXT;
            }
        }

        if ((field != 0U) && ((seen & field) != 0U))
        {
            return -EINVAL;
        }

        switch (field)
        {
        case STT_FIELD_SEQ:
            status = json_parse_u32(&cursor, &event->seq);
            break;

        case STT_FIELD_FINAL:
            status = json_parse_bool(&cursor, &final_value);
            break;

        case STT_FIELD_TYPE:
            status = json_parse_string(&cursor, type, sizeof(type), 1U, NULL);
            if (status == 0)
            {
                if (strcmp(type, "final") == 0)
                {
                    type_value = 1U;
                }
                else if (strcmp(type, "partial") == 0)
                {
                    type_value = 0U;
                }
                else
                {
                    status = -EINVAL;
                }
            }
            break;

        case STT_FIELD_START:
            status = json_parse_double(&cursor, &start_sec);
            break;

        case STT_FIELD_END:
            status = json_parse_double(&cursor, &end_sec);
            break;

        case STT_FIELD_TEXT:
            text_truncated = 0U;
            status =
                json_parse_string(&cursor, event->text, sizeof(event->text), 1U, &text_truncated);
            if ((status == 0) && (text_truncated != 0U))
            {
                LOG_WARNING("stt-rx: truncated JSON text field");
            }
            break;

        default:
            status = json_skip_value(&cursor);
            break;
        }

        if (status != 0)
        {
            return status;
        }
        seen |= field;

        skip_whitespace(&cursor);
        if (*cursor == ',')
        {
            cursor++;
            skip_whitespace(&cursor);
            if (*cursor == '}')
            {
                return -EINVAL;
            }
        }
        else if (*cursor == '}')
        {
            cursor++;
            object_done = 1U;
        }
        else
        {
            return -EINVAL;
        }
    }

    skip_whitespace(&cursor);
    if ((*cursor != '\0') || ((seen & STT_REQUIRED_FIELDS) != STT_REQUIRED_FIELDS)
        || ((seen & (STT_FIELD_FINAL | STT_FIELD_TYPE)) == 0U))
    {
        return -EINVAL;
    }

    if (((seen & STT_FIELD_FINAL) != 0U) && ((seen & STT_FIELD_TYPE) != 0U)
        && (final_value != type_value))
    {
        return -EINVAL;
    }

    event->is_final = ((seen & STT_FIELD_FINAL) != 0U) ? final_value : type_value;

    if ((start_sec < 0.0) || (end_sec < start_sec)
        || (start_sec > (((double)UINT32_MAX - STT_EVENT_RX_ROUND_HALF) / STT_EVENT_RX_MS_PER_SEC))
        || (end_sec > (((double)UINT32_MAX - STT_EVENT_RX_ROUND_HALF) / STT_EVENT_RX_MS_PER_SEC)))
    {
        return -EINVAL;
    }

    event->start_ms = (uint32_t)((start_sec * STT_EVENT_RX_MS_PER_SEC) + STT_EVENT_RX_ROUND_HALF);
    event->end_ms = (uint32_t)((end_sec * STT_EVENT_RX_MS_PER_SEC) + STT_EVENT_RX_ROUND_HALF);
    return 0;
}

// === End of documentation ======================================================================================== //
