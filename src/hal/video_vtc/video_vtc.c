/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

**********************************************************************************************************************/

///
/// @file video_vtc.c
/// @brief Video Timing Controller HAL adapter implementation
///

// === Headers files inclusions ==================================================================================== //

#include "video_vtc.h"

#include <string.h>

#include "hw_platform.h"
#include "xparameters.h"
#include "xvtc_hw.h"

// === Macros definitions ========================================================================================== //
// === Private data type declarations ============================================================================== //
// === Private variable declarations =============================================================================== //
// === Private function declarations =============================================================================== //
// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //
// === Public function implementation ============================================================================== //

/**
 * @brief Initialize an imported Xilinx VTC instance with a mapped EffectiveAddr.
 * @param vtc VTC adapter to initialize.
 * @param device_id Xilinx VTC device ID from xparameters.
 * @return 0 on success, -EINVAL for bad input, -ENODEV for unknown ID, or -EIO on mapping/init failure.
 */
static int video_vtc_init(video_vtc_t* const vtc, uint16_t device_id)
{
    XVtc_Config* config;
    uintptr_t effective_address;

    if (vtc == NULL)
    {
        return -EINVAL;
    }

    memset(vtc, 0, sizeof(*vtc));
    config = XVtc_LookupConfig(device_id);
    if (config == NULL)
    {
        return -ENODEV;
    }

    effective_address = hw_platform_translate(config->BaseAddress);
    if (effective_address == (uintptr_t)0)
    {
        return -EIO;
    }

    if (XVtc_CfgInitialize(&vtc->instance, config, effective_address) != 0)
    {
        return -EIO;
    }

    vtc->initialized = 1;
    return 0;
}

/**
 * @brief Initialize VTC for input timing detection.
 * @param vtc VTC adapter to initialize.
 * @return 0 on success, -EINVAL for bad input, -ENODEV for unknown device, or -EIO on init failure.
 */
int video_vtc_init_detector(video_vtc_t* const vtc)
{
    return video_vtc_init(vtc, XPAR_V_TC_1_DEVICE_ID);
}

/**
 * @brief Initialize VTC for output timing generation.
 * @param vtc VTC adapter to initialize.
 * @return 0 on success, -EINVAL for bad input, -ENODEV for unknown device, or -EIO on init failure.
 */
int video_vtc_init_generator(video_vtc_t* const vtc)
{
    return video_vtc_init(vtc, XPAR_V_TC_0_DEVICE_ID);
}

/**
 * @brief Program output-generator timing for one video mode.
 * @param vtc Initialized output VTC adapter.
 * @param mode Video timing values to apply.
 * @return 0 on success, or -EINVAL for bad input/uninitialized VTC.
 */
int video_vtc_configure_generator(video_vtc_t* const vtc, video_vtc_mode_t const* const mode)
{
    XVtc_Timing timing;
    XVtc_SourceSelect source;

    if ((vtc == NULL) || (mode == NULL) || !vtc->initialized)
    {
        return -EINVAL;
    }

    memset(&timing, 0, sizeof(timing));
    timing.HActiveVideo = mode->width;
    timing.HFrontPorch = mode->hps - mode->width;
    timing.HSyncWidth = mode->hpe - mode->hps;
    timing.HBackPorch = mode->hmax - mode->hpe + 1U;
    timing.HSyncPolarity = mode->hpol;
    timing.VActiveVideo = mode->height;
    timing.V0FrontPorch = mode->vps - mode->height;
    timing.V0SyncWidth = mode->vpe - mode->vps;
    timing.V0BackPorch = mode->vmax - mode->vpe + 1U;
    timing.V1FrontPorch = timing.V0FrontPorch;
    timing.V1SyncWidth = timing.V0SyncWidth;
    timing.V1BackPorch = timing.V0BackPorch;
    timing.VSyncPolarity = mode->vpol;
    timing.Interlaced = 0U;

    memset(&source, 0, sizeof(source));
    source.VBlankPolSrc = 1;
    source.VSyncPolSrc = 1;
    source.HBlankPolSrc = 1;
    source.HSyncPolSrc = 1;
    source.ActiveVideoPolSrc = 1;
    source.ActiveChromaPolSrc = 1;
    source.VChromaSrc = 1;
    source.VActiveSrc = 1;
    source.VBackPorchSrc = 1;
    source.VSyncSrc = 1;
    source.VFrontPorchSrc = 1;
    source.VTotalSrc = 1;
    source.HActiveSrc = 1;
    source.HBackPorchSrc = 1;
    source.HSyncSrc = 1;
    source.HFrontPorchSrc = 1;
    source.HTotalSrc = 1;
    source.FieldIdPolSrc = 1;
    source.InterlacedMode = 0;

    XVtc_RegUpdateEnable(&vtc->instance);
    XVtc_SetGeneratorTiming(&vtc->instance, &timing);
    XVtc_SetSource(&vtc->instance, &source);

    return 0;
}

// === End of documentation ======================================================================================== //

/**
 * @brief Enable the VTC generator and core.
 * @param vtc Initialized output VTC adapter.
 * @return None.
 */
void video_vtc_start_generator(video_vtc_t* const vtc)
{
    if ((vtc == NULL) || !vtc->initialized)
    {
        return;
    }

    XVtc_EnableGenerator(&vtc->instance);
    XVtc_Enable(&vtc->instance);
}

/**
 * @brief Disable the VTC generator.
 * @param vtc Initialized output VTC adapter.
 * @return None.
 */
void video_vtc_stop_generator(video_vtc_t* const vtc)
{
    if ((vtc == NULL) || !vtc->initialized)
    {
        return;
    }

    XVtc_DisableGenerator(&vtc->instance);
}

/**
 * @brief Enable the VTC detector and core.
 * @param vtc Initialized input VTC adapter.
 * @return 0 on success, or -EINVAL for bad input/uninitialized VTC.
 */
int video_vtc_start_detector(video_vtc_t* const vtc)
{
    if ((vtc == NULL) || !vtc->initialized)
    {
        return -EINVAL;
    }

    XVtc_RegUpdateEnable(&vtc->instance);
    XVtc_EnableDetector(&vtc->instance);
    XVtc_Enable(&vtc->instance);

    return 0;
}

/**
 * @brief Query whether the VTC detector reports timing lock.
 * @param vtc Initialized input VTC adapter.
 * @return Nonzero when locked, zero otherwise.
 */
int video_vtc_detector_locked(video_vtc_t* const vtc)
{
    if ((vtc == NULL) || !vtc->initialized)
    {
        return 0;
    }

    return (XVtc_GetDetectionStatus(&vtc->instance) & XVTC_STAT_LOCKED_MASK) != 0U;
}

/**
 * @brief Read active input dimensions from the VTC detector.
 * @param vtc Initialized input VTC adapter.
 * @param timing Output active width and height.
 * @return 0 on valid timing, -ENODATA when unlocked/invalid, or -EINVAL for bad input.
 */
int video_vtc_read_detector_timing(video_vtc_t* const vtc, video_vtc_timing_t* const timing)
{
    XVtc_Timing raw_timing;

    if ((vtc == NULL) || (timing == NULL) || !vtc->initialized)
    {
        return -EINVAL;
    }

    if (!video_vtc_detector_locked(vtc))
    {
        return -ENODATA;
    }

    memset(&raw_timing, 0, sizeof(raw_timing));
    XVtc_GetDetectorTiming(&vtc->instance, &raw_timing);

    if ((raw_timing.HActiveVideo == 0U) || (raw_timing.VActiveVideo == 0U))
    {
        return -ENODATA;
    }

    timing->width = raw_timing.HActiveVideo;
    timing->height = raw_timing.VActiveVideo;

    return 0;
}
