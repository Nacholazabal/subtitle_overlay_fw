/**********************************************************************************************************************
Copyright (c) 2024, <Your Name> <your_email@mail.com>

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file log.h
/// @brief Log Facility API. This module allows to print formatted messages over an output stream (currently an UART
/// peripheral) adding a timestamp and a log level.
///

// === Headers files inclusions ==================================================================================== //

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

// Maximum number of concurrent subscribers
#ifndef LOG_MAX_SUBSCRIBERS
#define LOG_MAX_SUBSCRIBERS (1U)
#endif

// Maximum length of the formatted log messages
#ifndef LOG_MAX_MESSAGE_LENGTH
#define LOG_MAX_MESSAGE_LENGTH (128U)
#endif

// Helper macros for printing log messages
#ifdef CONFIG_LOG_ENABLED
#define LOG(...) log_message(__VA_ARGS__)
#define LOG_TRACE(...) log_message(LOG_LEVEL_TRACE, __VA_ARGS__)
#define LOG_DEBUG(...) log_message(LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_INFO(...) log_message(LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_WARNING(...) log_message(LOG_LEVEL_WARNING, __VA_ARGS__)
#define LOG_ERROR(...) log_message(LOG_LEVEL_ERROR, __VA_ARGS__)
#else
#define LOG(...) \
    {            \
    }
#define LOG_TRACE(...) \
    {                  \
    }
#define LOG_DEBUG(...) \
    {                  \
    }
#define LOG_INFO(...) \
    {                 \
    }
#define LOG_WARNING(...) \
    {                    \
    }
#define LOG_ERROR(...) \
    {                  \
    }
#endif

// === Public data type declarations =============================================================================== //

/// @brief Log Severity Level, useful for filtering messages
typedef enum
{
    LOG_LEVEL_TRACE = 0,  ///< Trace Level
    LOG_LEVEL_DEBUG,      ///< Debug Level
    LOG_LEVEL_INFO,       ///< Info Level
    LOG_LEVEL_WARNING,    ///< Warning Level
    LOG_LEVEL_ERROR,      ///< Error Level
} log_level_e;

/// @brief Errors returned by this Log API
typedef enum
{
    LOG_ERROR_NONE = 0,              ///< No error happened
    LOG_ERROR_NOT_SUBSCRIBED,        ///< We're trying to unsubscribe a not subscribed function
    LOG_ERROR_SUBSCRIBERS_EXCEEDED,  ///< Reached Subscribber limit. Consider increasing `LOG_MAX_SUBSCRIBERS`
} log_error_e;

/// Prototype for Log subscribers. The log functions must follow this signature
typedef void (*log_function_t)(const log_level_e severity, const char* msg);

// === Public variable declarations ================================================================================ //
// === Public function declarations ================================================================================ //

/// @brief Initialize the Log Facility. This function must be called before sending any data.
void log_init();

/// @brief Subscribe the specified function to the log facility, so as to use to stream messages.
/// @param log_function Function to register
/// @param threshold Log level in which the log function should output mesages
/// @return `LOG_ERROR_NONE` on success.
log_error_e log_subscribe(log_function_t log_function, log_level_e threshold);

/// @brief Unsubscribe the specified function from the log facility.
/// @param log_function Function to unregister
/// @return `LOG_ERROR_NONE` on success.
log_error_e log_unsubscribe(log_function_t log_function);

/// @brief Helper function to get a printable version of the available Log levels. Useful for the client's
/// implementations
/// @param severity Log level to stringify
/// @return stringified version of the passed log level, or "UNK" if its unrecognized.
const char* log_level_to_str(const log_level_e severity);

/// @brief Private implementation of the log facility, used to print messages. This function should not be called. Use
/// the `LOG_X()` macros instead.
/// @param severity Message severity
/// @param fmt String to format
/// @param
void log_message(log_level_e severity, const char* fmt, ...);

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
