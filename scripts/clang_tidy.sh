#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"

if ! command -v clang-tidy >/dev/null 2>&1; then
    printf 'clang-tidy is required but was not found in PATH\n' >&2
    exit 127
fi

COMMON_FLAGS=(
    -std=gnu99
    -Wall
    -Wextra
    -DCONFIG_LOG_ENABLED
    -Ilinux/include/uapi
    -Isrc/bsp/bsp_compat
    -Isrc/bsp/platform
    -Isrc/bsp/platform/linux
    -Isrc/bsp/vtc_v7_2/src
    -Isrc/qpc/include
    -Isrc/qpc/ports/posix-qv
    -Isrc/qpc/ports/config
    -Isrc/app
    -Isrc/utils/log
    -Isrc/svc/system
    -Isrc/hal/subtitle_bram
    -Isrc/hal/subtitle_overlay
    -Isrc/hal/video_dma
    -Isrc/hal/video_dynclk
    -Isrc/hal/video_gpio
    -Isrc/hal/video_vtc
    -Isrc/hal/usb_audio
    -Isrc/svc/stt
    -Isrc/svc/subtitle_pipeline
    -Isrc/svc/usb_audio
    -Isrc/svc/video_pipeline
)

mapfile -t SOURCES < <(
    git ls-files \
        'src/app/*.c' \
        'src/hal/**/*.c' \
        'src/svc/**/*.c' \
        'src/utils/log/*.c' \
        ':!:src/bsp/**' \
        ':!:src/utils/template/**' |
    sort
)

if [[ ! -f src/qpc/include/qpc.h ]]; then
    printf 'notice: src/qpc/include/qpc.h is missing; skipping QP/C-dependent active-object sources\n' >&2
    mapfile -t SOURCES < <(
        printf '%s\n' "${SOURCES[@]}" |
        grep -Ev '(^src/app/|/[^/]*AO\.c$)' || true
    )
fi

if [[ ${#SOURCES[@]} -eq 0 ]]; then
    printf 'No source files selected for clang-tidy\n' >&2
    exit 1
fi

printf 'Running clang-tidy on %u source files\n' "${#SOURCES[@]}"

for src in "${SOURCES[@]}"; do
    printf 'clang-tidy %s\n' "${src}"
    clang-tidy "${src}" -- "${COMMON_FLAGS[@]}"
done
