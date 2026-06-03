#include "video_vtc.h"

#include <string.h>

#include "hw_platform.h"
#include "xvtc_hw.h"

/**
 * @brief Initialize an imported Xilinx VTC instance with a mapped EffectiveAddr.
 * @param vtc VTC adapter to initialize.
 * @param device_id Xilinx VTC device ID from xparameters.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, XST_DEVICE_NOT_FOUND for unknown ID, or XST_FAILURE on mapping/init failure.
 */
int video_vtc_init(video_vtc_t* const vtc, uint16_t device_id)
{
    XVtc_Config* config;
    uintptr_t effective_address;

    if (vtc == NULL)
    {
        return XST_INVALID_PARAM;
    }

    memset(vtc, 0, sizeof(*vtc));
    config = XVtc_LookupConfig(device_id);
    if (config == NULL)
    {
        return XST_DEVICE_NOT_FOUND;
    }

    effective_address = hw_platform_translate(config->BaseAddress);
    if (effective_address == (uintptr_t)0)
    {
        return XST_FAILURE;
    }

    if (XVtc_CfgInitialize(&vtc->instance, config, effective_address) != XST_SUCCESS)
    {
        return XST_FAILURE;
    }

    vtc->device_id = device_id;
    vtc->initialized = 1;
    return XST_SUCCESS;
}

/**
 * @brief Program output-generator timing for one video mode.
 * @param vtc Initialized output VTC adapter.
 * @param mode Video timing values to apply.
 * @return XST_SUCCESS on success, or XST_INVALID_PARAM for bad input/uninitialized VTC.
 */
int video_vtc_configure_generator(video_vtc_t* const vtc, video_vtc_mode_t const* const mode)
{
    XVtc_Timing timing;
    XVtc_SourceSelect source;

    if ((vtc == NULL) || (mode == NULL) || !vtc->initialized)
    {
        return XST_INVALID_PARAM;
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

    return XST_SUCCESS;
}

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
 * @return XST_SUCCESS on success, or XST_INVALID_PARAM for bad input/uninitialized VTC.
 */
int video_vtc_start_detector(video_vtc_t* const vtc)
{
    if ((vtc == NULL) || !vtc->initialized)
    {
        return XST_INVALID_PARAM;
    }

    XVtc_RegUpdateEnable(&vtc->instance);
    XVtc_EnableDetector(&vtc->instance);
    XVtc_Enable(&vtc->instance);

    return XST_SUCCESS;
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
 * @return XST_SUCCESS on valid timing, XST_NO_DATA when unlocked/invalid, or XST_INVALID_PARAM for bad input.
 */
int video_vtc_read_detector_timing(video_vtc_t* const vtc, video_vtc_timing_t* const timing)
{
    XVtc_Timing raw_timing;

    if ((vtc == NULL) || (timing == NULL) || !vtc->initialized)
    {
        return XST_INVALID_PARAM;
    }

    if (!video_vtc_detector_locked(vtc))
    {
        return XST_NO_DATA;
    }

    memset(&raw_timing, 0, sizeof(raw_timing));
    XVtc_GetDetectorTiming(&vtc->instance, &raw_timing);

    if ((raw_timing.HActiveVideo == 0U) || (raw_timing.VActiveVideo == 0U))
    {
        return XST_NO_DATA;
    }

    timing->width = raw_timing.HActiveVideo;
    timing->height = raw_timing.VActiveVideo;

    return XST_SUCCESS;
}
