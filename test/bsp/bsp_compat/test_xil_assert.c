// Tests for the userspace Xilinx assert compatibility macros (SRC-M09).
//
// These macros previously evaluated their expression but never acted on a
// failure, so the generated Xilinx driver would keep running past a failed
// precondition. They now match the genuine non-fatal Xilinx behavior: return
// early from the enclosing function when the precondition is false.

#include "unity.h"

#include "xil_assert.h"

static int void_fn_body_ran;

static void guarded_void_fn(int ok)
{
    Xil_AssertVoid(ok != 0);
    void_fn_body_ran = 1; // only reached when the precondition holds
}

static int guarded_nonvoid_fn(int ok)
{
    Xil_AssertNonvoid(ok != 0);
    return 42; // only reached when the precondition holds
}

void setUp(void)
{
    void_fn_body_ran = 0;
}

void tearDown(void)
{
}

void test_assert_void_returns_early_on_failed_precondition(void)
{
    guarded_void_fn(0);
    TEST_ASSERT_EQUAL_INT(0, void_fn_body_ran);

    guarded_void_fn(1);
    TEST_ASSERT_EQUAL_INT(1, void_fn_body_ran);
}

void test_assert_nonvoid_returns_zero_on_failed_precondition(void)
{
    TEST_ASSERT_EQUAL_INT(0, guarded_nonvoid_fn(0));
    TEST_ASSERT_EQUAL_INT(42, guarded_nonvoid_fn(1));
}
