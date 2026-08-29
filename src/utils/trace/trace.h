/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

#pragma once

///
/// @file trace.h
/// @brief Unified tracing infrastructure for performance profiling
///
/// Emits trace events in NDJSON format compatible with server-side traces.
/// All events can be merged and visualized in Perfetto.
///

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros =========================================

/// Enable/disable tracing at compile time
#ifndef CONFIG_TRACE_ENABLED
    #define CONFIG_TRACE_ENABLED (0)  // Disabled by default; enable with make TRACE=1
#endif

/// Trace event types (aligned with Chrome Trace Format)
typedef enum
{
    TRACE_PHASE_BEGIN = 'B',     ///< Begin async event
    TRACE_PHASE_END = 'E',       ///< End async event
    TRACE_PHASE_INSTANT = 'i',   ///< Instant event (point in time)
    TRACE_PHASE_COMPLETE = 'X',  ///< Complete event (begin + duration)
} trace_phase_e;

/// Trace context handle
typedef struct trace_ctx trace_ctx_t;

// === Public API ============================================

/**
 * @brief Initialize the trace subsystem.
 * @param output_path Path to NDJSON trace file (NULL for /tmp/fw_trace.jsonl)
 * @return Trace context or NULL on failure
 */
trace_ctx_t* trace_init(const char* output_path);

/**
 * @brief Close the trace subsystem and flush events.
 * @param ctx Trace context
 */
void trace_close(trace_ctx_t* ctx);

/**
 * @brief Emit an instant trace event (point in time).
 * @param ctx Trace context
 * @param name Event name
 * @param args_fmt Printf-style format for JSON args (can be NULL)
 */
void trace_instant(trace_ctx_t* ctx, const char* name, const char* args_fmt, ...);

/**
 * @brief Emit a duration event (begin + duration in one call).
 * @param ctx Trace context
 * @param name Event name
 * @param duration_ns Duration in nanoseconds
 * @param args_fmt Printf-style format for JSON args (can be NULL)
 */
void trace_duration(trace_ctx_t* ctx, const char* name, uint64_t duration_ns,
                    const char* args_fmt, ...);

/**
 * @brief Begin an async event (call trace_end to complete).
 * @param ctx Trace context
 * @param name Event name
 * @param args_fmt Printf-style format for JSON args (can be NULL)
 * @return Timestamp of begin event (pass to trace_end)
 */
uint64_t trace_begin(trace_ctx_t* ctx, const char* name, const char* args_fmt, ...);

/**
 * @brief End an async event started with trace_begin.
 * @param ctx Trace context
 * @param name Event name (must match trace_begin)
 * @param start_ns Timestamp from trace_begin
 * @param args_fmt Printf-style format for JSON args (can be NULL)
 */
void trace_end(trace_ctx_t* ctx, const char* name, uint64_t start_ns,
               const char* args_fmt, ...);

/**
 * @brief Get current monotonic timestamp in nanoseconds.
 * @return Nanoseconds since boot
 */
uint64_t trace_now_ns(void);

// === Convenience macros ====================================

#if CONFIG_TRACE_ENABLED

/// Emit instant event
#define TRACE_INSTANT(ctx, name, ...) \
    trace_instant(ctx, name, ##__VA_ARGS__)

/// Emit duration event
#define TRACE_DURATION(ctx, name, dur_ns, ...) \
    trace_duration(ctx, name, dur_ns, ##__VA_ARGS__)

/// Begin scoped event (use with TRACE_END)
#define TRACE_BEGIN(ctx, name, ...) \
    trace_begin(ctx, name, ##__VA_ARGS__)

/// End scoped event
#define TRACE_END(ctx, name, start_ns, ...) \
    trace_end(ctx, name, start_ns, ##__VA_ARGS__)

/// Helper for measuring duration of a code block
#define TRACE_SCOPE_START(ctx, name) \
    uint64_t _trace_start_##name = trace_begin(ctx, #name, NULL)

#define TRACE_SCOPE_END(ctx, name) \
    trace_end(ctx, #name, _trace_start_##name, NULL)

#else

// No-op macros when tracing disabled
#define TRACE_INSTANT(ctx, name, ...) do {} while(0)
#define TRACE_DURATION(ctx, name, dur_ns, ...) do {} while(0)
#define TRACE_BEGIN(ctx, name, ...) (0ULL)
#define TRACE_END(ctx, name, start_ns, ...) do {} while(0)
#define TRACE_SCOPE_START(ctx, name) do {} while(0)
#define TRACE_SCOPE_END(ctx, name) do {} while(0)

#endif

#ifdef __cplusplus
}
#endif
