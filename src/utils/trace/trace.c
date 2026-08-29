/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/
**********************************************************************************************************************/

///
/// @file trace.c
/// @brief Unified tracing implementation - NDJSON output compatible with Perfetto
///

#include "trace.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// === Private types =========================================

struct trace_ctx
{
    FILE* file;
    uint64_t base_ns;     ///< Baseline timestamp (first event = t0)
    uint32_t pid;         ///< Process ID
    uint32_t tid;         ///< Thread ID (currently unused, always 1)
};

// === Private helpers =======================================

/**
 * @brief Get current monotonic timestamp in nanoseconds.
 */
uint64_t trace_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/**
 * @brief Emit raw NDJSON trace event.
 */
static void emit_event(trace_ctx_t* ctx, const char* name, char phase,
                       uint64_t ts_ns, uint64_t dur_ns, const char* args_json)
{
    if (!ctx || !ctx->file || !name)
    {
        return;
    }

    // Convert to relative timestamp (microseconds for Perfetto)
    uint64_t ts_us = (ts_ns - ctx->base_ns) / 1000;
    uint64_t dur_us = dur_ns / 1000;

    fprintf(ctx->file, "{\"name\":\"%s\",\"ph\":\"%c\",\"ts\":%llu",
            name, phase, (unsigned long long)ts_us);

    if (dur_ns > 0)
    {
        fprintf(ctx->file, ",\"dur\":%llu", (unsigned long long)dur_us);
    }

    fprintf(ctx->file, ",\"pid\":%u,\"tid\":%u,\"cat\":\"firmware\"",
            ctx->pid, ctx->tid);

    if (args_json && args_json[0] != '\0')
    {
        fprintf(ctx->file, ",\"args\":{%s}", args_json);
    }

    fprintf(ctx->file, "}\n");
    fflush(ctx->file);
}

// === Public API ============================================

trace_ctx_t* trace_init(const char* output_path)
{
    trace_ctx_t* ctx = calloc(1, sizeof(trace_ctx_t));
    if (!ctx)
    {
        return NULL;
    }

    const char* path = output_path ? output_path : "/tmp/fw_trace.jsonl";
    ctx->file = fopen(path, "w");
    if (!ctx->file)
    {
        fprintf(stderr, "trace: failed to open %s: %s\n", path, strerror(errno));
        free(ctx);
        return NULL;
    }

    ctx->base_ns = trace_now_ns();
    ctx->pid = (uint32_t)getpid();
    ctx->tid = 1; // Single-threaded for now

    // Emit metadata event
    fprintf(ctx->file,
            "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":%u,"
            "\"args\":{\"name\":\"subtitle_overlay_fw\"}}\n",
            ctx->pid);
    fprintf(ctx->file,
            "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":%u,\"tid\":%u,"
            "\"args\":{\"name\":\"main\"}}\n",
            ctx->pid, ctx->tid);
    fflush(ctx->file);

    fprintf(stderr, "trace: initialized → %s (pid=%u)\n", path, ctx->pid);
    return ctx;
}

void trace_close(trace_ctx_t* ctx)
{
    if (!ctx)
    {
        return;
    }

    if (ctx->file)
    {
        fflush(ctx->file);
        fclose(ctx->file);
    }

    free(ctx);
}

void trace_instant(trace_ctx_t* ctx, const char* name, const char* args_fmt, ...)
{
    if (!ctx || !name)
    {
        return;
    }

    char args_buf[512] = {0};
    if (args_fmt)
    {
        va_list ap;
        va_start(ap, args_fmt);
        vsnprintf(args_buf, sizeof(args_buf), args_fmt, ap);
        va_end(ap);
    }

    uint64_t now_ns = trace_now_ns();
    emit_event(ctx, name, TRACE_PHASE_INSTANT, now_ns, 0, args_buf);
}

void trace_duration(trace_ctx_t* ctx, const char* name, uint64_t duration_ns,
                    const char* args_fmt, ...)
{
    if (!ctx || !name)
    {
        return;
    }

    char args_buf[512] = {0};
    if (args_fmt)
    {
        va_list ap;
        va_start(ap, args_fmt);
        vsnprintf(args_buf, sizeof(args_buf), args_fmt, ap);
        va_end(ap);
    }

    uint64_t end_ns = trace_now_ns();
    uint64_t start_ns = end_ns - duration_ns;

    emit_event(ctx, name, TRACE_PHASE_COMPLETE, start_ns, duration_ns, args_buf);
}

uint64_t trace_begin(trace_ctx_t* ctx, const char* name, const char* args_fmt, ...)
{
    if (!ctx || !name)
    {
        return 0;
    }

    char args_buf[512] = {0};
    if (args_fmt)
    {
        va_list ap;
        va_start(ap, args_fmt);
        vsnprintf(args_buf, sizeof(args_buf), args_fmt, ap);
        va_end(ap);
    }

    uint64_t now_ns = trace_now_ns();
    emit_event(ctx, name, TRACE_PHASE_BEGIN, now_ns, 0, args_buf);

    return now_ns;
}

void trace_end(trace_ctx_t* ctx, const char* name, uint64_t start_ns,
               const char* args_fmt, ...)
{
    if (!ctx || !name)
    {
        return;
    }

    char args_buf[512] = {0};
    if (args_fmt)
    {
        va_list ap;
        va_start(ap, args_fmt);
        vsnprintf(args_buf, sizeof(args_buf), args_fmt, ap);
        va_end(ap);
    }

    uint64_t now_ns = trace_now_ns();
    emit_event(ctx, name, TRACE_PHASE_END, now_ns, 0, args_buf);
}
