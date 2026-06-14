/**********************************************************************************************************************
Copyright (c) 2026 Ignacio Olazabal https://www.linkedin.com/in/ignacio-olazabal/

Some fancy copyright message here (if needed)
**********************************************************************************************************************/

///
/// @file video_dynclk.c
/// @brief Dynamic pixel clock HAL adapter implementation
///

// === Headers files inclusions ==================================================================================== //

#include "video_dynclk.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include "hw_platform.h"
#include "xil_io.h"

// === Macros definitions ========================================================================================== //

#define CLK_BIT_WEDGE   13U
#define CLK_BIT_NOCOUNT 12U

#define ERR_CLKDIVIDER   ((1U << CLK_BIT_WEDGE) | (1U << CLK_BIT_NOCOUNT))
#define ERR_CLKCOUNTCALC 0xFFFFFFFFU

#define OFST_DYNCLK_CTRL        0x00U
#define OFST_DYNCLK_STATUS      0x04U
#define OFST_DYNCLK_CLK_L       0x08U
#define OFST_DYNCLK_FB_L        0x0CU
#define OFST_DYNCLK_FB_H_CLK_H  0x10U
#define OFST_DYNCLK_DIV         0x14U
#define OFST_DYNCLK_LOCK_L      0x18U
#define OFST_DYNCLK_FLTR_LOCK_H 0x1CU

#define BIT_DYNCLK_START   0U
#define BIT_DYNCLK_RUNNING 0U

#define CLK_TIMEOUT_NS 10000000ULL

// === Private data type declarations ============================================================================== //

typedef struct
{
    uint32_t clk0_l;
    uint32_t clk_fb_l;
    uint32_t clk_fbh_clk0_h;
    uint32_t divclk;
    uint32_t lock_l;
    uint32_t fltr_lock_h;
} clk_config_t;

typedef struct
{
    double freq;
    uint32_t fbmult;
    uint32_t clkdiv;
    uint32_t maindiv;
} clk_mode_t;

// === Private variable declarations =============================================================================== //

static const uint64_t lock_lookup[64] = {
    0b0011000110111110100011111010010000000001ULL, 0b0011000110111110100011111010010000000001ULL,
    0b0100001000111110100011111010010000000001ULL, 0b0101101011111110100011111010010000000001ULL,
    0b0111001110111110100011111010010000000001ULL, 0b1000110001111110100011111010010000000001ULL,
    0b1001110011111110100011111010010000000001ULL, 0b1011010110111110100011111010010000000001ULL,
    0b1100111001111110100011111010010000000001ULL, 0b1110011100111110100011111010010000000001ULL,
    0b1111111111111000010011111010010000000001ULL, 0b1111111111110011100111111010010000000001ULL,
    0b1111111111101110111011111010010000000001ULL, 0b1111111111101011110011111010010000000001ULL,
    0b1111111111101000101011111010010000000001ULL, 0b1111111111100111000111111010010000000001ULL,
    0b1111111111100011111111111010010000000001ULL, 0b1111111111100010011011111010010000000001ULL,
    0b1111111111100000110111111010010000000001ULL, 0b1111111111011111010011111010010000000001ULL,
    0b1111111111011101101111111010010000000001ULL, 0b1111111111011100001011111010010000000001ULL,
    0b1111111111011010100111111010010000000001ULL, 0b1111111111011001000011111010010000000001ULL,
    0b1111111111011001000011111010010000000001ULL, 0b1111111111010111011111111010010000000001ULL,
    0b1111111111010101111011111010010000000001ULL, 0b1111111111010101111011111010010000000001ULL,
    0b1111111111010100010111111010010000000001ULL, 0b1111111111010100010111111010010000000001ULL,
    0b1111111111010010110011111010010000000001ULL, 0b1111111111010010110011111010010000000001ULL,
    0b1111111111010010110011111010010000000001ULL, 0b1111111111010001001111111010010000000001ULL,
    0b1111111111010001001111111010010000000001ULL, 0b1111111111010001001111111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
    0b1111111111001111101011111010010000000001ULL, 0b1111111111001111101011111010010000000001ULL,
};

static const uint32_t filter_lookup_low[64] = {
    0b0001011111U, 0b0001010111U, 0b0001111011U, 0b0001011011U, 0b0001101011U, 0b0001110011U,
    0b0001110011U, 0b0001110011U, 0b0001110011U, 0b0001001011U, 0b0001001011U, 0b0001001011U,
    0b0010110011U, 0b0001010011U, 0b0001010011U, 0b0001010011U, 0b0001010011U, 0b0001010011U,
    0b0001010011U, 0b0001010011U, 0b0001010011U, 0b0001010011U, 0b0001010011U, 0b0001100011U,
    0b0001100011U, 0b0001100011U, 0b0001100011U, 0b0001100011U, 0b0001100011U, 0b0001100011U,
    0b0001100011U, 0b0001100011U, 0b0001100011U, 0b0001100011U, 0b0001100011U, 0b0001100011U,
    0b0001100011U, 0b0010010011U, 0b0010010011U, 0b0010010011U, 0b0010010011U, 0b0010010011U,
    0b0010010011U, 0b0010010011U, 0b0010010011U, 0b0010010011U, 0b0010010011U, 0b0010100011U,
    0b0010100011U, 0b0010100011U, 0b0010100011U, 0b0010100011U, 0b0010100011U, 0b0010100011U,
    0b0010100011U, 0b0010100011U, 0b0010100011U, 0b0010100011U, 0b0010100011U, 0b0010100011U,
    0b0010100011U, 0b0010100011U, 0b0010100011U, 0b0010100011U,
};

// === Private function declarations =============================================================================== //

static uint32_t clk_divider(uint32_t divide);
static uint32_t clk_count_calc(uint32_t divide);
static int clk_find_reg(clk_config_t* reg_values, clk_mode_t const* clk_params);
static double clk_find_params(double frequency_mhz, clk_mode_t* best_pick);
static void clk_write_reg(uintptr_t base, clk_config_t const* reg_values);
static uint64_t dynclk_now_ns(void);
static int clk_start(uintptr_t base);

// === Public variable definitions ================================================================================= //
// === Private variable definitions ================================================================================ //
// === Private function implementation ============================================================================= //

/**
 * @brief Compute the MMCM divider register bitfield for an integer divider.
 * @param divide Divider value to encode.
 * @return Encoded divider value, or ERR_CLKDIVIDER when the divider is invalid.
 */
static uint32_t clk_divider(uint32_t divide)
{
    uint32_t output = 0U;
    uint32_t high_time;
    uint32_t low_time;

    if ((divide < 1U) || (divide > 128U))
    {
        return ERR_CLKDIVIDER;
    }

    if (divide == 1U)
    {
        return 0x1041U;
    }

    high_time = divide / 2U;
    if ((divide & 1U) != 0U)
    {
        low_time = high_time + 1U;
        output = 1U << CLK_BIT_WEDGE;
    }
    else
    {
        low_time = high_time;
    }

    output |= 0x03FU & low_time;
    output |= 0xFC0U & (high_time << 6U);

    return output;
}

/**
 * @brief Compute a dynclk counter register value from an integer divider.
 * @param divide Divider value to encode.
 * @return Encoded counter value, or ERR_CLKCOUNTCALC when the divider is invalid.
 */
static uint32_t clk_count_calc(uint32_t divide)
{
    uint32_t const div_calc = clk_divider(divide);

    if (div_calc == ERR_CLKDIVIDER)
    {
        return ERR_CLKCOUNTCALC;
    }

    return (0xFFFU & div_calc) | ((div_calc << 10U) & 0x00C00000U);
}

/**
 * @brief Convert selected clock parameters into dynclk register values.
 * @param reg_values Output register-value bundle.
 * @param clk_params Selected divider and feedback parameters.
 * @return Nonzero on success, zero if the parameters cannot be encoded.
 */
static int clk_find_reg(clk_config_t* const reg_values, clk_mode_t const* const clk_params)
{
    if ((reg_values == NULL) || (clk_params == NULL) || (clk_params->fbmult < 2U)
        || (clk_params->fbmult > 64U))
    {
        return 0;
    }

    reg_values->clk0_l = clk_count_calc(clk_params->clkdiv);
    reg_values->clk_fb_l = clk_count_calc(clk_params->fbmult);
    reg_values->clk_fbh_clk0_h = 0U;
    reg_values->divclk = clk_divider(clk_params->maindiv);

    if ((reg_values->clk0_l == ERR_CLKCOUNTCALC) || (reg_values->clk_fb_l == ERR_CLKCOUNTCALC)
        || (reg_values->divclk == ERR_CLKDIVIDER))
    {
        return 0;
    }

    reg_values->lock_l = (uint32_t)(lock_lookup[clk_params->fbmult - 1U] & 0xFFFFFFFFULL);
    reg_values->fltr_lock_h =
        (uint32_t)((lock_lookup[clk_params->fbmult - 1U] >> 32U) & 0x000000FFULL);
    reg_values->fltr_lock_h |= (filter_lookup_low[clk_params->fbmult - 1U] << 16U) & 0x03FF0000U;

    return 1;
}

/**
 * @brief Find integer MMCM parameters closest to a requested pixel clock.
 * @param frequency_mhz Desired pixel clock in MHz.
 * @param best_pick Output selected clock parameters.
 * @return Absolute frequency error in MHz.
 */
static double clk_find_params(double frequency_mhz, clk_mode_t* const best_pick)
{
    double best_error = 2000.0;
    uint32_t cur_div;

    frequency_mhz *= 5.0;
    best_pick->freq = 0.0;
    best_pick->fbmult = 0U;
    best_pick->clkdiv = 0U;
    best_pick->maindiv = 0U;

    for (cur_div = 1U; cur_div <= 10U; cur_div++)
    {
        uint32_t cur_fb;
        uint32_t const min_fb = cur_div * 6U;
        uint32_t max_fb = cur_div * 12U;
        double const cur_clk_mult = (100.0 / (double)cur_div) / frequency_mhz;

        if (max_fb > 64U)
        {
            max_fb = 64U;
        }

        for (cur_fb = min_fb; cur_fb <= max_fb; cur_fb++)
        {
            uint32_t const cur_clk_div = (uint32_t)((cur_clk_mult * (double)cur_fb) + 0.5);
            double const cur_freq =
                ((100.0 / (double)cur_div) / (double)cur_clk_div) * (double)cur_fb;
            double const cur_error = fabs(cur_freq - frequency_mhz);

            if (cur_error < best_error)
            {
                best_error = cur_error;
                best_pick->clkdiv = cur_clk_div;
                best_pick->fbmult = cur_fb;
                best_pick->maindiv = cur_div;
                best_pick->freq = cur_freq;
            }
        }
    }

    best_pick->freq /= 5.0;
    return best_error / 5.0;
}

/**
 * @brief Write dynclk configuration registers.
 * @param base Mapped dynclk register base.
 * @param reg_values Register-value bundle to write.
 * @return None.
 */
static void clk_write_reg(uintptr_t base, clk_config_t const* const reg_values)
{
    Xil_Out32(base + OFST_DYNCLK_CLK_L, reg_values->clk0_l);
    Xil_Out32(base + OFST_DYNCLK_FB_L, reg_values->clk_fb_l);
    Xil_Out32(base + OFST_DYNCLK_FB_H_CLK_H, reg_values->clk_fbh_clk0_h);
    Xil_Out32(base + OFST_DYNCLK_DIV, reg_values->divclk);
    Xil_Out32(base + OFST_DYNCLK_LOCK_L, reg_values->lock_l);
    Xil_Out32(base + OFST_DYNCLK_FLTR_LOCK_H, reg_values->fltr_lock_h);
}

static uint64_t dynclk_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/**
 * @brief Start the dynclk core and wait for lock.
 * @param base Mapped dynclk register base.
 * @return XST_SUCCESS when locked, or XST_FAILURE on timeout.
 */
static int clk_start(uintptr_t base)
{
    uint64_t const deadline_ns = dynclk_now_ns() + CLK_TIMEOUT_NS;

    Xil_Out32(base + OFST_DYNCLK_CTRL, 1U << BIT_DYNCLK_START);
    while ((Xil_In32(base + OFST_DYNCLK_STATUS) & (1U << BIT_DYNCLK_RUNNING)) == 0U)
    {
        if (dynclk_now_ns() >= deadline_ns)
        {
            fprintf(stderr, "[video_dynclk] PLL lock timeout\n");
            return XST_FAILURE;
        }
    }

    return XST_SUCCESS;
}

// === Public function implementation ============================================================================== //

/**
 * @brief Initialize a dynclk adapter from the mapped platform region.
 * @param dynclk Adapter to initialize.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or XST_FAILURE when the region is not mapped.
 */
int video_dynclk_init(video_dynclk_t* const dynclk)
{
    if (dynclk == NULL)
    {
        return XST_INVALID_PARAM;
    }

    dynclk->base = hw_platform_base(HW_REGION_DYNCLK);
    dynclk->actual_frequency_mhz = 0.0;

    return (dynclk->base != (uintptr_t)0) ? XST_SUCCESS : XST_FAILURE;
}

/**
 * @brief Stop the dynamic pixel clock.
 * @param dynclk Initialized dynclk adapter.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or XST_FAILURE on timeout.
 */
int video_dynclk_stop(video_dynclk_t* const dynclk)
{
    uint64_t deadline_ns;

    if ((dynclk == NULL) || (dynclk->base == (uintptr_t)0))
    {
        return XST_INVALID_PARAM;
    }

    Xil_Out32(dynclk->base + OFST_DYNCLK_CTRL, 0U);
    deadline_ns = dynclk_now_ns() + CLK_TIMEOUT_NS;
    while ((Xil_In32(dynclk->base + OFST_DYNCLK_STATUS) & (1U << BIT_DYNCLK_RUNNING)) != 0U)
    {
        if (dynclk_now_ns() >= deadline_ns)
        {
            fprintf(stderr, "[video_dynclk] clock stop timeout\n");
            return XST_FAILURE;
        }
    }

    dynclk->actual_frequency_mhz = 0.0;
    return XST_SUCCESS;
}

/**
 * @brief Configure and start the dynamic pixel clock.
 * @param dynclk Initialized dynclk adapter.
 * @param frequency_mhz Desired pixel clock in MHz.
 * @return XST_SUCCESS on success, XST_INVALID_PARAM for bad input, or XST_FAILURE when configuration/lock fails.
 */
int video_dynclk_configure(video_dynclk_t* const dynclk, double frequency_mhz)
{
    clk_mode_t mode;
    clk_config_t regs;

    if ((dynclk == NULL) || (dynclk->base == (uintptr_t)0) || (frequency_mhz <= 0.0))
    {
        return XST_INVALID_PARAM;
    }

    (void)clk_find_params(frequency_mhz, &mode);
    if (!clk_find_reg(&regs, &mode))
    {
        return XST_FAILURE;
    }

    (void)video_dynclk_stop(dynclk);
    clk_write_reg(dynclk->base, &regs);

    if (clk_start(dynclk->base) != XST_SUCCESS)
    {
        return XST_FAILURE;
    }

    dynclk->actual_frequency_mhz = mode.freq;
    return XST_SUCCESS;
}

// === End of documentation ======================================================================================== //
