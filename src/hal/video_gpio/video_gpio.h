#pragma once

#include <stdint.h>

#include "xstatus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uintptr_t base;
} video_gpio_t;

int video_gpio_init(video_gpio_t* gpio);
void video_gpio_set_hpd(video_gpio_t* gpio, int enabled);
int video_gpio_is_locked(video_gpio_t const* gpio);

#ifdef __cplusplus
}
#endif
