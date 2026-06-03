CC ?= gcc

VIDEO_PORT_BUILD_DIR := build/video-port-check

VIDEO_PORT_CFLAGS := \
	-std=gnu99 \
	-Wall \
	-Wextra \
	-Ilinux/include/uapi \
	-Isrc/bsp/bsp_compat \
	-Isrc/bsp/platform \
	-Isrc/bsp/platform/linux \
	-Isrc/bsp/vtc_v7_2/src \
	-Isrc/qpc/include \
	-Isrc/qpc/ports/posix-qv \
	-Isrc/qpc/ports/config \
	-Isrc/app \
	-Isrc/svc/system \
	-Isrc/hal/video_dma \
	-Isrc/hal/video_dynclk \
	-Isrc/hal/video_gpio \
	-Isrc/hal/video_vtc \
	-Isrc/svc/video_pipeline

VIDEO_PORT_SRCS := \
	src/bsp/platform/linux/hw_platform.c \
	src/bsp/vtc_v7_2/src/xvtc.c \
	src/bsp/vtc_v7_2/src/xvtc_g.c \
	src/bsp/vtc_v7_2/src/xvtc_intr.c \
	src/bsp/vtc_v7_2/src/xvtc_selftest.c \
	src/bsp/vtc_v7_2/src/xvtc_sinit.c \
	src/hal/video_dma/video_dma.c \
	src/hal/video_dynclk/video_dynclk.c \
	src/hal/video_gpio/video_gpio.c \
	src/hal/video_vtc/video_vtc.c \
	src/app/app.c \
	src/svc/system/SystemAO.c \
	src/svc/video_pipeline/VideoAO.c \
	src/svc/video_pipeline/video_input.c \
	src/svc/video_pipeline/video_modes.c \
	src/svc/video_pipeline/video_output.c \
	src/svc/video_pipeline/video_pipeline.c

VIDEO_PORT_OBJS := $(VIDEO_PORT_SRCS:%.c=$(VIDEO_PORT_BUILD_DIR)/%.o)

.PHONY: video-port-check clean-video-port-check

video-port-check: $(VIDEO_PORT_BUILD_DIR)/video-port-check.o
	@echo "video-port-check: compiled and linked $(words $(VIDEO_PORT_OBJS)) objects"

$(VIDEO_PORT_BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(VIDEO_PORT_CFLAGS) -c -o $@ $<

$(VIDEO_PORT_BUILD_DIR)/src/bsp/vtc_v7_2/src/%.o: VIDEO_PORT_CFLAGS += \
	-Wno-cast-function-type \
	-Wno-sign-compare \
	-Wno-tautological-compare

$(VIDEO_PORT_BUILD_DIR)/video-port-check.o: $(VIDEO_PORT_OBJS)
	$(CC) -r -o $@ $^

clean-video-port-check:
	rm -rf $(VIDEO_PORT_BUILD_DIR)
