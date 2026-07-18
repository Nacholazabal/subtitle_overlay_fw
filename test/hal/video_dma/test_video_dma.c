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

static int fake_open(const char* path, int flags, ...)
{
    (void)path;
    (void)flags;
    return 5; // fake fd
}

static int fake_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    if (request == HDMI_VDMA_GET_INFO)
    {
        va_list ap;
        va_start(ap, request);
        struct hdmi_vdma_info* const info = va_arg(ap, struct hdmi_vdma_info*);
        va_end(ap);
        info->frame_count = fake_kernel_frame_count;
        info->frame_size = fake_kernel_frame_size;
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
}

void tearDown(void)
{
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
