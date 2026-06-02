#include "hw_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../xparameters_linux.h"

typedef struct
{
    const char* name;
    uint32_t physicalBase;
    size_t size;
    void* virtualBase;
} HwPlatformMapping;

static HwPlatformMapping mappings[HW_REGION_COUNT] = {
    [HW_REGION_DYNCLK] = {"DynClk", XPAR_AXI_DYNCLK_0_BASEADDR, 0x00010000U, NULL},
    [HW_REGION_VTC_OUTPUT] = {"VTC output", XPAR_V_TC_0_BASEADDR, 0x00010000U, NULL},
    [HW_REGION_VTC_INPUT] = {"VTC input", XPAR_V_TC_1_BASEADDR, 0x00010000U, NULL},
    [HW_REGION_OVERLAY] = {"Overlay", XPAR_AXIS_VIDEO_OVERLAY_R_0_BASEADDR, 0x00010000U, NULL},
    [HW_REGION_SUBTITLE_BRAM] =
        {
            "Subtitle BRAM",
            XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR,
            0x00002000U,
            NULL,
        },
    [HW_REGION_VIDEO_GPIO] = {"Video GPIO", XPAR_AXI_GPIO_VIDEO_BASEADDR, 0x00010000U, NULL},
};

static int devmemFd = -1;

static void* map_region(HwPlatformMapping* const mapping)
{
    void* const virtualBase = mmap(NULL,
                                   mapping->size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   devmemFd,
                                   (off_t)mapping->physicalBase);

    if (virtualBase == MAP_FAILED)
    {
        fprintf(stderr,
                "[hw_platform] mmap %s at 0x%08X failed: %s\n",
                mapping->name,
                mapping->physicalBase,
                strerror(errno));
        return NULL;
    }

    return virtualBase;
}

int hw_platform_init(void)
{
    HwPlatformRegion region;

    if (devmemFd >= 0)
    {
        return 0;
    }

    devmemFd = open("/dev/mem", O_RDWR | O_SYNC);
    if (devmemFd < 0)
    {
        fprintf(stderr, "[hw_platform] open /dev/mem failed: %s\n", strerror(errno));
        return -1;
    }

    for (region = HW_REGION_DYNCLK; region < HW_REGION_COUNT; region++)
    {
        mappings[region].virtualBase = map_region(&mappings[region]);
        if (mappings[region].virtualBase == NULL)
        {
            hw_platform_cleanup();
            return -1;
        }
    }

    return 0;
}

void hw_platform_cleanup(void)
{
    HwPlatformRegion region;

    for (region = HW_REGION_DYNCLK; region < HW_REGION_COUNT; region++)
    {
        if (mappings[region].virtualBase != NULL)
        {
            munmap(mappings[region].virtualBase, mappings[region].size);
            mappings[region].virtualBase = NULL;
        }
    }

    if (devmemFd >= 0)
    {
        close(devmemFd);
        devmemFd = -1;
    }
}

uintptr_t hw_platform_base(HwPlatformRegion region)
{
    if ((region < HW_REGION_DYNCLK) || (region >= HW_REGION_COUNT))
    {
        return (uintptr_t)0;
    }

    return (uintptr_t)mappings[region].virtualBase;
}

uintptr_t hw_platform_translate(uint32_t physical_address)
{
    HwPlatformRegion region;

    for (region = HW_REGION_DYNCLK; region < HW_REGION_COUNT; region++)
    {
        HwPlatformMapping const* const mapping = &mappings[region];

        if (physical_address >= mapping->physicalBase)
        {
            uint32_t const offset = physical_address - mapping->physicalBase;

            if ((offset < mapping->size) && (mapping->virtualBase != NULL))
            {
                return (uintptr_t)mapping->virtualBase + offset;
            }
        }
    }

    return (uintptr_t)0;
}
