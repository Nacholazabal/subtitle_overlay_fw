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

#include "qpc.h"

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //
// === Public data type declarations =============================================================================== //

/// @brief Components coordinated by SystemAO during application startup.
typedef enum
{
    COMPONENT_NONE = 0,
    COMPONENT_VIDEO,
    COMPONENT_USB_AUDIO,
    COMPONENT_SUBTITLE_PIPELINE,
    COMPONENT_BUTTONS,
    COMPONENT_LED,
} ComponentId;

/// @brief Signals shared by the application active objects.
typedef enum
{
    COMPONENT_INIT_SIG = Q_USER_SIG,  ///< Directed command: initialize the receiving component.
    COMPONENT_READY_SIG,              ///< Directed response: one component finished initialization.
    COMPONENT_ERROR_SIG,              ///< Directed error report containing an AppErrorEvt payload.

    MAX_SIG
} AppSignals;

/// @brief Payload sent to SystemAO after a component finishes initializing.
typedef struct
{
    QEvt super;
    ComponentId source;
} ComponentReadyEvt;

/// @brief Dynamically allocated payload sent to SystemAO when a component fails.
typedef struct
{
    QEvt super;
    ComponentId source;
    uint32_t code;
} AppErrorEvt;

// === Public variable declarations ================================================================================ //

/// @brief Opaque handle used to post events to SystemAO.
extern QActive * const AO_System;

// === Public function declarations ================================================================================ //

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
