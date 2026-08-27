/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

#pragma once

///
/// @file template_qpc_AO.h
/// @brief Template header file for a QP/C active object
///

// === Headers files inclusions ==================================================================================== //

/*
 * Shared signals, event payloads, AO_* pointers, and constructors normally
 * belong in app.h. Keep AO-private data and state handlers in the AO .c file.
 */

// === C++ Guard =================================================================================================== //

#ifdef __cplusplus
extern "C" {
#endif

// === Public macros definitions =================================================================================== //

// Add rare AO-specific public macros here if another module needs them.

// === Public data type declarations =============================================================================== //

// Add rare AO-specific public types here. Prefer shared event payloads in app.h.

// === Public variable declarations ================================================================================ //

// Add rare AO-specific public variables here. Prefer the opaque AO_* pointer in app.h.

// === Public function declarations ================================================================================ //

// Add rare AO-specific helper APIs here. Prefer the AO constructor declaration in app.h.

// === End of documentation ======================================================================================== //

#ifdef __cplusplus
}
#endif
