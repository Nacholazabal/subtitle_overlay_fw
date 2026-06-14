#include <string.h>

#include "unity.h"
#include "errorno.h"
#include "subtitle_overlay.h"

#include "mock_hw_platform.h"

#define REG_POS_INDEX        (0x00U / sizeof(uint32_t))
#define REG_SIZE_INDEX       (0x04U / sizeof(uint32_t))
#define REG_BAR_COLOR_INDEX  (0x08U / sizeof(uint32_t))
#define REG_TEXT_COLOR_INDEX (0x0CU / sizeof(uint32_t))
#define REG_CTRL_INDEX       (0x10U / sizeof(uint32_t))

static subtitle_overlay_t overlay;
static uint32_t regs[5];

void setUp(void)
{
    memset(&overlay, 0, sizeof(overlay));
    memset(regs, 0, sizeof(regs));
    overlay.base = (uintptr_t)regs;
}

void tearDown(void)
{}

void test_subtitle_overlay_init_uses_mapped_platform_region(void)
{
    memset(&overlay, 0, sizeof(overlay));
    hw_platform_base_ExpectAndReturn(HW_REGION_OVERLAY, (uintptr_t)regs);

    TEST_ASSERT_EQUAL_INT(0, subtitle_overlay_init(&overlay));
    TEST_ASSERT_EQUAL_UINT((uintptr_t)regs, overlay.base);
}

void test_subtitle_overlay_init_rejects_null_and_missing_region(void)
{
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_overlay_init(NULL));

    hw_platform_base_ExpectAndReturn(HW_REGION_OVERLAY, (uintptr_t)0);
    TEST_ASSERT_EQUAL_INT(-EIO, subtitle_overlay_init(&overlay));
}

void test_subtitle_overlay_configure_writes_register_map(void)
{
    subtitle_overlay_config_t config = {
        .x = 11U,
        .y = 22U,
        .width = 333U,
        .height = 44U,
        .bar_color = 0x00112233U,
        .text_color = 0x00ABCDEFU,
    };

    TEST_ASSERT_EQUAL_INT(0, subtitle_overlay_configure(&overlay, &config));

    TEST_ASSERT_EQUAL_UINT32((22U << 16U) | 11U, regs[REG_POS_INDEX]);
    TEST_ASSERT_EQUAL_UINT32((44U << 16U) | 333U, regs[REG_SIZE_INDEX]);
    TEST_ASSERT_EQUAL_UINT32(0x00112233U, regs[REG_BAR_COLOR_INDEX]);
    TEST_ASSERT_EQUAL_UINT32(0x00ABCDEFU, regs[REG_TEXT_COLOR_INDEX]);
}

void test_subtitle_overlay_configure_rejects_invalid_arguments(void)
{
    subtitle_overlay_config_t config = {
        .x = 1U,
        .y = 2U,
        .width = 3U,
        .height = 4U,
        .bar_color = 0x00000000U,
        .text_color = 0x00FFFFFFU,
    };

    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_overlay_configure(NULL, &config));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_overlay_configure(&overlay, NULL));

    config.width = 0U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_overlay_configure(&overlay, &config));
    config.width = 3U;

    config.x = 0x10000U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_overlay_configure(&overlay, &config));
    config.x = 1U;

    config.bar_color = 0x01000000U;
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_overlay_configure(&overlay, &config));
}

void test_subtitle_overlay_enable_preserves_other_control_bits(void)
{
    regs[REG_CTRL_INDEX] = SUBTITLE_OVERLAY_CTRL_SOF;

    TEST_ASSERT_EQUAL_INT(0, subtitle_overlay_enable(&overlay, 1));
    TEST_ASSERT_EQUAL_UINT32(SUBTITLE_OVERLAY_CTRL_SOF | SUBTITLE_OVERLAY_CTRL_ENABLE,
                             regs[REG_CTRL_INDEX]);

    TEST_ASSERT_EQUAL_INT(0, subtitle_overlay_enable(&overlay, 0));
    TEST_ASSERT_EQUAL_UINT32(SUBTITLE_OVERLAY_CTRL_SOF, regs[REG_CTRL_INDEX]);
}

void test_subtitle_overlay_read_and_clear_sof(void)
{
    uint32_t control = 0U;

    regs[REG_CTRL_INDEX] = SUBTITLE_OVERLAY_CTRL_SOF | SUBTITLE_OVERLAY_CTRL_ENABLE;

    TEST_ASSERT_EQUAL_INT(0, subtitle_overlay_read_control(&overlay, &control));
    TEST_ASSERT_EQUAL_UINT32(SUBTITLE_OVERLAY_CTRL_SOF | SUBTITLE_OVERLAY_CTRL_ENABLE, control);

    TEST_ASSERT_EQUAL_INT(0, subtitle_overlay_clear_sof(&overlay));
    TEST_ASSERT_EQUAL_UINT32(SUBTITLE_OVERLAY_CTRL_ENABLE, regs[REG_CTRL_INDEX]);
}

void test_subtitle_overlay_wait_sof_reports_success_or_timeout(void)
{
    regs[REG_CTRL_INDEX] = SUBTITLE_OVERLAY_CTRL_SOF;
    TEST_ASSERT_EQUAL_INT(0, subtitle_overlay_wait_sof(&overlay, 1U));

    regs[REG_CTRL_INDEX] = 0U;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, subtitle_overlay_wait_sof(&overlay, 2U));
}

void test_subtitle_overlay_rejects_invalid_control_arguments(void)
{
    uint32_t control;

    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_overlay_enable(NULL, 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_overlay_read_control(NULL, &control));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_overlay_read_control(&overlay, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_overlay_clear_sof(NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_overlay_wait_sof(NULL, 1U));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subtitle_overlay_wait_sof(&overlay, 0U));

    overlay.base = (uintptr_t)0;
    TEST_ASSERT_EQUAL_INT(-ESTATE, subtitle_overlay_enable(&overlay, 1));
}
