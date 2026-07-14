#!/usr/bin/env bash
set -euo pipefail

# Deploy the latest VM-built Linux userspace app to the board and run it.
# By default this script refreshes the VM build first. Use -x to skip rebuilding
# and deploy/run the latest local artifact.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

USB_AUDIO_TCP_HOST="${USB_AUDIO_TCP_HOST:-192.168.1.20}"
USB_AUDIO_TCP_PORT="${USB_AUDIO_TCP_PORT:-5000}"
USB_AUDIO_PCM_DEVICE="${USB_AUDIO_PCM_DEVICE:-hw:0,0}"

BOARD_HOST="hdmi-overlay"
BOARD_DEPLOY_DIR="${BOARD_DEPLOY_DIR:-/home/root}"
BOARD_SSH_TARGET="${BOARD_HOST}"
BOARD_SSH_OPTS=()

APP_TARGET="${APP_TARGET:-subtitle_overlay_fw}"
LOCAL_ARTIFACT_DIR="${LOCAL_ARTIFACT_DIR:-${REPO_ROOT}/build/vm-artifacts}"

LOCAL_BINARY="${LOCAL_BINARY:-${LOCAL_ARTIFACT_DIR}/${APP_TARGET}}"

SKIP_BUILD=0

usage() {
    cat <<EOF
Usage: ${0##*/} [-x]

Options:
  -x    Skip VM rebuild and deploy/run the latest local artifact.
  -h    Show this help.
EOF
}

step() {
    printf '\n==> %s\n' "$1"
}

shell_quote() {
    local value

    value="${1//\'/\'\\\'\'}"
    printf "'%s'" "${value}"
}

while getopts ":xh" opt; do
    case "${opt}" in
        x)
            SKIP_BUILD=1
            ;;
        h)
            usage
            exit 0
            ;;
        :)
            printf 'Option -%s requires an argument\n' "${OPTARG}" >&2
            usage >&2
            exit 2
            ;;
        \?)
            printf 'Unknown option: -%s\n' "${OPTARG}" >&2
            usage >&2
            exit 2
            ;;
    esac
done

shift $((OPTIND - 1))

if [[ $# -ne 0 ]]; then
    printf 'Unexpected argument: %s\n' "$1" >&2
    usage >&2
    exit 2
fi

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
    step "Refreshing VM build artifact"
    "${SCRIPT_DIR}/build.sh"
else
    step "Using latest local artifact"
fi

if [[ ! -f "${LOCAL_BINARY}" ]]; then
    echo "Missing binary: ${LOCAL_BINARY}" >&2
    echo "Run scripts/build.sh first, or set LOCAL_BINARY=/path/to/${APP_TARGET}." >&2
    exit 2
fi

step "Copying ${LOCAL_BINARY} to ${BOARD_SSH_TARGET}:${BOARD_DEPLOY_DIR}/"
scp -O "${BOARD_SSH_OPTS[@]}" "${LOCAL_BINARY}" "${BOARD_SSH_TARGET}:${BOARD_DEPLOY_DIR}/"

step "Running ${APP_TARGET} on ${BOARD_HOST}"
ssh "${BOARD_SSH_OPTS[@]}" -t "${BOARD_SSH_TARGET}" \
    "cd $(shell_quote "${BOARD_DEPLOY_DIR}") && chmod +x $(shell_quote "${APP_TARGET}") && USB_AUDIO_PCM_DEVICE=$(shell_quote "${USB_AUDIO_PCM_DEVICE}") USB_AUDIO_TCP_HOST=$(shell_quote "${USB_AUDIO_TCP_HOST}") USB_AUDIO_TCP_PORT=$(shell_quote "${USB_AUDIO_TCP_PORT}") ./$(shell_quote "${APP_TARGET}")"
