// Unit test for the reference-counted hw_platform ownership (SRC-C02).
//
// hw_platform.c talks to /dev/mem via open/mmap/munmap/close, which cannot run
// in CI. This test compiles the real translation unit directly (via #include)
// with those four syscalls redirected to in-memory fakes, so the actual
// refcount/teardown logic is exercised end-to-end. The system headers are
// included BEFORE the redirect macros so their own prototypes/inlines are
// untouched; only the call sites inside hw_platform.c expand to the fakes.

#include "unity.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

// --- fake syscall backend ---------------------------------------------------
static int open_count;
static int mmap_count;
static int munmap_count;
static int close_count;

static int fake_open_ret;   // value fake_open returns
static int fail_map_at;     // 0-based mmap call that returns MAP_FAILED, -1 = never
static int map_call_index;  // running mmap call counter

static int fake_open(const char* path, int flags, ...)
{
    (void)path;
    (void)flags;
    ++open_count;
    return fake_open_ret;
}

static void* fake_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)addr;
    (void)length;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;

    int const idx = map_call_index++;
    ++mmap_count;
    if (idx == fail_map_at)
    {
        return MAP_FAILED;
    }
    // A distinct non-NULL, non-MAP_FAILED address per region (never dereferenced).
    return (void*)(uintptr_t)(0x10000000UL + (uintptr_t)(idx + 1) * 0x1000UL);
}

static int fake_munmap(void* addr, size_t length)
{
    (void)addr;
    (void)length;
    ++munmap_count;
    return 0;
}

static int fake_close(int fd)
{
    (void)fd;
    ++close_count;
    return 0;
}

#define open fake_open
#define mmap fake_mmap
#define munmap fake_munmap
#define close fake_close
#include "hw_platform.c"
#undef open
#undef mmap
#undef munmap
#undef close

#define HW_REGION_TOTAL ((int)HW_REGION_COUNT)

void setUp(void)
{
    // Reset the fakes.
    open_count = 0;
    mmap_count = 0;
    munmap_count = 0;
    close_count = 0;
    fake_open_ret = 3; // a plausible non-negative fd
    fail_map_at = -1;
    map_call_index = 0;

    // Reset hw_platform's internal state (same translation unit via #include).
    refcount = 0U;
    devmem_fd = -1;
    for (hw_platform_region_e r = HW_REGION_DYNCLK; r < HW_REGION_COUNT; ++r)
    {
        mappings[r].virtual_base = NULL;
    }
}

void tearDown(void)
{
}

void test_first_acquire_opens_and_maps_every_region(void)
{
    TEST_ASSERT_EQUAL_INT(0, hw_platform_init());

    TEST_ASSERT_EQUAL_INT(1, open_count);
    TEST_ASSERT_EQUAL_INT(HW_REGION_TOTAL, mmap_count);
    TEST_ASSERT_NOT_EQUAL(0, hw_platform_base(HW_REGION_OVERLAY));
    TEST_ASSERT_NOT_EQUAL(0, hw_platform_base(HW_REGION_SUBTITLE_BRAM));
}

void test_second_acquire_takes_reference_without_remapping(void)
{
    TEST_ASSERT_EQUAL_INT(0, hw_platform_init());
    TEST_ASSERT_EQUAL_INT(0, hw_platform_init());

    // No second open/map: the platform was already mapped.
    TEST_ASSERT_EQUAL_INT(1, open_count);
    TEST_ASSERT_EQUAL_INT(HW_REGION_TOTAL, mmap_count);
}

void test_release_while_still_referenced_does_not_unmap(void)
{
    (void)hw_platform_init(); // refcount 1 (VideoAO)
    (void)hw_platform_init(); // refcount 2 (SubtitleAO)

    hw_platform_cleanup(); // e.g. VideoAO error path releases

    // SubtitleAO still holds a reference: nothing torn down, bases still valid.
    TEST_ASSERT_EQUAL_INT(0, munmap_count);
    TEST_ASSERT_EQUAL_INT(0, close_count);
    TEST_ASSERT_NOT_EQUAL(0, hw_platform_base(HW_REGION_OVERLAY));
    TEST_ASSERT_NOT_EQUAL(0, hw_platform_base(HW_REGION_SUBTITLE_BRAM));
}

void test_last_release_unmaps_all_regions_and_closes(void)
{
    (void)hw_platform_init();
    (void)hw_platform_init();

    hw_platform_cleanup(); // still referenced
    hw_platform_cleanup(); // last owner releases

    TEST_ASSERT_EQUAL_INT(HW_REGION_TOTAL, munmap_count);
    TEST_ASSERT_EQUAL_INT(1, close_count);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)hw_platform_base(HW_REGION_OVERLAY));
}

void test_release_without_reference_is_a_noop(void)
{
    hw_platform_cleanup(); // no outstanding reference

    TEST_ASSERT_EQUAL_INT(0, munmap_count);
    TEST_ASSERT_EQUAL_INT(0, close_count);

    // A stray extra release after a full acquire/release cycle stays safe.
    (void)hw_platform_init();
    hw_platform_cleanup();
    int const munmaps_after_cycle = munmap_count;
    hw_platform_cleanup(); // underflow attempt
    TEST_ASSERT_EQUAL_INT(munmaps_after_cycle, munmap_count);
}

void test_open_failure_reports_error_without_mapping(void)
{
    fake_open_ret = -1;

    TEST_ASSERT_EQUAL_INT(-1, hw_platform_init());
    TEST_ASSERT_EQUAL_INT(0, mmap_count);

    // Refcount stayed at zero, so a later successful acquire still works.
    fake_open_ret = 3;
    TEST_ASSERT_EQUAL_INT(0, hw_platform_init());
    TEST_ASSERT_EQUAL_INT(HW_REGION_TOTAL, mmap_count);
}

void test_partial_map_failure_tears_down_and_leaves_clean_state(void)
{
    fail_map_at = 2; // third region's mmap fails

    TEST_ASSERT_EQUAL_INT(-1, hw_platform_init());

    // The two regions mapped before the failure are unmapped, fd closed.
    TEST_ASSERT_EQUAL_INT(2, munmap_count);
    TEST_ASSERT_EQUAL_INT(1, close_count);
    TEST_ASSERT_EQUAL_UINT(0, (unsigned)hw_platform_base(HW_REGION_DYNCLK));

    // Refcount stayed 0, so the platform can still be acquired cleanly.
    fail_map_at = -1;
    map_call_index = 0;
    mmap_count = 0;
    TEST_ASSERT_EQUAL_INT(0, hw_platform_init());
    TEST_ASSERT_EQUAL_INT(HW_REGION_TOTAL, mmap_count);
}
