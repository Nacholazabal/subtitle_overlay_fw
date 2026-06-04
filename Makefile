CC ?= gcc
STRIP ?= strip
CEEDLING ?= $(shell if command -v bundle >/dev/null 2>&1; then echo "bundle exec ceedling"; else echo "ceedling"; fi)

VIDEO_PORT_BUILD_DIR := build/video-port-check
APP_BUILD_DIR := build/app
APP_TARGET := subtitle_overlay_fw

COMMON_CFLAGS := \
	-std=gnu99 \
	-Wall \
	-Wextra \
	-DCONFIG_LOG_ENABLED \
	-Ilinux/include/uapi \
	-Isrc/bsp/bsp_compat \
	-Isrc/bsp/platform \
	-Isrc/bsp/platform/linux \
	-Isrc/bsp/vtc_v7_2/src \
	-Isrc/qpc/include \
	-Isrc/qpc/ports/posix-qv \
	-Isrc/qpc/ports/config \
	-Isrc/app \
	-Isrc/utils/log \
	-Isrc/svc/system \
	-Isrc/hal/subtitle_bram \
	-Isrc/hal/subtitle_overlay \
	-Isrc/hal/video_dma \
	-Isrc/hal/video_dynclk \
	-Isrc/hal/video_gpio \
	-Isrc/hal/video_vtc \
	-Isrc/svc/subtitle_pipeline \
	-Isrc/svc/video_pipeline

VIDEO_PORT_CFLAGS := \
	$(COMMON_CFLAGS)

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
	src/hal/subtitle_bram/subtitle_bram.c \
	src/hal/subtitle_overlay/subtitle_overlay.c \
	src/utils/log/log.c \
	src/app/app.c \
	src/svc/system/SystemAO.c \
	src/svc/subtitle_pipeline/SubtitleAO.c \
	src/svc/subtitle_pipeline/subtitle_pipeline.c \
	src/svc/video_pipeline/VideoAO.c \
	src/svc/video_pipeline/video_io.c \
	src/svc/video_pipeline/video_modes.c \
	src/svc/video_pipeline/video_pipeline.c

VIDEO_PORT_OBJS := $(VIDEO_PORT_SRCS:%.c=$(VIDEO_PORT_BUILD_DIR)/%.o)

APP_ARCH_FLAGS ?= \
	-mcpu=cortex-a9 \
	-mfpu=neon \
	-mfloat-abi=hard

APP_CFLAGS := \
	$(COMMON_CFLAGS) \
	-D_POSIX_C_SOURCE=200809L \
	$(APP_ARCH_FLAGS)

APP_LDFLAGS := -pthread -lm

APP_QPC_SRCS := \
	src/qpc/ports/posix-qv/qf_port.c \
	src/qpc/src/qf/qep_hsm.c \
	src/qpc/src/qf/qf_act.c \
	src/qpc/src/qf/qf_actq.c \
	src/qpc/src/qf/qf_dyn.c \
	src/qpc/src/qf/qf_mem.c \
	src/qpc/src/qf/qf_qact.c \
	src/qpc/src/qf/qf_qeq.c \
	src/qpc/src/qf/qf_time.c

APP_SRCS := \
	$(VIDEO_PORT_SRCS) \
	$(APP_QPC_SRCS)

APP_OBJS := $(APP_SRCS:%.c=$(APP_BUILD_DIR)/%.o)

.PHONY: app clean-app test coverage video-port-check clean-video-port-check

video-port-check: $(VIDEO_PORT_BUILD_DIR)/video-port-check.o
	@echo "video-port-check: compiled and linked $(words $(VIDEO_PORT_OBJS)) objects"

app: $(APP_BUILD_DIR)/$(APP_TARGET)
	@echo "app: built $<"

test:
	$(CEEDLING) clean
	$(CEEDLING) test:all

coverage:
	scripts/coverage.sh

$(VIDEO_PORT_BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(VIDEO_PORT_CFLAGS) -c -o $@ $<

$(VIDEO_PORT_BUILD_DIR)/src/bsp/vtc_v7_2/src/%.o: VIDEO_PORT_CFLAGS += \
	-Wno-cast-function-type \
	-Wno-sign-compare \
	-Wno-tautological-compare

$(VIDEO_PORT_BUILD_DIR)/video-port-check.o: $(VIDEO_PORT_OBJS)
	$(CC) -r -o $@ $^

$(APP_BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(APP_BUILD_DIR)/src/bsp/vtc_v7_2/src/%.o: APP_CFLAGS += \
	-Wno-cast-function-type \
	-Wno-sign-compare \
	-Wno-tautological-compare

$(APP_BUILD_DIR)/$(APP_TARGET): $(APP_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -o $@ $^ $(APP_LDFLAGS)
	$(STRIP) $@

clean-video-port-check:
	rm -rf $(VIDEO_PORT_BUILD_DIR)

clean-app:
	rm -rf $(APP_BUILD_DIR)
