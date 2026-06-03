/**********************************************************************************************************************
Copyright (c) 2024, <Your Name> <your_email@mail.com>

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file log.c
/// @brief Log Facility API (implementation)
///

// === Headers files inclusions ==================================================================================== //

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //

/// @brief Structure that represents a subscriber. It will consist of a tuple function + log_level_e.
typedef struct
{
    log_function_t log_function;  ///< User-defined log function.
    log_level_e threshold;           ///< Log level in which messsages will be printed out
} subscriber_t;

// === Private variable declarations =============================================================================== //

/// @brief Array containing all the log subscribers. W
static subscriber_t log_subscribers[LOG_MAX_SUBSCRIBERS] = {0};
static char log_message_buffer[LOG_MAX_MESSAGE_LENGTH] = {0};

// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

void log_init() { memset(log_subscribers, 0, sizeof(log_subscribers)); }

log_error_e log_subscribe(log_function_t log_function, log_level_e threshold)
{
    assert(log_function);

    int8_t available_slot = -1;

    // Let's find an available slot in our subscriber list.
    for (size_t i = 0; i < LOG_MAX_SUBSCRIBERS; i++) {
        if (log_subscribers[i].log_function == log_function) {
            // Already subscribed: Update threshold and return immediately.
            log_subscribers[i].threshold = threshold;
            return LOG_ERROR_NONE;

        } else if (log_subscribers[i].log_function == NULL) {
            // Found a free slot!
            available_slot = i;
        }
    }
    // log_function is not yet a subscriber.  Assign it if possible.
    if (available_slot == -1) { return LOG_ERROR_SUBSCRIBERS_EXCEEDED; }
    log_subscribers[available_slot].log_function = log_function;
    log_subscribers[available_slot].threshold = threshold;
    return LOG_ERROR_NONE;
}

log_error_e log_unsubscribe(log_function_t log_function)
{
    for (size_t i = 0; i < LOG_MAX_SUBSCRIBERS; i++) {
        if (log_subscribers[i].log_function == log_function) {
            log_subscribers[i].log_function = NULL;  // Mark as empty
            return LOG_ERROR_NONE;
        }
    }
    return LOG_ERROR_NOT_SUBSCRIBED;
}

void log_message(log_level_e severity, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_message_buffer, LOG_MAX_MESSAGE_LENGTH, fmt, ap);
    va_end(ap);

    for (size_t i = 0; i < LOG_MAX_SUBSCRIBERS; i++) {
        if (log_subscribers[i].log_function != NULL) {
            if (severity >= log_subscribers[i].threshold) {
                log_subscribers[i].log_function(severity, log_message_buffer);
            }
        }
    }
}

const char* log_level_to_str(const log_level_e severity)
{
    switch (severity) {
    case LOG_LEVEL_TRACE:
        return "TRC";
    case LOG_LEVEL_DEBUG:
        return "DBG";
    case LOG_LEVEL_INFO:
        return "INF";
    case LOG_LEVEL_WARNING:
        return "WRN";
    case LOG_LEVEL_ERROR:
        return "ERR";
    default:
        return "UNK";
    }
}

// === End of documentation ======================================================================================== //
