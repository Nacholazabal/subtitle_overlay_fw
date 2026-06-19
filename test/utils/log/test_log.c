#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "unity.h"

#define LOG_THREAD_MESSAGES (1000U)

static pthread_mutex_t result_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t callback_count;
static uint32_t mismatched_count;
static __thread char expected_thread_tag;

static void capture_log(log_level_e severity, char const* message)
{
    (void)severity;
    (void)pthread_mutex_lock(&result_mutex);
    callback_count++;
    if ((message == NULL) || (strncmp(message, "thread-", 7U) != 0)
        || (message[7] != expected_thread_tag))
    {
        mismatched_count++;
    }
    (void)pthread_mutex_unlock(&result_mutex);
}

static void second_log(log_level_e severity, char const* message)
{
    (void)severity;
    (void)message;
}

static void* log_thread(void* argument)
{
    uint32_t i;

    expected_thread_tag = *(char const*)argument;
    for (i = 0U; i < LOG_THREAD_MESSAGES; i++)
    {
        log_message(LOG_LEVEL_INFO, "thread-%c-%lu", expected_thread_tag, (unsigned long)i);
    }
    return NULL;
}

void setUp(void)
{
    log_init();
    callback_count = 0U;
    mismatched_count = 0U;
}

void tearDown(void)
{}

void test_log_subscribe_validates_arguments_and_capacity(void)
{
    TEST_ASSERT_EQUAL_INT(LOG_ERROR_INVALID_ARGUMENT, log_subscribe(NULL, LOG_LEVEL_INFO));
    TEST_ASSERT_EQUAL_INT(LOG_ERROR_INVALID_ARGUMENT,
                          log_subscribe(capture_log, (log_level_e)(LOG_LEVEL_ERROR + 1)));
    TEST_ASSERT_EQUAL_INT(LOG_ERROR_NONE, log_subscribe(capture_log, LOG_LEVEL_INFO));
    TEST_ASSERT_EQUAL_INT(LOG_ERROR_SUBSCRIBERS_EXCEEDED,
                          log_subscribe(second_log, LOG_LEVEL_INFO));
    TEST_ASSERT_EQUAL_INT(LOG_ERROR_INVALID_ARGUMENT, log_unsubscribe(NULL));
}

void test_log_threshold_and_unsubscribe_control_delivery(void)
{
    expected_thread_tag = 'A';
    TEST_ASSERT_EQUAL_INT(LOG_ERROR_NONE, log_subscribe(capture_log, LOG_LEVEL_WARNING));

    log_message(LOG_LEVEL_INFO, "thread-A-hidden");
    log_message(LOG_LEVEL_ERROR, "thread-A-visible");
    TEST_ASSERT_EQUAL_UINT32(1U, callback_count);

    TEST_ASSERT_EQUAL_INT(LOG_ERROR_NONE, log_unsubscribe(capture_log));
    TEST_ASSERT_EQUAL_INT(LOG_ERROR_NOT_SUBSCRIBED, log_unsubscribe(capture_log));
    log_message(LOG_LEVEL_ERROR, "thread-A-hidden-again");
    TEST_ASSERT_EQUAL_UINT32(1U, callback_count);
}

void test_log_message_uses_independent_buffers_across_threads(void)
{
    pthread_t first;
    pthread_t second;
    char const first_tag = 'A';
    char const second_tag = 'B';

    TEST_ASSERT_EQUAL_INT(LOG_ERROR_NONE, log_subscribe(capture_log, LOG_LEVEL_INFO));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&first, NULL, log_thread, (void*)&first_tag));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&second, NULL, log_thread, (void*)&second_tag));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(first, NULL));
    TEST_ASSERT_EQUAL_INT(0, pthread_join(second, NULL));

    TEST_ASSERT_EQUAL_UINT32(2U * LOG_THREAD_MESSAGES, callback_count);
    TEST_ASSERT_EQUAL_UINT32(0U, mismatched_count);
}
