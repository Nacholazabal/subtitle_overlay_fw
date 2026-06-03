#ifndef HW_PLATFORM_H_
#define HW_PLATFORM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * AXI-Lite regions controlled directly by the Linux userspace application.
 *
 * AXI VDMA and its framebuffers are intentionally absent. Linux owns those
 * resources through the xilinx-vdma driver and /dev/hdmi-vdma.
 */
typedef enum
{
    HW_REGION_DYNCLK = 0,
    HW_REGION_VTC_OUTPUT,
    HW_REGION_VTC_INPUT,
    HW_REGION_OVERLAY,
    HW_REGION_SUBTITLE_BRAM,
    HW_REGION_VIDEO_GPIO,
    HW_REGION_COUNT
} hw_platform_region_e;

/**
 * Open /dev/mem and map each userspace-owned AXI-Lite region.
 *
 * @return 0 on success, or -1 after cleaning up any partial initialization.
 */
int hw_platform_init(void);

/**
 * Unmap every AXI-Lite region and close /dev/mem.
 */
void hw_platform_cleanup(void);

/**
 * Return the mapped virtual base for one AXI-Lite region.
 *
 * @return The virtual base, or 0 if the region is invalid or not mapped.
 */
uintptr_t hw_platform_base(hw_platform_region_e region);

/**
 * Translate a physical AXI-Lite address into its mapped virtual address.
 *
 * This is used when an imported Xilinx BSP API expects an EffectiveAddr.
 *
 * @return The virtual address, or 0 when the address is not mapped.
 */
uintptr_t hw_platform_translate(uint32_t physical_address);

#ifdef __cplusplus
}
#endif

#endif /* HW_PLATFORM_H_ */
