/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file trace.h
/// @brief Unified profiling tracer emitting Chrome Trace Format NDJSON
///
/// One line per event, written atomically, so Perfetto can import the file
/// directly (https://perfetto.dev/docs/getting-started/other-formats).
///
/// Design rules that this API enforces rather than documents:
///  * **Typed arguments.** Call sites never build JSON. `trace_arg_t` values are
///    serialized by this module, which escapes strings and truncates them on a
///    UTF-8 code-point boundary. A transcript can therefore never corrupt a line.
///  * **Self-contained slices.** There is no begin/end pair to leak. A duration
///    is reported with `trace_slice()` from a start stamp taken by
///    `trace_now_ns()`, so an early `return` can never leave a dangling slice.
///  * **Real thread ids.** Every event carries the Linux TID of the emitting
///    thread, so Perfetto shows the QP/C thread, the ALSA capture thread and the
///    STT network worker as separate tracks.
///  * **Bounded output.** The file stops growing at `max_bytes`; further events
///    are counted as drops instead of filling the board's filesystem.
///  * **Never fatal.** Every entry point tolerates a NULL context and a failed
///    write. Profiling can degrade, but it cannot take the firmware down.
///
/// Threading: all entry points are safe to call from any thread once
/// `trace_init()` has returned. `trace_init()` and `trace_close()` are not, and
/// are expected to run on the startup/shutdown thread.
///

// === Headers files inclusions ==================================================================================== //

#include <stddef.h>
#include <stdint.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

/// Compile-time switch. Profiling builds define it to 1 (see `scripts/build.sh -p`).
#ifndef CONFIG_TRACE_ENABLED
    #define CONFIG_TRACE_ENABLED (0)
#endif

/// Maximum serialized length of one event line, including the newline.
#define TRACE_LINE_MAX (1024U)

/// Maximum number of arguments accepted on a single event.
#define TRACE_ARG_MAX (12U)

/// Maximum number of source bytes copied out of a string argument.
#define TRACE_STR_ARG_MAX (96U)

/// Default cap for the trace file. Roughly 25 minutes at the measured ~44 MB/h.
#define TRACE_DEFAULT_MAX_BYTES (16ULL * 1024ULL * 1024ULL)

/// Default interval between forced flushes, bounding loss on a crash.
#define TRACE_DEFAULT_FLUSH_MS (1000U)

/// Version stamped into the `trace_start` record so importers can adapt.
#define TRACE_FORMAT_VERSION (2U)

// === Public data type declarations =============================================================================== //

/// @brief Discriminator for a typed event argument.
typedef enum
{
    TRACE_ARG_TYPE_I64 = 0,
    TRACE_ARG_TYPE_U64,
    TRACE_ARG_TYPE_F64,
    TRACE_ARG_TYPE_BOOL,
    TRACE_ARG_TYPE_STR,
} trace_arg_type_e;

/// @brief One `key: value` pair attached to an event.
typedef struct
{
    char const* key;
    trace_arg_type_e type;
    union
    {
        int64_t i64;
        uint64_t u64;
        double f64;
        char const* str;
    } value;
} trace_arg_t;

/// Argument constructors. These expand to brace initializers, so they are only
/// valid inside the argument list of the TRACE_* macros below.
#define TRACE_I64(key_, value_)  {(key_), TRACE_ARG_TYPE_I64, {.i64 = (int64_t)(value_)}}
#define TRACE_U64(key_, value_)  {(key_), TRACE_ARG_TYPE_U64, {.u64 = (uint64_t)(value_)}}
#define TRACE_F64(key_, value_)  {(key_), TRACE_ARG_TYPE_F64, {.f64 = (double)(value_)}}
#define TRACE_BOOL(key_, value_) {(key_), TRACE_ARG_TYPE_BOOL, {.u64 = ((value_) ? 1ULL : 0ULL)}}
#define TRACE_STR(key_, value_)  {(key_), TRACE_ARG_TYPE_STR, {.str = (value_)}}

/// @brief Opaque tracer instance.
typedef struct trace_ctx trace_ctx_t;

/// @brief Startup configuration. A NULL field or zero selects the default.
typedef struct
{
    char const* output_path;      ///< NDJSON destination (default `/tmp/fw_trace.jsonl`).
    char const* source;           ///< Trace origin, e.g. `board`.
    char const* run_id;           ///< Correlates this file with the server trace.
    char const* build_id;         ///< Firmware build/commit identifier.
    uint64_t max_bytes;           ///< Hard cap on file size (default TRACE_DEFAULT_MAX_BYTES).
    uint32_t flush_interval_ms;   ///< Forced flush period (default TRACE_DEFAULT_FLUSH_MS).
} trace_config_t;

/// @brief Self-reported tracer health, so a capture can prove it lost nothing.
typedef struct
{
    uint64_t events_written;     ///< Events serialized and accepted into the buffer.
    uint64_t bytes_written;      ///< Bytes handed to the file descriptor.
    uint64_t dropped_full;       ///< Events discarded after the size cap was hit.
    uint64_t dropped_truncated;  ///< Events discarded because they exceeded TRACE_LINE_MAX.
    uint64_t write_errors;       ///< Failed `write()` calls.
    uint64_t flushes;            ///< Times the staging buffer was written out.
    uint64_t max_flush_ns;       ///< Longest single flush, i.e. the worst stall an
                                 ///< emitting thread paid. Reported so the need for
                                 ///< a dedicated writer thread is a measurement
                                 ///< rather than an assumption.
    uint8_t full;                ///< Nonzero once the size cap stopped the capture.
} trace_stats_t;

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/**
 * @brief Create the tracer and write the metadata header.
 *
 * Emits `process_name` metadata plus a `trace_start` instant carrying the
 * monotonic/realtime clock anchor pair needed to place this file on a shared
 * UTC timeline during the merge.
 *
 * @param config Optional configuration; NULL selects every default.
 * @return Tracer instance, or NULL when the output file cannot be opened.
 */
trace_ctx_t* trace_init(trace_config_t const* config);

/**
 * @brief Emit the closing statistics, flush and release the tracer.
 * @param ctx Tracer instance; NULL is ignored.
 */
void trace_close(trace_ctx_t* ctx);

/**
 * @brief Force buffered events out to the file descriptor.
 * @param ctx Tracer instance; NULL is ignored.
 */
void trace_flush(trace_ctx_t* ctx);

/**
 * @brief Name the calling thread's track and cache its Linux TID.
 *
 * Safe to call more than once per thread; only the first call emits metadata.
 *
 * @param ctx Tracer instance; NULL is ignored.
 * @param thread_name Stable track name, e.g. `qpc-main`.
 */
void trace_thread_register(trace_ctx_t* ctx, char const* thread_name);

/**
 * @brief Read the monotonic clock used for every timestamp.
 * @return Nanoseconds on CLOCK_MONOTONIC, or 0 when the clock is unreadable.
 */
uint64_t trace_now_ns(void);

/**
 * @brief Emit a point-in-time event on the calling thread's track.
 * @param ctx Tracer instance; NULL is ignored.
 * @param name Event name.
 * @param args Argument array, or NULL.
 * @param arg_count Number of arguments, clamped to TRACE_ARG_MAX.
 */
void trace_instant(trace_ctx_t* ctx, char const* name, trace_arg_t const* args, size_t arg_count);

/**
 * @brief Emit a complete slice spanning @p start_ns until now.
 *
 * The slice is self-contained (Chrome Trace Format phase `X`), so no separate
 * end event is required and no code path can leave it open.
 *
 * @param ctx Tracer instance; NULL is ignored.
 * @param name Slice name.
 * @param start_ns Stamp previously returned by trace_now_ns().
 * @param args Argument array, or NULL.
 * @param arg_count Number of arguments, clamped to TRACE_ARG_MAX.
 */
void trace_slice(trace_ctx_t* ctx,
                 char const* name,
                 uint64_t start_ns,
                 trace_arg_t const* args,
                 size_t arg_count);

/**
 * @brief Emit a counter sample. Perfetto plots one track per numeric argument.
 * @param ctx Tracer instance; NULL is ignored.
 * @param name Counter group name, e.g. `pipeline_health`.
 * @param args Numeric arguments; string arguments are rejected by Perfetto.
 * @param arg_count Number of arguments, clamped to TRACE_ARG_MAX.
 */
void trace_counter(trace_ctx_t* ctx, char const* name, trace_arg_t const* args, size_t arg_count);

/**
 * @brief Copy the tracer's self-reported health.
 * @param ctx Tracer instance; NULL zeroes @p out.
 * @param out Destination statistics.
 */
void trace_get_stats(trace_ctx_t* ctx, trace_stats_t* out);

// === Convenience macros ========================================================================================== //

#if CONFIG_TRACE_ENABLED

/// Emit an instant with at least one argument.
#define TRACE_INSTANT(ctx_, name_, ...)                                                            \
    do                                                                                             \
    {                                                                                              \
        if ((ctx_) != NULL)                                                                        \
        {                                                                                          \
            trace_arg_t const trace_args_[] = {__VA_ARGS__};                                       \
            trace_instant((ctx_), (name_), trace_args_,                                            \
                          sizeof(trace_args_) / sizeof(trace_args_[0]));                           \
        }                                                                                          \
    } while (0)

/// Emit an instant with no arguments.
#define TRACE_INSTANT0(ctx_, name_)                                                                \
    do                                                                                             \
    {                                                                                              \
        if ((ctx_) != NULL)                                                                        \
        {                                                                                          \
            trace_instant((ctx_), (name_), NULL, 0U);                                              \
        }                                                                                          \
    } while (0)

/// Emit a complete slice with at least one argument.
#define TRACE_SLICE(ctx_, name_, start_ns_, ...)                                                   \
    do                                                                                             \
    {                                                                                              \
        if ((ctx_) != NULL)                                                                        \
        {                                                                                          \
            trace_arg_t const trace_args_[] = {__VA_ARGS__};                                       \
            trace_slice((ctx_), (name_), (start_ns_), trace_args_,                                 \
                        sizeof(trace_args_) / sizeof(trace_args_[0]));                             \
        }                                                                                          \
    } while (0)

/// Emit a complete slice with no arguments.
#define TRACE_SLICE0(ctx_, name_, start_ns_)                                                       \
    do                                                                                             \
    {                                                                                              \
        if ((ctx_) != NULL)                                                                        \
        {                                                                                          \
            trace_slice((ctx_), (name_), (start_ns_), NULL, 0U);                                   \
        }                                                                                          \
    } while (0)

/// Emit a counter sample.
#define TRACE_COUNTER(ctx_, name_, ...)                                                            \
    do                                                                                             \
    {                                                                                              \
        if ((ctx_) != NULL)                                                                        \
        {                                                                                          \
            trace_arg_t const trace_args_[] = {__VA_ARGS__};                                       \
            trace_counter((ctx_), (name_), trace_args_,                                            \
                          sizeof(trace_args_) / sizeof(trace_args_[0]));                           \
        }                                                                                          \
    } while (0)

/// Name the calling thread's track.
#define TRACE_THREAD(ctx_, name_)                                                                  \
    do                                                                                             \
    {                                                                                              \
        if ((ctx_) != NULL)                                                                        \
        {                                                                                          \
            trace_thread_register((ctx_), (name_));                                                \
        }                                                                                          \
    } while (0)

/// Take a slice start stamp.
#define TRACE_NOW() trace_now_ns()

#else /* CONFIG_TRACE_ENABLED */

/* A disabled build must not even name the tracer: referencing the context would
 * make every instrumented translation unit depend on the g_trace symbol, which
 * only the firmware defines. The slice macros still consume their start stamp so
 * the local kept at the call site does not become an unused variable. */
#define TRACE_INSTANT(ctx_, name_, ...)                                                            \
    do                                                                                             \
    {                                                                                              \
    } while (0)
#define TRACE_INSTANT0(ctx_, name_)                                                                \
    do                                                                                             \
    {                                                                                              \
    } while (0)
#define TRACE_SLICE(ctx_, name_, start_ns_, ...)                                                   \
    do                                                                                             \
    {                                                                                              \
        (void)(start_ns_);                                                                         \
    } while (0)
#define TRACE_SLICE0(ctx_, name_, start_ns_)                                                       \
    do                                                                                             \
    {                                                                                              \
        (void)(start_ns_);                                                                         \
    } while (0)
#define TRACE_COUNTER(ctx_, name_, ...)                                                            \
    do                                                                                             \
    {                                                                                              \
    } while (0)
#define TRACE_THREAD(ctx_, name_)                                                                  \
    do                                                                                             \
    {                                                                                              \
    } while (0)
#define TRACE_NOW() (0ULL)

#endif /* CONFIG_TRACE_ENABLED */

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
