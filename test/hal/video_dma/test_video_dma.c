// Tests for video_dma_init frame-count handling (SRC-M05).
//
// video_dma is normally mocked (not in the test :source set). This test
// compiles the real translation unit directly (via #include) with the
// open/ioctl/mmap/munmap/close syscalls redirected to fakes, so the frame-count
// contract is exercised without a real /dev/hdmi-vdma device. System headers
// are included before the redirect macros so their prototypes stay intact.

#include "unity.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "hdmi_vdma.h"

// --- fake syscall backend ---------------------------------------------------
static uint32_t fake_kernel_frame_count;
static uint32_t fake_kernel_frame_size;
static int mmap_count;
static int munmap_count;
static int close_count;

// Failure injection: set to make the corresponding syscall fail.
static int fail_open;
static int fail_mmap;
static unsigned long fail_ioctl_request; // ioctl matching this request returns -1
static int ioctl_fail_all;

// Last observed ioctl, so tests can assert the right request code was issued.
static unsigned long last_ioctl_request;
static struct hdmi_vdma_config last_ioctl_config;
static uint32_t last_ioctl_frame;

// Value the fake reports for channel-status ioctls.
static uint32_t fake_channel_running;

static int fake_open(const char* path, int flags, ...)
{
    (void)path;
    (void)flags;
    return fail_open ? -1 : 5; // fake fd
}

static int fake_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    last_ioctl_request = request;

    if (ioctl_fail_all || ((fail_ioctl_request != 0UL) && (request == fail_ioctl_request)))
    {
        return -1;
    }

    if (request == HDMI_VDMA_GET_INFO)
    {
        va_list ap;
        va_start(ap, request);
        struct hdmi_vdma_info* const info = va_arg(ap, struct hdmi_vdma_info*);
        va_end(ap);
        info->frame_count = fake_kernel_frame_count;
        info->frame_size = fake_kernel_frame_size;
    }
    else if ((request == HDMI_VDMA_MM2S_CONFIGURE) || (request == HDMI_VDMA_S2MM_CONFIGURE))
    {
        va_list ap;
        va_start(ap, request);
        struct hdmi_vdma_config const* const cfg = va_arg(ap, struct hdmi_vdma_config*);
        va_end(ap);
        last_ioctl_config = *cfg;
    }
    else if ((request == HDMI_VDMA_MM2S_SELECT) || (request == HDMI_VDMA_S2MM_SELECT))
    {
        va_list ap;
        va_start(ap, request);
        uint32_t const* const frame = va_arg(ap, uint32_t*);
        va_end(ap);
        last_ioctl_frame = *frame;
    }
    else if ((request == HDMI_VDMA_MM2S_STATUS) || (request == HDMI_VDMA_S2MM_STATUS))
    {
        va_list ap;
        va_start(ap, request);
        struct hdmi_vdma_channel_status* const status = va_arg(ap, struct hdmi_vdma_channel_status*);
        va_end(ap);
        status->running = fake_channel_running;
    }

    return 0; // start/stop ioctls succeed
}

static void* fake_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)addr;
    (void)length;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;
    ++mmap_count;
    if (fail_mmap)
    {
        return MAP_FAILED;
    }
    return (void*)(uintptr_t)(0x20000000UL + (uintptr_t)mmap_count * 0x1000UL);
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
#define ioctl fake_ioctl
#define mmap fake_mmap
#define munmap fake_munmap
#define close fake_close
#include "video_dma.c"
#undef open
#undef ioctl
#undef mmap
#undef munmap
#undef close

static video_dma_t dma;
static uint8_t* frames[VIDEO_DMA_MAX_FRAMES];

void setUp(void)
{
    fake_kernel_frame_count = 3U;
    fake_kernel_frame_size = 4096U;
    mmap_count = 0;
    munmap_count = 0;
    close_count = 0;
    fail_open = 0;
    fail_mmap = 0;
    fail_ioctl_request = 0UL;
    ioctl_fail_all = 0;
    last_ioctl_request = 0UL;
    last_ioctl_frame = 0U;
    fake_channel_running = 0U;
    memset(&last_ioctl_config, 0, sizeof(last_ioctl_config));
    memset(&dma, 0, sizeof(dma));
    memset(frames, 0, sizeof(frames));
}

void tearDown(void)
{
}

// Bring `dma` to a mapped, ready-to-use state for the API tests below.
static void open_dma(uint32_t frame_count)
{
    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_dma_init(&dma, frames, frame_count));
}

void test_init_maps_requested_frames_when_kernel_has_enough(void)
{
    fake_kernel_frame_count = 3U;

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_dma_init(&dma, frames, 3U));
    TEST_ASSERT_EQUAL_UINT32(3U, dma.frame_count);
    TEST_ASSERT_EQUAL_INT(3, mmap_count);
    TEST_ASSERT_NOT_NULL(frames[0]);
    TEST_ASSERT_NOT_NULL(frames[2]);
}

void test_init_fails_when_kernel_exposes_fewer_frames_than_requested(void)
{
    // SRC-M05: the kernel only has 1 buffer but 3 were requested. Init must fail
    // instead of aliasing framebuffers via i % info.frame_count.
    fake_kernel_frame_count = 1U;

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dma_init(&dma, frames, 3U));
}

void test_init_fails_when_kernel_reports_no_buffers(void)
{
    fake_kernel_frame_count = 0U;

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dma_init(&dma, frames, 2U));
}

// === video_dma_init: argument validation and syscall failures ===

void test_init_rejects_bad_arguments(void)
{
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dma_init(NULL, frames, 2U));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dma_init(&dma, NULL, 2U));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dma_init(&dma, frames, 0U));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM,
                          video_dma_init(&dma, frames, VIDEO_DMA_MAX_FRAMES + 1U));
}

void test_init_fails_when_device_cannot_be_opened(void)
{
    fail_open = 1;

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dma_init(&dma, frames, 2U));
    TEST_ASSERT_EQUAL_INT(0, mmap_count);
}

void test_init_fails_when_get_info_ioctl_fails(void)
{
    fail_ioctl_request = HDMI_VDMA_GET_INFO;

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dma_init(&dma, frames, 2U));
    // cleanup ran: the fd was closed
    TEST_ASSERT_EQUAL_INT(1, close_count);
}

void test_init_fails_when_kernel_reports_zero_frame_size(void)
{
    fake_kernel_frame_size = 0U;

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dma_init(&dma, frames, 2U));
}

void test_init_unmaps_earlier_frames_when_mmap_fails(void)
{
    fail_mmap = 1;

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dma_init(&dma, frames, 3U));
    TEST_ASSERT_EQUAL_INT(1, close_count);
}

// === video_dma_cleanup ===

void test_cleanup_tolerates_null(void)
{
    video_dma_cleanup(NULL); // must not crash
}

void test_cleanup_unmaps_all_frames_and_closes_device(void)
{
    open_dma(3U);

    video_dma_cleanup(&dma);

    TEST_ASSERT_EQUAL_INT(3, munmap_count);
    TEST_ASSERT_EQUAL_INT(1, close_count);
    TEST_ASSERT_EQUAL_UINT32(0U, dma.frame_count);
    TEST_ASSERT_EQUAL_INT(-1, dma.fd);
}

// === video_dma_configure ===

void test_configure_rejects_unknown_channel(void)
{
    open_dma(2U);

    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM,
                          video_dma_configure(&dma, (video_dma_channel_e)99, 1920U, 1080U, 7680U, 0U));
}

void test_configure_rejects_bad_arguments(void)
{
    open_dma(2U);

    TEST_ASSERT_EQUAL_INT(
        XST_INVALID_PARAM,
        video_dma_configure(NULL, VIDEO_DMA_CHANNEL_MM2S, 1920U, 1080U, 7680U, 0U));
    TEST_ASSERT_EQUAL_INT(
        XST_INVALID_PARAM,
        video_dma_configure(&dma, VIDEO_DMA_CHANNEL_MM2S, 0U, 1080U, 7680U, 0U));
    TEST_ASSERT_EQUAL_INT(
        XST_INVALID_PARAM,
        video_dma_configure(&dma, VIDEO_DMA_CHANNEL_MM2S, 1920U, 0U, 7680U, 0U));
    TEST_ASSERT_EQUAL_INT(
        XST_INVALID_PARAM,
        video_dma_configure(&dma, VIDEO_DMA_CHANNEL_MM2S, 1920U, 1080U, 0U, 0U));
    // frame_index out of range
    TEST_ASSERT_EQUAL_INT(
        XST_INVALID_PARAM,
        video_dma_configure(&dma, VIDEO_DMA_CHANNEL_MM2S, 1920U, 1080U, 7680U, 2U));
}

void test_configure_rejects_unopened_adapter(void)
{
    video_dma_t closed = {.fd = -1, .is_open = 0, .frame_count = 2U};

    TEST_ASSERT_EQUAL_INT(
        XST_INVALID_PARAM,
        video_dma_configure(&closed, VIDEO_DMA_CHANNEL_MM2S, 1920U, 1080U, 7680U, 0U));
}

void test_configure_mm2s_passes_geometry_to_kernel(void)
{
    open_dma(2U);

    TEST_ASSERT_EQUAL_INT(
        XST_SUCCESS, video_dma_configure(&dma, VIDEO_DMA_CHANNEL_MM2S, 1920U, 1080U, 7680U, 1U));

    TEST_ASSERT_EQUAL_UINT(HDMI_VDMA_MM2S_CONFIGURE, last_ioctl_request);
    TEST_ASSERT_EQUAL_UINT32(1920U, last_ioctl_config.width);
    TEST_ASSERT_EQUAL_UINT32(1080U, last_ioctl_config.height);
    TEST_ASSERT_EQUAL_UINT32(7680U, last_ioctl_config.stride);
    TEST_ASSERT_EQUAL_UINT32(1U, last_ioctl_config.frame_index);
}

void test_configure_s2mm_uses_s2mm_request(void)
{
    open_dma(2U);

    TEST_ASSERT_EQUAL_INT(
        XST_SUCCESS, video_dma_configure(&dma, VIDEO_DMA_CHANNEL_S2MM, 1280U, 720U, 5120U, 0U));

    TEST_ASSERT_EQUAL_UINT(HDMI_VDMA_S2MM_CONFIGURE, last_ioctl_request);
    TEST_ASSERT_EQUAL_UINT32(1280U, last_ioctl_config.width);
}

void test_configure_reports_ioctl_failure(void)
{
    open_dma(2U);
    fail_ioctl_request = HDMI_VDMA_MM2S_CONFIGURE;

    TEST_ASSERT_EQUAL_INT(
        XST_FAILURE, video_dma_configure(&dma, VIDEO_DMA_CHANNEL_MM2S, 1920U, 1080U, 7680U, 0U));
}

// === video_dma_start / video_dma_stop ===

void test_start_issues_channel_specific_ioctl(void)
{
    open_dma(2U);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_dma_start(&dma, VIDEO_DMA_CHANNEL_MM2S));
    TEST_ASSERT_EQUAL_UINT(HDMI_VDMA_MM2S_START, last_ioctl_request);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_dma_start(&dma, VIDEO_DMA_CHANNEL_S2MM));
    TEST_ASSERT_EQUAL_UINT(HDMI_VDMA_S2MM_START, last_ioctl_request);
}

void test_stop_issues_channel_specific_ioctl(void)
{
    open_dma(2U);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_dma_stop(&dma, VIDEO_DMA_CHANNEL_MM2S));
    TEST_ASSERT_EQUAL_UINT(HDMI_VDMA_MM2S_STOP, last_ioctl_request);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_dma_stop(&dma, VIDEO_DMA_CHANNEL_S2MM));
    TEST_ASSERT_EQUAL_UINT(HDMI_VDMA_S2MM_STOP, last_ioctl_request);
}

void test_start_and_stop_reject_unknown_channel(void)
{
    open_dma(2U);

    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dma_start(&dma, (video_dma_channel_e)99));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dma_stop(&dma, (video_dma_channel_e)99));
}

void test_start_and_stop_reject_null_and_unopened(void)
{
    video_dma_t closed = {.fd = -1, .is_open = 0};

    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dma_start(NULL, VIDEO_DMA_CHANNEL_MM2S));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dma_start(&closed, VIDEO_DMA_CHANNEL_MM2S));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dma_stop(NULL, VIDEO_DMA_CHANNEL_S2MM));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM, video_dma_stop(&closed, VIDEO_DMA_CHANNEL_S2MM));
}

void test_start_reports_ioctl_failure(void)
{
    open_dma(2U);
    fail_ioctl_request = HDMI_VDMA_MM2S_START;

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dma_start(&dma, VIDEO_DMA_CHANNEL_MM2S));
}

// === video_dma_select_frame ===

void test_select_frame_passes_index_to_kernel(void)
{
    open_dma(3U);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_dma_select_frame(&dma, VIDEO_DMA_CHANNEL_MM2S, 2U));
    TEST_ASSERT_EQUAL_UINT(HDMI_VDMA_MM2S_SELECT, last_ioctl_request);
    TEST_ASSERT_EQUAL_UINT32(2U, last_ioctl_frame);

    TEST_ASSERT_EQUAL_INT(XST_SUCCESS, video_dma_select_frame(&dma, VIDEO_DMA_CHANNEL_S2MM, 1U));
    TEST_ASSERT_EQUAL_UINT(HDMI_VDMA_S2MM_SELECT, last_ioctl_request);
    TEST_ASSERT_EQUAL_UINT32(1U, last_ioctl_frame);
}

void test_select_frame_rejects_out_of_range_index(void)
{
    open_dma(2U);

    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM,
                          video_dma_select_frame(&dma, VIDEO_DMA_CHANNEL_MM2S, 2U));
}

void test_select_frame_rejects_unknown_channel_and_null(void)
{
    open_dma(2U);

    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM,
                          video_dma_select_frame(&dma, (video_dma_channel_e)99, 0U));
    TEST_ASSERT_EQUAL_INT(XST_INVALID_PARAM,
                          video_dma_select_frame(NULL, VIDEO_DMA_CHANNEL_MM2S, 0U));
}

void test_select_frame_reports_ioctl_failure(void)
{
    open_dma(2U);
    fail_ioctl_request = HDMI_VDMA_S2MM_SELECT;

    TEST_ASSERT_EQUAL_INT(XST_FAILURE, video_dma_select_frame(&dma, VIDEO_DMA_CHANNEL_S2MM, 0U));
}

// === video_dma_status ===

void test_status_reports_running_channel_as_zero(void)
{
    open_dma(2U);
    fake_channel_running = 1U;

    TEST_ASSERT_EQUAL_UINT32(0U, video_dma_status(&dma, VIDEO_DMA_CHANNEL_MM2S));
    TEST_ASSERT_EQUAL_UINT(HDMI_VDMA_MM2S_STATUS, last_ioctl_request);

    TEST_ASSERT_EQUAL_UINT32(0U, video_dma_status(&dma, VIDEO_DMA_CHANNEL_S2MM));
    TEST_ASSERT_EQUAL_UINT(HDMI_VDMA_S2MM_STATUS, last_ioctl_request);
}

void test_status_reports_stopped_channel_as_idle(void)
{
    open_dma(2U);
    fake_channel_running = 0U;

    TEST_ASSERT_EQUAL_UINT32(VIDEO_DMA_STATUS_IDLE,
                             video_dma_status(&dma, VIDEO_DMA_CHANNEL_MM2S));
}

void test_status_reports_halted_on_bad_input(void)
{
    video_dma_t closed = {.fd = -1, .is_open = 0};

    TEST_ASSERT_EQUAL_UINT32(VIDEO_DMA_STATUS_HALTED,
                             video_dma_status(NULL, VIDEO_DMA_CHANNEL_MM2S));
    TEST_ASSERT_EQUAL_UINT32(VIDEO_DMA_STATUS_HALTED,
                             video_dma_status(&closed, VIDEO_DMA_CHANNEL_MM2S));
}

void test_status_reports_halted_on_unknown_channel(void)
{
    open_dma(2U);

    TEST_ASSERT_EQUAL_UINT32(VIDEO_DMA_STATUS_HALTED,
                             video_dma_status(&dma, (video_dma_channel_e)99));
}

void test_status_reports_halted_when_ioctl_fails(void)
{
    open_dma(2U);
    fail_ioctl_request = HDMI_VDMA_MM2S_STATUS;

    TEST_ASSERT_EQUAL_UINT32(VIDEO_DMA_STATUS_HALTED,
                             video_dma_status(&dma, VIDEO_DMA_CHANNEL_MM2S));
}
