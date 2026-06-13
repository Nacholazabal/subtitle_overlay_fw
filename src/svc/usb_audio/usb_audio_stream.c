/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file usb_audio_stream.c
/// @brief USB audio capture and TCP streaming service implementation
///

// === Headers files inclusions ==================================================================================== //

#include "usb_audio_stream.h"

#include "errorno.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

#include <errno.h>

// === Macros definitions ========================================================================================== //

#define USB_AUDIO_STREAM_FORMAT_S16_LE      (1U)
#define USB_AUDIO_STREAM_CONNECT_RETRY_SEC  (1U)
#define USB_AUDIO_STREAM_POP_WAIT_NS        (200000000L)
#define USB_AUDIO_STREAM_HEADER_MAGIC       "SAUDPCM"
#define USB_AUDIO_STREAM_HEADER_MAGIC_BYTES (8U)
#define USB_AUDIO_STREAM_HEADER_WORDS       (6U)
#define USB_AUDIO_STREAM_CHUNK_HEADER_WORDS (5U)

// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //

static uint64_t now_ns(void);
static uint64_t htonll(uint64_t value);
static void queue_init(usb_audio_stream_queue_t* queue);
static void queue_cleanup(usb_audio_stream_queue_t* queue);
static void queue_push_drop_oldest(usb_audio_stream_queue_t* queue,
                                   usb_audio_stream_chunk_t const* chunk,
                                   uint32_t* dropped_total);
static int queue_pop_wait(usb_audio_stream_queue_t* queue,
                          usb_audio_stream_chunk_t* chunk,
                          uint8_t* stop_requested);
static int connect_tcp(char const* host, uint32_t port);
static int send_all(int fd, void const* data, size_t size);
static int send_stream_header(int fd);
static int send_chunk(int fd, usb_audio_stream_chunk_t const* chunk, uint32_t dropped);
static void close_sender_fd(usb_audio_stream_t* stream);
static void* capture_thread_main(void* arg);
static void* sender_thread_main(void* arg);
static void copy_env_string(char* dst, size_t dst_size, char const* value);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Return a monotonic timestamp in nanoseconds.
 * @param None.
 * @return Monotonic time in nanoseconds.
 */
static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/**
 * @brief Convert a 64-bit value to network byte order.
 * @param value Host-order value.
 * @return Network-order value.
 */
static uint64_t htonll(uint64_t value)
{
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return value;
#else
    uint32_t const hi = htonl((uint32_t)(value >> 32U));
    uint32_t const lo = htonl((uint32_t)(value & 0xFFFFFFFFULL));

    return ((uint64_t)lo << 32U) | hi;
#endif
}

/**
 * @brief Initialize the chunk queue used between capture and sender threads.
 * @param queue Queue instance.
 * @return None.
 */
static void queue_init(usb_audio_stream_queue_t* const queue)
{
    memset(queue, 0, sizeof(*queue));
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
    queue->initialized = 1U;
}

/**
 * @brief Clean up queue synchronization primitives.
 * @param queue Queue instance.
 * @return None.
 */
static void queue_cleanup(usb_audio_stream_queue_t* const queue)
{
    if ((queue == NULL) || (queue->initialized == 0U))
    {
        return;
    }

    pthread_cond_destroy(&queue->cond);
    pthread_mutex_destroy(&queue->mutex);
    memset(queue, 0, sizeof(*queue));
}

/**
 * @brief Push one audio chunk, dropping the oldest chunk when the queue is full.
 * @param queue Queue instance.
 * @param chunk Chunk to enqueue.
 * @param dropped_total Stream drop counter to update.
 * @return None.
 */
static void queue_push_drop_oldest(usb_audio_stream_queue_t* const queue,
                                   usb_audio_stream_chunk_t const* const chunk,
                                   uint32_t* const dropped_total)
{
    pthread_mutex_lock(&queue->mutex);

    if (queue->count == USB_AUDIO_STREAM_QUEUE_CHUNKS)
    {
        queue->tail = (queue->tail + 1U) % USB_AUDIO_STREAM_QUEUE_CHUNKS;
        queue->count--;
        queue->dropped++;
        (*dropped_total)++;
    }

    queue->chunks[queue->head] = *chunk;
    queue->head = (queue->head + 1U) % USB_AUDIO_STREAM_QUEUE_CHUNKS;
    queue->count++;

    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

/**
 * @brief Pop one chunk, waiting briefly so shutdown can be observed.
 * @param queue Queue instance.
 * @param chunk Destination chunk.
 * @param stop_requested Shared stop flag.
 * @return 0 on success, -EAGAIN when no chunk is available, or -ESTATE on stop.
 */
static int queue_pop_wait(usb_audio_stream_queue_t* const queue,
                          usb_audio_stream_chunk_t* const chunk,
                          uint8_t* const stop_requested)
{
    struct timespec deadline;
    int status = 0;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += USB_AUDIO_STREAM_POP_WAIT_NS;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&queue->mutex);
    while ((queue->count == 0U) && (*stop_requested == 0U) && (status == 0))
    {
        status = pthread_cond_timedwait(&queue->cond, &queue->mutex, &deadline);
    }

    if (*stop_requested != 0U)
    {
        status = -ESTATE;
    }
    else if (queue->count == 0U)
    {
        status = -EAGAIN;
    }
    else
    {
        *chunk = queue->chunks[queue->tail];
        queue->tail = (queue->tail + 1U) % USB_AUDIO_STREAM_QUEUE_CHUNKS;
        queue->count--;
        status = 0;
    }

    pthread_mutex_unlock(&queue->mutex);
    return status;
}

/**
 * @brief Open a TCP connection to the configured receiver.
 * @param host Receiver host or IP string.
 * @param port Receiver TCP port.
 * @return Socket fd on success, or -1 on failure.
 */
static int connect_tcp(char const* const host, uint32_t port)
{
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    struct addrinfo* it;
    char port_str[16];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%lu", (unsigned long)port);
    if (getaddrinfo(host, port_str, &hints, &result) != 0)
    {
        return -1;
    }

    for (it = result; it != NULL; it = it->ai_next)
    {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0)
        {
            continue;
        }

        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0)
        {
            break;
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

/**
 * @brief Send an entire buffer over a socket.
 * @param fd Socket fd.
 * @param data Source buffer.
 * @param size Source buffer length.
 * @return 0 on success, or -EIO on failure.
 */
static int send_all(int fd, void const* const data, size_t size)
{
    uint8_t const* cursor = (uint8_t const*)data;

    while (size > 0U)
    {
        ssize_t const sent = send(fd, cursor, size, MSG_NOSIGNAL);
        if (sent <= 0)
        {
            return -EIO;
        }

        cursor += sent;
        size -= (size_t)sent;
    }

    return 0;
}

/**
 * @brief Send the fixed stream description header.
 * @param fd Socket fd.
 * @return 0 on success, or -EIO on failure.
 */
static int send_stream_header(int fd)
{
    char magic[USB_AUDIO_STREAM_HEADER_MAGIC_BYTES];
    uint32_t words[USB_AUDIO_STREAM_HEADER_WORDS];
    int status;

    memset(magic, 0, sizeof(magic));
    memcpy(magic, USB_AUDIO_STREAM_HEADER_MAGIC, strlen(USB_AUDIO_STREAM_HEADER_MAGIC));
    words[0] = htonl(USB_AUDIO_STREAM_SAMPLE_RATE_HZ);
    words[1] = htonl(USB_AUDIO_STREAM_CHANNELS);
    words[2] = htonl(USB_AUDIO_STREAM_FORMAT_S16_LE);
    words[3] = htonl(USB_AUDIO_STREAM_CHUNK_MS);
    words[4] = htonl(USB_AUDIO_STREAM_SAMPLES_PER_CHUNK);
    words[5] = htonl(USB_AUDIO_STREAM_BYTES_PER_CHUNK);

    status = send_all(fd, magic, sizeof(magic));
    if (status == 0)
    {
        status = send_all(fd, words, sizeof(words));
    }

    return status;
}

/**
 * @brief Send one chunk header plus PCM payload.
 * @param fd Socket fd.
 * @param chunk Audio chunk to send.
 * @param dropped Number of chunks dropped before this chunk.
 * @return 0 on success, or -EIO on failure.
 */
static int send_chunk(int fd, usb_audio_stream_chunk_t const* const chunk, uint32_t dropped)
{
    uint64_t qwords[2];
    uint32_t words[USB_AUDIO_STREAM_CHUNK_HEADER_WORDS];
    int status;

    qwords[0] = htonll((uint64_t)chunk->sequence);
    qwords[1] = htonll(chunk->timestamp_ns);
    words[0] = htonl(chunk->bytes_used);
    words[1] = htonl(dropped);
    words[2] = htonl(USB_AUDIO_STREAM_SAMPLE_RATE_HZ);
    words[3] = htonl(USB_AUDIO_STREAM_CHANNELS);
    words[4] = htonl(USB_AUDIO_STREAM_FORMAT_S16_LE);

    status = send_all(fd, qwords, sizeof(qwords));
    if (status == 0)
    {
        status = send_all(fd, words, sizeof(words));
    }
    if (status == 0)
    {
        status = send_all(fd, chunk->payload, chunk->bytes_used);
    }

    return status;
}

/**
 * @brief Close the current sender socket.
 * @param stream Stream service instance.
 * @return None.
 */
static void close_sender_fd(usb_audio_stream_t* const stream)
{
    if (stream->sender_fd >= 0)
    {
        close(stream->sender_fd);
        stream->sender_fd = -1;
    }
}

/**
 * @brief Main capture thread: read ALSA chunks and enqueue them.
 * @param arg Stream service instance.
 * @return NULL.
 */
static void* capture_thread_main(void* const arg)
{
    usb_audio_stream_t* const stream = (usb_audio_stream_t*)arg;
    uint8_t first_read_pending = 1U;

    LOG_INFO("usb-audio: capture thread started");

    while (stream->stop_requested == 0U)
    {
        usb_audio_stream_chunk_t chunk;
        size_t bytes_read = 0U;
        int status;

        if (first_read_pending != 0U)
        {
            LOG_INFO("usb-audio: waiting for first ALSA chunk");
        }

        status = usb_audio_capture_read_chunk(&stream->capture,
                                              chunk.payload,
                                              sizeof(chunk.payload),
                                              &bytes_read);

        if (status != 0)
        {
            if (status == -EAGAIN)
            {
                continue;
            }

            LOG_ERROR("usb-audio: capture read failed, code=%ld", (long)status);
            stream->stop_requested = 1U;
            pthread_cond_broadcast(&stream->queue.cond);
            break;
        }

        chunk.timestamp_ns = now_ns();
        chunk.sequence = stream->next_sequence++;
        chunk.bytes_used = (uint32_t)bytes_read;
        queue_push_drop_oldest(&stream->queue, &chunk, &stream->total_dropped);

        if ((first_read_pending != 0U) || ((chunk.sequence % 50U) == 0U))
        {
            LOG_INFO("usb-audio: captured chunk seq=%lu bytes=%lu dropped=%lu",
                     (unsigned long)chunk.sequence,
                     (unsigned long)chunk.bytes_used,
                     (unsigned long)stream->total_dropped);
            first_read_pending = 0U;
        }
    }

    LOG_INFO("usb-audio: capture thread stopped");
    return NULL;
}

/**
 * @brief Main sender thread: connect/reconnect and stream queued chunks.
 * @param arg Stream service instance.
 * @return NULL.
 */
static void* sender_thread_main(void* const arg)
{
    usb_audio_stream_t* const stream = (usb_audio_stream_t*)arg;

    while (stream->stop_requested == 0U)
    {
        usb_audio_stream_chunk_t chunk;
        int status;

        if (stream->sender_fd < 0)
        {
            stream->sender_fd = connect_tcp(stream->config.tcp_host, stream->config.tcp_port);
            if (stream->sender_fd < 0)
            {
                LOG_WARNING("usb-audio: TCP connect failed target=%s:%lu",
                            stream->config.tcp_host,
                            (unsigned long)stream->config.tcp_port);
                sleep(USB_AUDIO_STREAM_CONNECT_RETRY_SEC);
                continue;
            }

            LOG_INFO("usb-audio: connected to %s:%lu",
                     stream->config.tcp_host,
                     (unsigned long)stream->config.tcp_port);

            if (send_stream_header(stream->sender_fd) != 0)
            {
                LOG_WARNING("usb-audio: failed to send stream header");
                close_sender_fd(stream);
                continue;
            }

            LOG_INFO("usb-audio: stream header sent");
        }

        status = queue_pop_wait(&stream->queue, &chunk, &stream->stop_requested);
        if (status == -EAGAIN)
        {
            continue;
        }
        if (status != 0)
        {
            break;
        }

        if (send_chunk(stream->sender_fd, &chunk, stream->total_dropped) != 0)
        {
            LOG_WARNING("usb-audio: sender disconnected");
            close_sender_fd(stream);
        }
        else if ((chunk.sequence % 50U) == 0U)
        {
            LOG_INFO("usb-audio: sent chunk seq=%lu bytes=%lu dropped=%lu",
                     (unsigned long)chunk.sequence,
                     (unsigned long)chunk.bytes_used,
                     (unsigned long)stream->total_dropped);
        }
    }

    close_sender_fd(stream);
    LOG_INFO("usb-audio: sender thread stopped");
    return NULL;
}

/**
 * @brief Copy an environment string into a fixed-size config field.
 * @param dst Destination string.
 * @param dst_size Destination capacity.
 * @param value Optional source string.
 * @return None.
 */
static void copy_env_string(char* const dst, size_t dst_size, char const* const value)
{
    if ((value == NULL) || (value[0] == '\0'))
    {
        return;
    }

    snprintf(dst, dst_size, "%s", value);
}

// === Public function implementation ============================================================================== //

/**
 * @brief Fill a USB audio stream configuration from defaults and environment.
 * @param config Configuration to initialize.
 * @return None.
 */
void usb_audio_stream_default_config(usb_audio_stream_config_t* const config)
{
    char const* env_port;

    memset(config, 0, sizeof(*config));
    snprintf(config->pcm_device, sizeof(config->pcm_device), "%s", USB_AUDIO_STREAM_DEFAULT_DEVICE);
    snprintf(config->tcp_host, sizeof(config->tcp_host), "%s", USB_AUDIO_STREAM_DEFAULT_HOST);
    config->tcp_port = USB_AUDIO_STREAM_DEFAULT_PORT;

    copy_env_string(config->pcm_device, sizeof(config->pcm_device), getenv("USB_AUDIO_PCM_DEVICE"));
    copy_env_string(config->tcp_host, sizeof(config->tcp_host), getenv("USB_AUDIO_TCP_HOST"));

    env_port = getenv("USB_AUDIO_TCP_PORT");
    if ((env_port != NULL) && (env_port[0] != '\0'))
    {
        config->tcp_port = (uint32_t)strtoul(env_port, NULL, 10);
    }
}

/**
 * @brief Start ALSA capture and TCP sender worker threads.
 * @param stream Stream service instance.
 * @param config Runtime configuration.
 * @return 0 on success, or a negative errorno_e value on failure.
 */
int usb_audio_stream_start(usb_audio_stream_t* const stream,
                           usb_audio_stream_config_t const* const config)
{
    usb_audio_capture_config_t capture_config;
    int status;

    if ((stream == NULL) || (config == NULL) || (config->pcm_device[0] == '\0')
        || (config->tcp_host[0] == '\0') || (config->tcp_port == 0U))
    {
        return -EINVAL;
    }

    memset(stream, 0, sizeof(*stream));
    stream->config = *config;
    stream->sender_fd = -1;
    queue_init(&stream->queue);

    memset(&capture_config, 0, sizeof(capture_config));
    snprintf(capture_config.device, sizeof(capture_config.device), "%s", config->pcm_device);
    capture_config.sample_rate_hz = USB_AUDIO_STREAM_SAMPLE_RATE_HZ;
    capture_config.channels = USB_AUDIO_STREAM_CHANNELS;
    capture_config.samples_per_chunk = USB_AUDIO_STREAM_SAMPLES_PER_CHUNK;

    status = usb_audio_capture_init(&stream->capture, &capture_config);
    if (status != 0)
    {
        queue_cleanup(&stream->queue);
        return status;
    }

    status = pthread_create(&stream->capture_thread, NULL, capture_thread_main, stream);
    if (status != 0)
    {
        usb_audio_capture_cleanup(&stream->capture);
        queue_cleanup(&stream->queue);
        return -EIO;
    }

    status = pthread_create(&stream->sender_thread, NULL, sender_thread_main, stream);
    if (status != 0)
    {
        stream->stop_requested = 1U;
        usb_audio_capture_abort(&stream->capture);
        pthread_join(stream->capture_thread, NULL);
        usb_audio_capture_cleanup(&stream->capture);
        queue_cleanup(&stream->queue);
        return -EIO;
    }

    stream->running = 1U;
    return 0;
}

/**
 * @brief Stop worker threads and release capture/socket resources.
 * @param stream Stream service instance.
 * @return None.
 */
void usb_audio_stream_stop(usb_audio_stream_t* const stream)
{
    if ((stream == NULL) || (stream->running == 0U))
    {
        return;
    }

    stream->stop_requested = 1U;
    usb_audio_capture_abort(&stream->capture);
    pthread_cond_broadcast(&stream->queue.cond);

    pthread_join(stream->capture_thread, NULL);
    pthread_join(stream->sender_thread, NULL);

    close_sender_fd(stream);
    usb_audio_capture_cleanup(&stream->capture);
    queue_cleanup(&stream->queue);
    stream->running = 0U;
}

// === End of documentation ======================================================================================== //
