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
    uint32_t physical_base;
    size_t size;
    void* virtual_base;
} hw_platform_mapping_t;

static hw_platform_mapping_t mappings[HW_REGION_COUNT] = {
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

static int devmem_fd = -1;

static void* map_region(hw_platform_mapping_t* const mapping)
{
    void* const virtual_base = mmap(NULL,
                                   mapping->size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   devmem_fd,
                                   (off_t)mapping->physical_base);

    if (virtual_base == MAP_FAILED)
    {
        fprintf(stderr,
                "[hw_platform] mmap %s at 0x%08X failed: %s\n",
                mapping->name,
                mapping->physical_base,
                strerror(errno));
        return NULL;
    }

    return virtual_base;
}

int hw_platform_init(void)
{
    hw_platform_region_e region;

    if (devmem_fd >= 0)
    {
        return 0;
    }

    devmem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (devmem_fd < 0)
    {
        fprintf(stderr, "[hw_platform] open /dev/mem failed: %s\n", strerror(errno));
        return -1;
    }

    for (region = HW_REGION_DYNCLK; region < HW_REGION_COUNT; region++)
    {
        mappings[region].virtual_base = map_region(&mappings[region]);
        if (mappings[region].virtual_base == NULL)
        {
            hw_platform_cleanup();
            return -1;
        }
    }

    return 0;
}

void hw_platform_cleanup(void)
{
    hw_platform_region_e region;

    for (region = HW_REGION_DYNCLK; region < HW_REGION_COUNT; region++)
    {
        if (mappings[region].virtual_base != NULL)
        {
            munmap(mappings[region].virtual_base, mappings[region].size);
            mappings[region].virtual_base = NULL;
        }
    }

    if (devmem_fd >= 0)
    {
        close(devmem_fd);
        devmem_fd = -1;
    }
}

uintptr_t hw_platform_base(hw_platform_region_e region)
{
    if ((region < HW_REGION_DYNCLK) || (region >= HW_REGION_COUNT))
    {
        return (uintptr_t)0;
    }

    return (uintptr_t)mappings[region].virtual_base;
}

uintptr_t hw_platform_translate(uint32_t physical_address)
{
    hw_platform_region_e region;

    for (region = HW_REGION_DYNCLK; region < HW_REGION_COUNT; region++)
    {
        hw_platform_mapping_t const* const mapping = &mappings[region];

        if (physical_address >= mapping->physical_base)
        {
            uint32_t const offset = physical_address - mapping->physical_base;

            if ((offset < mapping->size) && (mapping->virtual_base != NULL))
            {
                return (uintptr_t)mapping->virtual_base + offset;
            }
        }
    }

    return (uintptr_t)0;
}
