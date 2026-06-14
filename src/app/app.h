/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

#pragma once

///
/// @file app.h
/// @brief Shared QP/C application contracts
///

// === Headers files inclusions ==================================================================================== //

#include <stdint.h>

#include "errorno.h"
#include "qpc.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

#define SUBTITLE_TEXT_MAX_LEN (128U)

// === Public data type declarations =============================================================================== //

/// @brief Components coordinated by system_ao_t during application startup.
typedef enum
{
    COMPONENT_NONE = 0,
    COMPONENT_VIDEO,
    COMPONENT_USB_AUDIO,
    COMPONENT_SUBTITLE_PIPELINE,
    COMPONENT_STT,
    COMPONENT_BUTTONS,
    COMPONENT_LED,
} component_id_e;

/// @brief Signals shared by the application active objects.
typedef enum
{
    COMPONENT_INIT_SIG = Q_USER_SIG, ///< Directed command: initialize the receiving component.
    COMPONENT_READY_SIG,             ///< Directed response: one component finished initialization.
    COMPONENT_ERROR_SIG, ///< Directed error report containing an app_error_evt_t payload.
    VIDEO_POLL_SIG,      ///< Private video AO timer signal used to poll the video pipeline.
    STT_POLL_SIG,        ///< Private STT AO timer signal used to poll TCP transcript input.
    SUBTITLE_TEXT_SIG,   ///< Directed event from stt_ao_t to subtitle_ao_t.

    MAX_SIG
} app_signal_e;

/// @brief Payload sent to system_ao_t after a component finishes initializing.
typedef struct
{
    QEvt super;
    component_id_e source;
    uint32_t width;
    uint32_t height;
} component_ready_evt_t;

/// @brief Payload sent when one active object requests component initialization.
typedef struct
{
    QEvt super;
    component_id_e source;
    uint32_t width;
    uint32_t height;
} component_init_evt_t;

/// @brief Dynamically allocated payload sent to system_ao_t when a component fails.
typedef struct
{
    QEvt super;
    component_id_e source;
    int32_t code;
} app_error_evt_t;

/// @brief Transcript/subtitle text payload exchanged by STT and subtitle active objects.
typedef struct
{
    QEvt super;
    uint32_t seq;
    uint32_t start_ms;
    uint32_t end_ms;
    uint8_t is_final;
    char text[SUBTITLE_TEXT_MAX_LEN];
} subtitle_text_evt_t;

// === Public variable declarations ================================================================================ //

/// @brief Opaque handle used to post events to system_ao_t.
extern QActive* const AO_System;

// === Public function declarations ================================================================================ //

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
