#include <errno.h>
#include <stdint.h>

#include "number_parse.h"
#include "unity.h"

void setUp(void)
{}

void tearDown(void)
{}

void test_number_parse_u32_accepts_exact_bounded_decimal_span(void)
{
    uint32_t value = 99U;
    char const text[] = {'6', '5', '5', '3', '5', 'x'};

    TEST_ASSERT_EQUAL_INT(0, number_parse_u32(text, 5U, 1U, 65535U, &value));
    TEST_ASSERT_EQUAL_UINT32(65535U, value);
}

void test_number_parse_u32_rejects_malformed_arguments_without_changing_output(void)
{
    uint32_t value = 77U;

    TEST_ASSERT_EQUAL_INT(-EINVAL, number_parse_u32(NULL, 1U, 0U, 10U, &value));
    TEST_ASSERT_EQUAL_INT(-EINVAL, number_parse_u32("1", 0U, 0U, 10U, &value));
    TEST_ASSERT_EQUAL_INT(-EINVAL, number_parse_u32("1", 1U, 0U, 10U, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, number_parse_u32("1", 1U, 10U, 1U, &value));
    TEST_ASSERT_EQUAL_INT(-EINVAL, number_parse_u32(" 1", 2U, 0U, 10U, &value));
    TEST_ASSERT_EQUAL_INT(-EINVAL, number_parse_u32("+1", 2U, 0U, 10U, &value));
    TEST_ASSERT_EQUAL_INT(-EINVAL, number_parse_u32("1x", 2U, 0U, 10U, &value));
    TEST_ASSERT_EQUAL_UINT32(77U, value);
}

void test_number_parse_u32_rejects_overflow_and_requested_range_failures(void)
{
    uint32_t value = 55U;

    TEST_ASSERT_EQUAL_INT(-ERANGE, number_parse_u32("4294967296", 10U, 0U, UINT32_MAX, &value));
    TEST_ASSERT_EQUAL_INT(-ERANGE, number_parse_u32("0", 1U, 1U, 65535U, &value));
    TEST_ASSERT_EQUAL_INT(-ERANGE, number_parse_u32("11", 2U, 0U, 10U, &value));
    TEST_ASSERT_EQUAL_INT(-ERANGE, number_parse_u32("1", 1U, 0U, 0U, &value));
    TEST_ASSERT_EQUAL_UINT32(55U, value);
}
