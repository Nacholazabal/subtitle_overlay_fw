// Tests for the dynamic pixel clock HAL adapter (SRC-M08).
//
// video_dynclk is normally mocked (not in the test :source set), so this test
// compiles the real translation unit directly (via #include). Xil_In32/Xil_Out32
// are volatile-pointer macros, so xil_io.h is included first (setting its guard)
// and the two accessors are then redirected at a memory-backed fake register
// file. That lets the register-programming and lock-polling paths run for real
// instead of faulting on an unmapped address.

#include "unity.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "xil_io.h"

#include "hw_platform.h"

// --- hw_platform stub -------------------------------------------------------
// Only hw_platform_base is exercised; the rest satisfy the link.
static uintptr_t fake_platform_base;

uintptr_t hw_platform_base(hw_platform_region_e region)
{
    (void)region;
    return fake_platform_base;
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

// --- memory-backed MMIO fake ------------------------------------------------
// The adapter drives an 8-register AXI-Lite window (CTRL..FLTR_LOCK_H).
#define FAKE_REG_COUNT   16U
#define FAKE_OFST_CTRL   0x00U
#define FAKE_OFST_STATUS 0x04U

static uint32_t fake_regs[FAKE_REG_COUNT];
static uint32_t fake_write_count;

// Scripted STATUS reads. video_dynclk_stop() spins until the RUNNING bit is
// clear and clk_start() spins until it is set, so one static value cannot drive
// both halves of a configure(). The script yields one value per STATUS read and
// sticks on its last entry, so every polling loop still terminates.
static uint32_t status_script[4];
static unsigned status_script_len;
static unsigned status_script_idx;

static void script_status(uint32_t first, uint32_t second, unsigned count)
{
    status_script[0] = first;
    status_script[1] = second;
    status_script_len = count;
    status_script_idx = 0U;
}

static void fake_out32(uintptr_t addr, uint32_t value)
{
    uintptr_t const offset = addr - (uintptr_t)fake_regs;

    fake_write_count++;
    fake_regs[offset / 4U] = value;
}

static uint32_t fake_in32(uintptr_t addr)
{
    uintptr_t const offset = addr - (uintptr_t)fake_regs;

    if ((offset == FAKE_OFST_STATUS) && (status_script_len != 0U))
    {
        uint32_t const value = status_script[status_script_idx];

        if ((status_script_idx + 1U) < status_script_len)
        {
            status_script_idx++;
        }
        return value;
    }

    return fake_regs[offset / 4U];
}

#undef Xil_Out32
#undef Xil_In32
#define Xil_Out32(Addr, Value) fake_out32((uintptr_t)(Addr), (uint32_t)(Value))
#define Xil_In32(Addr)         fake_in32((uintptr_t)(Addr))

#include "video_dynclk.c"

static video_dynclk_t dynclk;

void setUp(void)
{
    memset(fake_regs, 0, sizeof(fake_regs));
    fake_write_count = 0U;
    fake_platform_base = (uintptr_t)fake_regs;
    script_status(0U, 0U, 0U); // no script: STATUS reads fall through to fake_regs

    dynclk.base = (uintptr_t)fake_regs;
    dynclk.actual_frequency_mhz = 0.0;
}

void tearDown(void)
{
}

// === video_dynclk_configure: input validation ===

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

// === video_dynclk_configure: full programming path ===

void test_configure_programs_registers_and_starts_clock(void)
{
    // STATUS reads 0 first (so the pre-stop sees an idle core), then 1 (so the
    // PLL lock poll in clk_start succeeds).
    script_status(0U, 1U, 2U);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_dynclk_configure(&dynclk, 148.5));

    // Divider/lock/filter registers were programmed.
    TEST_ASSERT_NOT_EQUAL(0U, fake_regs[2]); // CLK_L
    TEST_ASSERT_NOT_EQUAL(0U, fake_regs[3]); // FB_L
    TEST_ASSERT_EQUAL_UINT32(0U, fake_regs[4]); // FB_H_CLK_H is always zero
    TEST_ASSERT_NOT_EQUAL(0U, fake_regs[5]); // DIV
    TEST_ASSERT_NOT_EQUAL(0U, fake_regs[6]); // LOCK_L
    TEST_ASSERT_NOT_EQUAL(0U, fake_regs[7]); // FLTR_LOCK_H

    // CTRL was left with the START bit set.
    TEST_ASSERT_EQUAL_UINT32(1U << BIT_DYNCLK_START, fake_regs[0]);

    // The synthesized frequency was recorded and lands close to the request.
    TEST_ASSERT_TRUE(fabs(dynclk.actual_frequency_mhz - 148.5) < 1.0);
}

void test_configure_reports_failure_when_pll_never_locks(void)
{
    // STATUS stays 0: the pre-stop succeeds, then clk_start times out waiting
    // for the RUNNING bit.
    script_status(0U, 0U, 1U);

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dynclk_configure(&dynclk, 148.5));
    TEST_ASSERT_TRUE(dynclk.actual_frequency_mhz == 0.0);
}

void test_configure_reports_failure_when_pre_stop_times_out(void)
{
    // STATUS stays 1: video_dynclk_stop() never sees the core go idle.
    script_status(1U, 1U, 1U);

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dynclk_configure(&dynclk, 148.5));
}

void test_configure_rejects_unsynthesizable_frequency(void)
{
    // 0.001 MHz has no valid divider combination, so the synthesis error blows
    // past DYNCLK_MAX_FREQ_ERR_MHZ and configure fails before touching MMIO.
    script_status(0U, 1U, 2U);

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dynclk_configure(&dynclk, 0.001));
}

// === video_dynclk_stop ===

void test_stop_rejects_null_and_unmapped(void)
{
    video_dynclk_t unmapped = {.base = (uintptr_t)0, .actual_frequency_mhz = 0.0};

    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dynclk_stop(NULL));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dynclk_stop(&unmapped));
}

void test_stop_clears_control_register_and_resets_frequency(void)
{
    script_status(0U, 0U, 1U); // already idle
    dynclk.actual_frequency_mhz = 148.5;
    fake_regs[0] = 0xFFFFFFFFU;

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_dynclk_stop(&dynclk));

    TEST_ASSERT_EQUAL_UINT32(0U, fake_regs[0]);
    TEST_ASSERT_TRUE(dynclk.actual_frequency_mhz == 0.0);
}

void test_stop_times_out_when_running_bit_never_clears(void)
{
    script_status(1U, 1U, 1U);
    dynclk.actual_frequency_mhz = 148.5;

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dynclk_stop(&dynclk));
    // The frequency is left untouched on a failed stop.
    TEST_ASSERT_TRUE(dynclk.actual_frequency_mhz > 0.0);
}

// === clk_divider ===

void test_clk_divider_rejects_zero_and_out_of_range(void)
{
    TEST_ASSERT_EQUAL_UINT32(ERR_CLKDIVIDER, clk_divider(0));
    TEST_ASSERT_EQUAL_UINT32(ERR_CLKDIVIDER, clk_divider(129));
    TEST_ASSERT_EQUAL_UINT32(ERR_CLKDIVIDER, clk_divider(255));
}

void test_clk_divider_handles_divide_by_1(void)
{
    TEST_ASSERT_EQUAL_UINT32(0x1041, clk_divider(1));
}

void test_clk_divider_even_dividers(void)
{
    // divide=2: high_time=1, low_time=1, no wedge
    uint32_t result = clk_divider(2);
    TEST_ASSERT_EQUAL_UINT32(1, result & 0x3F);                 // low_time
    TEST_ASSERT_EQUAL_UINT32(1 << 6, result & 0xFC0);           // high_time
    TEST_ASSERT_EQUAL_UINT32(0, result & (1 << CLK_BIT_WEDGE)); // no wedge

    // divide=4: high_time=2, low_time=2
    result = clk_divider(4);
    TEST_ASSERT_EQUAL_UINT32(2, result & 0x3F);
    TEST_ASSERT_EQUAL_UINT32(2 << 6, result & 0xFC0);
    TEST_ASSERT_EQUAL_UINT32(0, result & (1 << CLK_BIT_WEDGE));
}

void test_clk_divider_odd_dividers_set_wedge_bit(void)
{
    // divide=3: high_time=1, low_time=2, wedge bit set
    uint32_t result = clk_divider(3);
    TEST_ASSERT_EQUAL_UINT32(2, result & 0x3F);              // low_time
    TEST_ASSERT_EQUAL_UINT32(1 << 6, result & 0xFC0);        // high_time
    TEST_ASSERT_NOT_EQUAL(0, result & (1 << CLK_BIT_WEDGE)); // wedge set

    // divide=5: high_time=2, low_time=3, wedge bit set
    result = clk_divider(5);
    TEST_ASSERT_EQUAL_UINT32(3, result & 0x3F);
    TEST_ASSERT_EQUAL_UINT32(2 << 6, result & 0xFC0);
    TEST_ASSERT_NOT_EQUAL(0, result & (1 << CLK_BIT_WEDGE));
}

void test_clk_divider_max_valid_value(void)
{
    TEST_ASSERT_NOT_EQUAL(ERR_CLKDIVIDER, clk_divider(128));
}

// === clk_count_calc ===

void test_clk_count_calc_propagates_divider_errors(void)
{
    TEST_ASSERT_EQUAL_UINT32(ERR_CLKCOUNTCALC, clk_count_calc(0));
    TEST_ASSERT_EQUAL_UINT32(ERR_CLKCOUNTCALC, clk_count_calc(129));
}

void test_clk_count_calc_encodes_valid_divider(void)
{
    uint32_t div_result = clk_divider(4);
    uint32_t count_result = clk_count_calc(4);

    TEST_ASSERT_EQUAL_UINT32(div_result & 0xFFF, count_result & 0xFFF);
    TEST_ASSERT_EQUAL_UINT32((div_result << 10) & 0x00C00000, count_result & 0x00C00000);
}

// === clk_find_reg ===

void test_clk_find_reg_rejects_null_pointers(void)
{
    clk_config_t regs;
    clk_mode_t mode = {.freq = 148.5, .fbmult = 32, .clkdiv = 5, .maindiv = 2};

    TEST_ASSERT_EQUAL_INT(0, clk_find_reg(NULL, &mode));
    TEST_ASSERT_EQUAL_INT(0, clk_find_reg(&regs, NULL));
}

void test_clk_find_reg_rejects_invalid_fbmult(void)
{
    clk_config_t regs;
    clk_mode_t mode = {.freq = 148.5, .fbmult = 1, .clkdiv = 5, .maindiv = 2}; // fbmult < 2
    TEST_ASSERT_EQUAL_INT(0, clk_find_reg(&regs, &mode));

    mode.fbmult = 65; // fbmult > 64
    TEST_ASSERT_EQUAL_INT(0, clk_find_reg(&regs, &mode));
}

void test_clk_find_reg_rejects_unencodable_dividers(void)
{
    clk_config_t regs;
    // fbmult is in range but clkdiv cannot be encoded, so the register bundle
    // is rejected after the count calculations.
    clk_mode_t mode = {.freq = 148.5, .fbmult = 32, .clkdiv = 200, .maindiv = 2};
    TEST_ASSERT_EQUAL_INT(0, clk_find_reg(&regs, &mode));

    // Same for an unencodable main divider.
    mode.clkdiv = 5;
    mode.maindiv = 0;
    TEST_ASSERT_EQUAL_INT(0, clk_find_reg(&regs, &mode));
}

void test_clk_find_reg_accepts_valid_parameters(void)
{
    clk_config_t regs;
    clk_mode_t mode = {.freq = 148.5, .fbmult = 32, .clkdiv = 5, .maindiv = 2};

    TEST_ASSERT_EQUAL_INT(1, clk_find_reg(&regs, &mode));

    TEST_ASSERT_NOT_EQUAL(0, regs.clk0_l);
    TEST_ASSERT_NOT_EQUAL(0, regs.clk_fb_l);
    TEST_ASSERT_EQUAL_UINT32(0, regs.clk_fbh_clk0_h); // always zero per implementation
    TEST_ASSERT_NOT_EQUAL(0, regs.divclk);
    TEST_ASSERT_NOT_EQUAL(0, regs.lock_l);
    TEST_ASSERT_NOT_EQUAL(0, regs.fltr_lock_h);
}

void test_clk_find_reg_computes_lock_and_filter_values(void)
{
    clk_config_t regs;
    clk_mode_t mode = {.freq = 148.5, .fbmult = 10, .clkdiv = 5, .maindiv = 2};

    TEST_ASSERT_EQUAL_INT(1, clk_find_reg(&regs, &mode));

    // Lock/filter words come from the fbmult-1 lookup entry.
    uint64_t expected_lock = lock_lookup[9];
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(expected_lock & 0xFFFFFFFFULL), regs.lock_l);

    uint32_t expected_fltr_h = (uint32_t)((expected_lock >> 32) & 0x000000FFULL);
    expected_fltr_h |= (filter_lookup_low[9] << 16) & 0x03FF0000U;
    TEST_ASSERT_EQUAL_UINT32(expected_fltr_h, regs.fltr_lock_h);
}

// === clk_find_params ===

void test_clk_find_params_finds_valid_frequency(void)
{
    clk_mode_t mode;

    // 148.5 MHz is the 1080p60 pixel clock.
    double error = clk_find_params(148.5, &mode);

    TEST_ASSERT_TRUE(error < 1.0);
    TEST_ASSERT_NOT_EQUAL(0, mode.fbmult);
    TEST_ASSERT_NOT_EQUAL(0, mode.clkdiv);
    TEST_ASSERT_NOT_EQUAL(0, mode.maindiv);
    TEST_ASSERT_TRUE(mode.freq > 0.0);
    TEST_ASSERT_TRUE(fabs(mode.freq - 148.5) < 1.0);
}

void test_clk_find_params_respects_fbmult_range(void)
{
    clk_mode_t mode;

    double error = clk_find_params(100.0, &mode);

    TEST_ASSERT_GREATER_OR_EQUAL(2, mode.fbmult);
    TEST_ASSERT_LESS_OR_EQUAL(64, mode.fbmult);
    TEST_ASSERT_TRUE(error < 1.0);
}

void test_clk_find_params_handles_low_frequency(void)
{
    clk_mode_t mode;

    // 25.175 MHz is the VGA 640x480@60 pixel clock.
    double error = clk_find_params(25.175, &mode);

    TEST_ASSERT_TRUE(error < 1.0);
    TEST_ASSERT_TRUE(fabs(mode.freq - 25.175) < 1.0);
}

void test_clk_find_params_handles_high_frequency(void)
{
    clk_mode_t mode;

    double error = clk_find_params(200.0, &mode);

    TEST_ASSERT_TRUE(error < 10.0);
    TEST_ASSERT_TRUE(mode.freq > 0.0);
}

// === dynclk_now_ns ===

void test_dynclk_now_ns_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(XST_FAILURE, dynclk_now_ns(NULL));
}

void test_dynclk_now_ns_returns_valid_timestamp(void)
{
    uint64_t timestamp = 0;

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, dynclk_now_ns(&timestamp));
    TEST_ASSERT_GREATER_THAN(1000000000ULL, timestamp); // > 1 s in ns
}

void test_dynclk_now_ns_timestamps_increase(void)
{
    uint64_t t1 = 0, t2 = 0;

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, dynclk_now_ns(&t1));
    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, dynclk_now_ns(&t2));
    TEST_ASSERT_GREATER_OR_EQUAL(t1, t2); // CLOCK_MONOTONIC never goes backwards
}

// === video_dynclk_init ===

void test_dynclk_init_rejects_null(void)
{
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dynclk_init(NULL));
}

void test_dynclk_init_adopts_mapped_platform_region(void)
{
    video_dynclk_t dclk;

    memset(&dclk, 0xA5, sizeof(dclk));
    fake_platform_base = (uintptr_t)fake_regs;

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_dynclk_init(&dclk));
    TEST_ASSERT_EQUAL_UINT((uintptr_t)fake_regs, dclk.base);
    TEST_ASSERT_TRUE(dclk.actual_frequency_mhz == 0.0);
}

void test_dynclk_init_fails_when_platform_unmapped(void)
{
    video_dynclk_t dclk;

    fake_platform_base = (uintptr_t)0;

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dynclk_init(&dclk));
}
