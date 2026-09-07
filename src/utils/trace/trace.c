/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file trace.c
/// @brief Chrome Trace Format NDJSON writer with atomic lines and a size cap
///

// glibc only declares syscall()/SYS_gettid outside strict _POSIX_C_SOURCE, and
// the firmware compiles with -D_POSIX_C_SOURCE=200809L.
#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif

// === Headers files inclusions ==================================================================================== //

#include "trace.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
    #include <sys/syscall.h>
#endif

// === Macros definitions ========================================================================================== //

/// Staging buffer between the emitting threads and the file descriptor.
#define TRACE_BUFFER_BYTES (32U * 1024U)

// === Private data type declarations ============================================================================== //

/// @brief Bounded string builder used to serialize one line without allocating.
typedef struct
{
    char* data;
    size_t capacity;
    size_t length;
    uint8_t overflow;
} trace_sbuf_t;

struct trace_ctx
{
    int fd;
    pthread_mutex_t lock;

    char buffer[TRACE_BUFFER_BYTES];
    size_t buffer_used;

    uint64_t base_monotonic_ns;
    uint64_t base_realtime_ns;
    uint64_t last_flush_ns;
    uint64_t flush_interval_ns;

    uint64_t accepted_bytes; ///< Drives the size cap; includes buffered data.
    uint64_t max_bytes;

    trace_stats_t stats;
    uint32_t pid;
};

// === Private variable declarations =============================================================================== //

/// Bumped by every trace_init() so per-thread metadata is re-emitted for a new file.
static uint32_t trace_generation = 0U;

/// Cached Linux TID; a syscall per event would show up in the audio path.
static __thread uint32_t trace_tls_tid = 0U;

/// Generation whose thread_name metadata this thread already emitted.
static __thread uint32_t trace_tls_named_generation = 0U;

// === Private function declarations =============================================================================== //

static uint64_t trace_realtime_ns(void);
static uint32_t trace_current_tid(void);
static void sbuf_init(trace_sbuf_t* sb, char* data, size_t capacity);
static void sbuf_write(trace_sbuf_t* sb, char const* src, size_t length);
static void sbuf_putc(trace_sbuf_t* sb, char c);
static void sbuf_puts(trace_sbuf_t* sb, char const* text);
static void sbuf_u64(trace_sbuf_t* sb, uint64_t value);
static void sbuf_i64(trace_sbuf_t* sb, int64_t value);
static void sbuf_f64(trace_sbuf_t* sb, double value);
static size_t utf8_sequence_length(uint8_t lead);
static void sbuf_json_string(trace_sbuf_t* sb, char const* text, size_t max_source_bytes);
static void sbuf_args(trace_sbuf_t* sb, trace_arg_t const* args, size_t arg_count);
static size_t build_line(trace_ctx_t const* ctx,
                         char* out,
                         size_t out_size,
                         char const* name,
                         char phase,
                         uint64_t ts_ns,
                         uint64_t dur_ns,
                         uint8_t has_duration,
                         uint32_t tid,
                         trace_arg_t const* args,
                         size_t arg_count);
static void flush_locked(trace_ctx_t* ctx);
static void write_all_locked(trace_ctx_t* ctx, char const* data, size_t length);
static void append_locked(trace_ctx_t* ctx, char const* line, size_t length, uint8_t bypass_cap);
static void emit(trace_ctx_t* ctx,
                 char const* name,
                 char phase,
                 uint64_t ts_ns,
                 uint64_t dur_ns,
                 uint8_t has_duration,
                 trace_arg_t const* args,
                 size_t arg_count);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/** @brief Wall-clock nanoseconds, used only for the merge anchor. */
static uint64_t trace_realtime_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
    {
        return 0U;
    }

    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/** @brief Linux TID of the calling thread, cached in thread-local storage. */
static uint32_t trace_current_tid(void)
{
    if (trace_tls_tid == 0U)
    {
#if defined(__linux__) && defined(SYS_gettid)
        trace_tls_tid = (uint32_t)syscall(SYS_gettid);
#else
        trace_tls_tid = (uint32_t)(uintptr_t)pthread_self();
#endif
        if (trace_tls_tid == 0U)
        {
            trace_tls_tid = 1U; // Keep 0 reserved for "unknown".
        }
    }

    return trace_tls_tid;
}

static void sbuf_init(trace_sbuf_t* const sb, char* const data, size_t const capacity)
{
    sb->data = data;
    sb->capacity = capacity;
    sb->length = 0U;
    sb->overflow = 0U;
}

static void sbuf_write(trace_sbuf_t* const sb, char const* const src, size_t const length)
{
    if (sb->overflow != 0U)
    {
        return;
    }

    // Reserve one byte so the caller can always terminate the buffer.
    if (length > (sb->capacity - sb->length - 1U))
    {
        sb->overflow = 1U;
        return;
    }

    memcpy(&sb->data[sb->length], src, length);
    sb->length += length;
}

static void sbuf_putc(trace_sbuf_t* const sb, char const c)
{
    sbuf_write(sb, &c, 1U);
}

static void sbuf_puts(trace_sbuf_t* const sb, char const* const text)
{
    sbuf_write(sb, text, strlen(text));
}

static void sbuf_u64(trace_sbuf_t* const sb, uint64_t value)
{
    char digits[20];
    size_t count = 0U;

    if (value == 0U)
    {
        sbuf_putc(sb, '0');
        return;
    }

    while ((value > 0U) && (count < sizeof(digits)))
    {
        digits[count] = (char)('0' + (char)(value % 10U));
        value /= 10U;
        count++;
    }

    while (count > 0U)
    {
        count--;
        sbuf_putc(sb, digits[count]);
    }
}

static void sbuf_i64(trace_sbuf_t* const sb, int64_t const value)
{
    if (value < 0)
    {
        sbuf_putc(sb, '-');
        // Negate in unsigned space so INT64_MIN does not overflow.
        sbuf_u64(sb, (uint64_t)(-(value + 1)) + 1ULL);
        return;
    }

    sbuf_u64(sb, (uint64_t)value);
}

static void sbuf_f64(trace_sbuf_t* const sb, double const value)
{
    char text[40];
    int written;

    // JSON has no representation for non-finite numbers; report them as null.
    if (!(value == value) || (value > 1.0e308) || (value < -1.0e308))
    {
        sbuf_puts(sb, "null");
        return;
    }

    written = snprintf(text, sizeof(text), "%.6f", value);
    if ((written <= 0) || ((size_t)written >= sizeof(text)))
    {
        sbuf_puts(sb, "null");
        return;
    }

    sbuf_write(sb, text, (size_t)written);
}

/** @brief Byte length of the UTF-8 sequence introduced by @p lead, or 0 if invalid. */
static size_t utf8_sequence_length(uint8_t const lead)
{
    if (lead < 0x80U)
    {
        return 1U;
    }
    if ((lead & 0xE0U) == 0xC0U)
    {
        return 2U;
    }
    if ((lead & 0xF0U) == 0xE0U)
    {
        return 3U;
    }
    if ((lead & 0xF8U) == 0xF0U)
    {
        return 4U;
    }

    return 0U;
}

/**
 * @brief Append @p text as a quoted JSON string, escaped and UTF-8 safe.
 *
 * Only whole code points are copied, so truncating at @p max_source_bytes can
 * never split a multi-byte character. Malformed input is replaced with U+FFFD
 * rather than passed through, which keeps every emitted line decodable.
 */
static void sbuf_json_string(trace_sbuf_t* const sb,
                             char const* const text,
                             size_t const max_source_bytes)
{
    size_t index = 0U;

    sbuf_putc(sb, '"');

    if (text == NULL)
    {
        sbuf_putc(sb, '"');
        return;
    }

    while ((index < max_source_bytes) && (text[index] != '\0'))
    {
        uint8_t const byte = (uint8_t)text[index];
        size_t sequence;
        size_t offset;
        uint8_t valid;

        if (byte < 0x80U)
        {
            switch (byte)
            {
            case (uint8_t)'"':
                sbuf_puts(sb, "\\\"");
                break;
            case (uint8_t)'\\':
                sbuf_puts(sb, "\\\\");
                break;
            case (uint8_t)'\n':
                sbuf_puts(sb, "\\n");
                break;
            case (uint8_t)'\r':
                sbuf_puts(sb, "\\r");
                break;
            case (uint8_t)'\t':
                sbuf_puts(sb, "\\t");
                break;
            case (uint8_t)'\b':
                sbuf_puts(sb, "\\b");
                break;
            case (uint8_t)'\f':
                sbuf_puts(sb, "\\f");
                break;
            default:
                if (byte < 0x20U)
                {
                    static char const hex[] = "0123456789abcdef";

                    sbuf_puts(sb, "\\u00");
                    sbuf_putc(sb, hex[(byte >> 4U) & 0x0FU]);
                    sbuf_putc(sb, hex[byte & 0x0FU]);
                }
                else
                {
                    sbuf_putc(sb, (char)byte);
                }
                break;
            }

            index++;
            continue;
        }

        sequence = utf8_sequence_length(byte);
        valid = ((sequence >= 2U) && ((index + sequence) <= max_source_bytes)) ? 1U : 0U;

        for (offset = 1U; (valid != 0U) && (offset < sequence); offset++)
        {
            if (((uint8_t)text[index + offset] & 0xC0U) != 0x80U)
            {
                valid = 0U;
            }
        }

        if (valid == 0U)
        {
            // Either malformed input or a code point that does not fit in the
            // budget. Emitting the replacement character keeps the line valid.
            sbuf_puts(sb, "\\ufffd");
            index++;
            continue;
        }

        sbuf_write(sb, &text[index], sequence);
        index += sequence;
    }

    sbuf_putc(sb, '"');
}

static void sbuf_args(trace_sbuf_t* const sb, trace_arg_t const* const args, size_t arg_count)
{
    size_t index;

    if ((args == NULL) || (arg_count == 0U))
    {
        return;
    }

    if (arg_count > TRACE_ARG_MAX)
    {
        arg_count = TRACE_ARG_MAX;
    }

    sbuf_puts(sb, ",\"args\":{");
    for (index = 0U; index < arg_count; index++)
    {
        trace_arg_t const* const arg = &args[index];

        if (index > 0U)
        {
            sbuf_putc(sb, ',');
        }

        sbuf_json_string(sb, (arg->key != NULL) ? arg->key : "?", 64U);
        sbuf_putc(sb, ':');

        switch (arg->type)
        {
        case TRACE_ARG_TYPE_I64:
            sbuf_i64(sb, arg->value.i64);
            break;
        case TRACE_ARG_TYPE_U64:
            sbuf_u64(sb, arg->value.u64);
            break;
        case TRACE_ARG_TYPE_F64:
            sbuf_f64(sb, arg->value.f64);
            break;
        case TRACE_ARG_TYPE_BOOL:
            sbuf_puts(sb, (arg->value.u64 != 0ULL) ? "true" : "false");
            break;
        case TRACE_ARG_TYPE_STR:
        default:
            sbuf_json_string(sb, arg->value.str, TRACE_STR_ARG_MAX);
            break;
        }
    }
    sbuf_putc(sb, '}');
}

/**
 * @brief Serialize one complete NDJSON line.
 * @return Byte length written to @p out, or 0 when the line did not fit.
 */
static size_t build_line(trace_ctx_t const* const ctx,
                         char* const out,
                         size_t const out_size,
                         char const* const name,
                         char const phase,
                         uint64_t const ts_ns,
                         uint64_t const dur_ns,
                         uint8_t const has_duration,
                         uint32_t const tid,
                         trace_arg_t const* const args,
                         size_t const arg_count)
{
    trace_sbuf_t sb;
    uint64_t relative_ns;

    sbuf_init(&sb, out, out_size);

    // A clock read that failed returns 0; clamp instead of wrapping around.
    relative_ns = (ts_ns > ctx->base_monotonic_ns) ? (ts_ns - ctx->base_monotonic_ns) : 0U;

    sbuf_puts(&sb, "{\"name\":");
    sbuf_json_string(&sb, name, 96U);
    sbuf_puts(&sb, ",\"ph\":\"");
    sbuf_putc(&sb, phase);
    sbuf_puts(&sb, "\",\"ts\":");
    sbuf_u64(&sb, relative_ns / 1000U);

    if (has_duration != 0U)
    {
        sbuf_puts(&sb, ",\"dur\":");
        sbuf_u64(&sb, dur_ns / 1000U);
    }

    if (phase == 'i')
    {
        // Thread scope keeps instants on the emitting track instead of spanning
        // the whole process row.
        sbuf_puts(&sb, ",\"s\":\"t\"");
    }

    sbuf_puts(&sb, ",\"pid\":");
    sbuf_u64(&sb, ctx->pid);
    sbuf_puts(&sb, ",\"tid\":");
    sbuf_u64(&sb, tid);
    sbuf_puts(&sb, ",\"cat\":\"firmware\"");

    sbuf_args(&sb, args, arg_count);

    sbuf_puts(&sb, "}\n");

    if (sb.overflow != 0U)
    {
        return 0U;
    }

    return sb.length;
}

/** @brief Push the staging buffer to the file descriptor. Caller holds the lock. */
static void flush_locked(trace_ctx_t* const ctx)
{
    uint64_t started_ns;
    uint64_t elapsed_ns;

    if (ctx->buffer_used == 0U)
    {
        return;
    }

    started_ns = trace_now_ns();
    write_all_locked(ctx, ctx->buffer, ctx->buffer_used);
    ctx->buffer_used = 0U;
    ctx->last_flush_ns = trace_now_ns();

    // The emitting thread pays this write. Recording the worst case turns "does
    // profiling stall the audio path?" into a number the capture itself answers.
    elapsed_ns = (ctx->last_flush_ns > started_ns) ? (ctx->last_flush_ns - started_ns) : 0U;
    ctx->stats.flushes++;
    if (elapsed_ns > ctx->stats.max_flush_ns)
    {
        ctx->stats.max_flush_ns = elapsed_ns;
    }
}

/** @brief write(2) with partial-write and EINTR handling. Caller holds the lock. */
static void write_all_locked(trace_ctx_t* const ctx, char const* const data, size_t const length)
{
    size_t offset = 0U;

    if (ctx->fd < 0)
    {
        return;
    }

    while (offset < length)
    {
        ssize_t const written = write(ctx->fd, &data[offset], length - offset);

        if (written > 0)
        {
            offset += (size_t)written;
            continue;
        }

        if ((written < 0) && (errno == EINTR))
        {
            continue;
        }

        // A profiling write must never escalate: count it and move on.
        ctx->stats.write_errors++;
        break;
    }

    ctx->stats.bytes_written += (uint64_t)offset;
}

/** @brief Accept one serialized line, honouring the size cap. Caller holds the lock. */
static void append_locked(trace_ctx_t* const ctx,
                          char const* const line,
                          size_t const length,
                          uint8_t const bypass_cap)
{
    uint64_t now_ns;

    if ((bypass_cap == 0U) && (ctx->stats.full != 0U))
    {
        ctx->stats.dropped_full++;
        return;
    }

    if ((bypass_cap == 0U) && ((ctx->accepted_bytes + (uint64_t)length) > ctx->max_bytes))
    {
        ctx->stats.full = 1U;
        ctx->stats.dropped_full++;
        return;
    }

    if ((ctx->buffer_used + length) > sizeof(ctx->buffer))
    {
        flush_locked(ctx);
    }

    if (length > sizeof(ctx->buffer))
    {
        // Cannot happen with TRACE_LINE_MAX < TRACE_BUFFER_BYTES, but a direct
        // write keeps the invariant true if either constant ever changes.
        write_all_locked(ctx, line, length);
    }
    else
    {
        memcpy(&ctx->buffer[ctx->buffer_used], line, length);
        ctx->buffer_used += length;
    }

    ctx->accepted_bytes += (uint64_t)length;
    ctx->stats.events_written++;

    now_ns = trace_now_ns();
    if ((now_ns - ctx->last_flush_ns) >= ctx->flush_interval_ns)
    {
        flush_locked(ctx);
    }
}

/** @brief Serialize outside the lock, then append under it. */
static void emit(trace_ctx_t* const ctx,
                 char const* const name,
                 char const phase,
                 uint64_t const ts_ns,
                 uint64_t const dur_ns,
                 uint8_t const has_duration,
                 trace_arg_t const* const args,
                 size_t const arg_count)
{
    char line[TRACE_LINE_MAX];
    size_t length;
    uint32_t const tid = trace_current_tid();

    if ((ctx == NULL) || (name == NULL))
    {
        return;
    }

    length = build_line(ctx, line, sizeof(line), name, phase, ts_ns, dur_ns, has_duration, tid,
                        args, arg_count);

    pthread_mutex_lock(&ctx->lock);
    if (length == 0U)
    {
        // Dropping one oversized event beats emitting a line that would make
        // the whole file unparseable.
        ctx->stats.dropped_truncated++;
    }
    else
    {
        uint8_t const was_full = ctx->stats.full;

        append_locked(ctx, line, length, 0U);

        if ((was_full == 0U) && (ctx->stats.full != 0U))
        {
            char notice[TRACE_LINE_MAX];
            trace_arg_t const notice_args[] = {
                TRACE_U64("max_bytes", ctx->max_bytes),
                TRACE_U64("accepted_bytes", ctx->accepted_bytes),
            };
            size_t const notice_length =
                build_line(ctx, notice, sizeof(notice), "trace_full", 'i', trace_now_ns(), 0U, 0U,
                           tid, notice_args, sizeof(notice_args) / sizeof(notice_args[0]));

            if (notice_length > 0U)
            {
                append_locked(ctx, notice, notice_length, 1U);
            }
            flush_locked(ctx);

            fprintf(stderr, "trace: size cap of %llu bytes reached; tracing stopped\n",
                    (unsigned long long)ctx->max_bytes);
        }
    }
    pthread_mutex_unlock(&ctx->lock);
}

// === Public function implementation ============================================================================== //

uint64_t trace_now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0U;
    }

    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

trace_ctx_t* trace_init(trace_config_t const* const config)
{
    static trace_config_t const defaults = {NULL, NULL, NULL, NULL, 0U, 0U};
    trace_config_t const* const cfg = (config != NULL) ? config : &defaults;
    char const* const path =
        (cfg->output_path != NULL) ? cfg->output_path : "/tmp/fw_trace.jsonl";
    char const* const source = (cfg->source != NULL) ? cfg->source : "board";
    trace_ctx_t* ctx;
    char line[TRACE_LINE_MAX];
    size_t length;

    ctx = calloc(1U, sizeof(*ctx));
    if (ctx == NULL)
    {
        fprintf(stderr, "trace: out of memory\n");
        return NULL;
    }

    if (pthread_mutex_init(&ctx->lock, NULL) != 0)
    {
        fprintf(stderr, "trace: mutex init failed\n");
        free(ctx);
        return NULL;
    }

    // O_APPEND makes each write atomic against any other writer of this file.
    ctx->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (ctx->fd < 0)
    {
        fprintf(stderr, "trace: cannot open %s: %s\n", path, strerror(errno));
        (void)pthread_mutex_destroy(&ctx->lock);
        free(ctx);
        return NULL;
    }

    ctx->pid = (uint32_t)getpid();
    ctx->base_monotonic_ns = trace_now_ns();
    ctx->base_realtime_ns = trace_realtime_ns();
    ctx->last_flush_ns = ctx->base_monotonic_ns;
    ctx->max_bytes = (cfg->max_bytes != 0U) ? cfg->max_bytes : TRACE_DEFAULT_MAX_BYTES;
    ctx->flush_interval_ns =
        ((cfg->flush_interval_ms != 0U) ? (uint64_t)cfg->flush_interval_ms
                                        : (uint64_t)TRACE_DEFAULT_FLUSH_MS)
        * 1000000ULL;

    trace_generation++;

    {
        trace_arg_t const process_args[] = {TRACE_STR("name", source)};

        length = build_line(ctx, line, sizeof(line), "process_name", 'M', ctx->base_monotonic_ns,
                            0U, 0U, 0U, process_args, 1U);
        if (length > 0U)
        {
            append_locked(ctx, line, length, 1U);
        }
    }

    {
        // The anchor pair is what lets the merge place board and server events
        // on one UTC timeline; without it the two files only share t=0.
        trace_arg_t const start_args[] = {
            TRACE_U64("format_version", TRACE_FORMAT_VERSION),
            TRACE_STR("source", source),
            TRACE_STR("run_id", (cfg->run_id != NULL) ? cfg->run_id : ""),
            TRACE_STR("build_id", (cfg->build_id != NULL) ? cfg->build_id : ""),
            TRACE_U64("pid", ctx->pid),
            TRACE_U64("clock_monotonic_ns", ctx->base_monotonic_ns),
            TRACE_U64("clock_realtime_ns", ctx->base_realtime_ns),
            TRACE_STR("clock_domain", "CLOCK_MONOTONIC"),
            TRACE_U64("max_bytes", ctx->max_bytes),
        };

        length = build_line(ctx, line, sizeof(line), "trace_start", 'i', ctx->base_monotonic_ns, 0U,
                            0U, trace_current_tid(), start_args,
                            sizeof(start_args) / sizeof(start_args[0]));
        if (length > 0U)
        {
            append_locked(ctx, line, length, 1U);
        }
    }

    flush_locked(ctx);

    fprintf(stderr, "trace: writing %s (pid=%u, cap=%llu bytes)\n", path, ctx->pid,
            (unsigned long long)ctx->max_bytes);
    return ctx;
}

void trace_thread_register(trace_ctx_t* const ctx, char const* const thread_name)
{
    char line[TRACE_LINE_MAX];
    size_t length;
    trace_arg_t const args[] = {TRACE_STR("name", thread_name)};

    if ((ctx == NULL) || (thread_name == NULL))
    {
        return;
    }

    if (trace_tls_named_generation == trace_generation)
    {
        return;
    }
    trace_tls_named_generation = trace_generation;

    length = build_line(ctx, line, sizeof(line), "thread_name", 'M', trace_now_ns(), 0U, 0U,
                        trace_current_tid(), args, sizeof(args) / sizeof(args[0]));
    if (length == 0U)
    {
        return;
    }

    pthread_mutex_lock(&ctx->lock);
    append_locked(ctx, line, length, 1U);
    pthread_mutex_unlock(&ctx->lock);
}

void trace_instant(trace_ctx_t* const ctx,
                   char const* const name,
                   trace_arg_t const* const args,
                   size_t const arg_count)
{
    emit(ctx, name, 'i', trace_now_ns(), 0U, 0U, args, arg_count);
}

void trace_slice(trace_ctx_t* const ctx,
                 char const* const name,
                 uint64_t const start_ns,
                 trace_arg_t const* const args,
                 size_t const arg_count)
{
    uint64_t const end_ns = trace_now_ns();
    uint64_t const duration_ns = (end_ns > start_ns) ? (end_ns - start_ns) : 0U;

    emit(ctx, name, 'X', start_ns, duration_ns, 1U, args, arg_count);
}

void trace_counter(trace_ctx_t* const ctx,
                   char const* const name,
                   trace_arg_t const* const args,
                   size_t const arg_count)
{
    emit(ctx, name, 'C', trace_now_ns(), 0U, 0U, args, arg_count);
}

void trace_flush(trace_ctx_t* const ctx)
{
    if (ctx == NULL)
    {
        return;
    }

    pthread_mutex_lock(&ctx->lock);
    flush_locked(ctx);
    pthread_mutex_unlock(&ctx->lock);
}

void trace_get_stats(trace_ctx_t* const ctx, trace_stats_t* const out)
{
    if (out == NULL)
    {
        return;
    }

    if (ctx == NULL)
    {
        memset(out, 0, sizeof(*out));
        return;
    }

    pthread_mutex_lock(&ctx->lock);
    *out = ctx->stats;
    pthread_mutex_unlock(&ctx->lock);
}

void trace_close(trace_ctx_t* const ctx)
{
    char line[TRACE_LINE_MAX];
    size_t length;

    if (ctx == NULL)
    {
        return;
    }

    pthread_mutex_lock(&ctx->lock);
    {
        // A capture that dropped events must say so in the file itself.
        trace_arg_t const args[] = {
            TRACE_U64("events_written", ctx->stats.events_written),
            TRACE_U64("dropped_full", ctx->stats.dropped_full),
            TRACE_U64("dropped_truncated", ctx->stats.dropped_truncated),
            TRACE_U64("write_errors", ctx->stats.write_errors),
            TRACE_U64("bytes_written", ctx->stats.bytes_written),
            TRACE_U64("flushes", ctx->stats.flushes),
            TRACE_U64("max_flush_us", ctx->stats.max_flush_ns / 1000U),
            TRACE_BOOL("full", ctx->stats.full),
        };

        length = build_line(ctx, line, sizeof(line), "trace_stats", 'i', trace_now_ns(), 0U, 0U,
                            trace_current_tid(), args, sizeof(args) / sizeof(args[0]));
        if (length > 0U)
        {
            append_locked(ctx, line, length, 1U);
        }
        flush_locked(ctx);
    }
    pthread_mutex_unlock(&ctx->lock);

    if (ctx->fd >= 0)
    {
        (void)close(ctx->fd);
        ctx->fd = -1;
    }

    (void)pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

// === End of documentation ======================================================================================== //
