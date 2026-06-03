/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file video_modes.c
/// @brief Supported video mode table implementation
///

// === Headers files inclusions ==================================================================================== //

#include "video_modes.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //

static const video_pipeline_mode_t modes[] = {
    {
        "640x480@60Hz",
        {
            640U, 480U,
            656U, 752U, 799U, 0U,
            490U, 492U, 524U, 0U,
            25.0,
        },
    },
    {
        "800x600@60Hz",
        {
            800U, 600U,
            840U, 968U, 1055U, 1U,
            601U, 605U, 627U, 1U,
            40.0,
        },
    },
    {
        "1280x720@60Hz",
        {
            1280U, 720U,
            1390U, 1430U, 1649U, 1U,
            725U, 730U, 749U, 1U,
            74.25,
        },
    },
    {
        "1280x1024@60Hz",
        {
            1280U, 1024U,
            1328U, 1440U, 1687U, 1U,
            1025U, 1028U, 1065U, 1U,
            108.0,
        },
    },
    {
        "1920x1080@60Hz",
        {
            1920U, 1080U,
            2008U, 2052U, 2199U, 1U,
            1084U, 1089U, 1124U, 1U,
            148.5,
        },
    },
};

// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

/**
 * @brief Return the default display mode used by the video pipeline.
 * @param None.
 * @return Pointer to the default mode.
 */
video_pipeline_mode_t const* video_modes_default(void)
{
    return &modes[0];
}

/**
 * @brief Find a supported mode by exact active resolution.
 * @param width Active width in pixels.
 * @param height Active height in lines.
 * @return Pointer to the matching mode, or NULL when unsupported.
 */
video_pipeline_mode_t const* video_modes_find(uint32_t width, uint32_t height)
{
    size_t i;

    for (i = 0U; i < (sizeof(modes) / sizeof(modes[0])); i++)
    {
        if ((modes[i].timing.width == width) && (modes[i].timing.height == height))
        {
            return &modes[i];
        }
    }

    return NULL;
}

/**
 * @brief Return the full supported mode table.
 * @param count Optional output receiving the number of modes.
 * @return Pointer to the first mode in the static table.
 */
video_pipeline_mode_t const* video_modes_all(size_t* const count)
{
    if (count != NULL)
    {
        *count = sizeof(modes) / sizeof(modes[0]);
    }

    return modes;
}

// === End of documentation ======================================================================================== //
