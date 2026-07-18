// Validation tests for video_dynclk_configure (SRC-M08).
//
// video_dynclk is normally mocked (not in the test :source set), so this test
// compiles the real translation unit directly (via #include) and exercises the
// input-validation early-returns, which reject bad frequencies BEFORE any MMIO
// access. Xil_In32/Xil_Out32 are volatile-pointer macros (no external symbol),
// so only hw_platform_base needs a stub to satisfy the link.

#include "unity.h"

#include <math.h>

// Stub the only external HAL symbol referenced by the translation unit; the
// validation paths under test never call it.
#include "hw_platform.h"
uintptr_t hw_platform_base(hw_platform_region_e region)
{
    (void)region;
    return (uintptr_t)0;
}
uintptr_t hw_platform_translate(uint32_t physical_address)
{
    (void)physical_address;
    return (uintptr_t)0;
}
int hw_platform_init(void)
{
    return 0;
}
void hw_platform_cleanup(void)
{
}

#include "video_dynclk.c"

static video_dynclk_t dynclk;

void setUp(void)
{
    dynclk.base = (uintptr_t)0x1000; // non-zero so validation reaches the freq checks
    dynclk.actual_frequency_mhz = 0.0;
}

void tearDown(void)
{
}

void test_configure_rejects_null_and_unmapped(void)
{
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dynclk_configure(NULL, 148.5));

    video_dynclk_t unmapped = {.base = (uintptr_t)0, .actual_frequency_mhz = 0.0};
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dynclk_configure(&unmapped, 148.5));
}

void test_configure_rejects_non_finite_frequency(void)
{
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dynclk_configure(&dynclk, NAN));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dynclk_configure(&dynclk, INFINITY));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dynclk_configure(&dynclk, -INFINITY));
}

void test_configure_rejects_zero_negative_and_out_of_range(void)
{
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dynclk_configure(&dynclk, 0.0));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dynclk_configure(&dynclk, -148.5));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dynclk_configure(&dynclk, 400.0));
}
