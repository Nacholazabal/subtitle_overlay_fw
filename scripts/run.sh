#!/usr/bin/env bash
set -euo pipefail

# Deploy the latest VM-built Linux userspace app to the board and run it.
#
# The board now opens its own WebSocket session to the Nemotron server, so no
# PC-side bridge is started here. What the PC still owns is deployment and,
# critically, the clock: the Arty Z7 has no RTC and boots at its rootfs build
# date, which would make every TLS handshake fail with "certificate is not yet
# valid". See -d/--detach if you intend to power the PC off afterwards.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

USB_AUDIO_PCM_DEVICE="${USB_AUDIO_PCM_DEVICE:-hw:0,0}"

# Streaming STT endpoint. No default is compiled into the firmware: an
# unconfigured board must stay down and say so rather than dial somewhere.
SUBTITLE_STT_WS_URL="${SUBTITLE_STT_WS_URL:-wss://passage-capacity-wistful.ngrok-free.dev/stt/stream}"
SUBTITLE_STT_WS_CA_FILE="${SUBTITLE_STT_WS_CA_FILE:-/etc/ssl/certs/ca-certificates.crt}"

# Nemotron operating point selected by the thesis.
SUBTITLE_STT_NEMOTRON_LATENCY_MS="${SUBTITLE_STT_NEMOTRON_LATENCY_MS:-560}"
SUBTITLE_STT_NEMOTRON_STOP_HISTORY_EOU_MS="${SUBTITLE_STT_NEMOTRON_STOP_HISTORY_EOU_MS:-600}"
SUBTITLE_STT_NEMOTRON_RESIDUE_TOKENS_AT_END="${SUBTITLE_STT_NEMOTRON_RESIDUE_TOKENS_AT_END:-2}"
SUBTITLE_STT_NEMOTRON_TARGET_LANG="${SUBTITLE_STT_NEMOTRON_TARGET_LANG:-es-ES}"

BOARD_HOST="hdmi-overlay"
BOARD_DEPLOY_DIR="${BOARD_DEPLOY_DIR:-/home/root}"
BOARD_LOG_DIR="${BOARD_LOG_DIR:-/home/root/logs}"
BOARD_SSH_TARGET="${BOARD_HOST}"
BOARD_SSH_OPTS=()

APP_TARGET="${APP_TARGET:-subtitle_overlay_fw}"
LOCAL_ARTIFACT_DIR="${LOCAL_ARTIFACT_DIR:-${REPO_ROOT}/build/vm-artifacts}"
LOCAL_BINARY="${LOCAL_BINARY:-${LOCAL_ARTIFACT_DIR}/${APP_TARGET}}"

SKIP_BUILD=0
DETACH=0
INSTALL_SERVICE=0
SKIP_CLOCK=0

usage() {
    cat <<EOF
Usage: ${0##*/} [-x] [-d] [-s] [-C]

Options:
  -x    Skip the VM rebuild and deploy the latest local artifact.
  -d    Run detached on the board so it survives the SSH session ending
        (required if you want to power this PC off afterwards).
  -s    Install /etc/init.d/subtitle-overlay so the board starts it at boot.
  -C    Skip setting the board clock (TLS will fail unless NTP already ran).
  -h    Show this help.

Environment:
  SUBTITLE_STT_WS_URL   Streaming STT endpoint (default: the reserved ngrok domain)
  USB_AUDIO_PCM_DEVICE  ALSA capture device (default: hw:0,0)
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

while getopts ":xdsCh" opt; do
    case "${opt}" in
        x) SKIP_BUILD=1 ;;
        d) DETACH=1 ;;
        s) INSTALL_SERVICE=1 ;;
        C) SKIP_CLOCK=1 ;;
        h) usage; exit 0 ;;
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

if [[ "${SKIP_CLOCK}" -eq 0 ]]; then
    step "Setting the board clock (no RTC on this hardware)"
    ssh "${BOARD_SSH_OPTS[@]}" "${BOARD_SSH_TARGET}" "date -s @$(date -u +%s) >/dev/null && date -u"
fi

step "Copying ${LOCAL_BINARY} to ${BOARD_SSH_TARGET}:${BOARD_DEPLOY_DIR}/"
scp -O "${BOARD_SSH_OPTS[@]}" "${LOCAL_BINARY}" "${BOARD_SSH_TARGET}:${BOARD_DEPLOY_DIR}/"

# Environment shared by every launch mode.
ENV_ASSIGNMENTS=(
    "USB_AUDIO_PCM_DEVICE=$(shell_quote "${USB_AUDIO_PCM_DEVICE}")"
    "SUBTITLE_STT_WS_URL=$(shell_quote "${SUBTITLE_STT_WS_URL}")"
    "SUBTITLE_STT_WS_CA_FILE=$(shell_quote "${SUBTITLE_STT_WS_CA_FILE}")"
    "SUBTITLE_STT_NEMOTRON_LATENCY_MS=$(shell_quote "${SUBTITLE_STT_NEMOTRON_LATENCY_MS}")"
    "SUBTITLE_STT_NEMOTRON_STOP_HISTORY_EOU_MS=$(shell_quote "${SUBTITLE_STT_NEMOTRON_STOP_HISTORY_EOU_MS}")"
    "SUBTITLE_STT_NEMOTRON_RESIDUE_TOKENS_AT_END=$(shell_quote "${SUBTITLE_STT_NEMOTRON_RESIDUE_TOKENS_AT_END}")"
    "SUBTITLE_STT_NEMOTRON_TARGET_LANG=$(shell_quote "${SUBTITLE_STT_NEMOTRON_TARGET_LANG}")"
)
ENV_LINE="${ENV_ASSIGNMENTS[*]}"

if [[ "${INSTALL_SERVICE}" -eq 1 ]]; then
    step "Installing the boot service on ${BOARD_HOST}"
    scp -O "${BOARD_SSH_OPTS[@]}" "${SCRIPT_DIR}/board/subtitle-overlay.init" \
        "${BOARD_SSH_TARGET}:/etc/init.d/subtitle-overlay"
    # The endpoint lives in /etc/default so the service can be repointed
    # without touching the init script or rebuilding the image.
    {
        printf '# Written by scripts/run.sh -s\n'
        for assignment in "${ENV_ASSIGNMENTS[@]}"; do
            printf 'export %s\n' "${assignment}"
        done
    } | ssh "${BOARD_SSH_OPTS[@]}" "${BOARD_SSH_TARGET}" "cat > /etc/default/subtitle-overlay"
    ssh "${BOARD_SSH_OPTS[@]}" "${BOARD_SSH_TARGET}" \
        "chmod +x /etc/init.d/subtitle-overlay && ln -sf ../init.d/subtitle-overlay /etc/rc5.d/S95subtitle-overlay && echo 'service installed'"
fi

if [[ "${DETACH}" -eq 1 ]]; then
    step "Starting ${APP_TARGET} detached on ${BOARD_HOST}"
    # setsid + nohup so the process outlives this SSH session; the PC can then
    # be powered off without taking the subtitles down with it.
    ssh "${BOARD_SSH_OPTS[@]}" "${BOARD_SSH_TARGET}" \
        "cd $(shell_quote "${BOARD_DEPLOY_DIR}") \
         && chmod +x $(shell_quote "${APP_TARGET}") \
         && mkdir -p $(shell_quote "${BOARD_LOG_DIR}") \
         && LOGFILE=$(shell_quote "${BOARD_LOG_DIR}")/run-\$(date -u +%Y%m%dT%H%M%SZ).log \
         && ln -sf \"\${LOGFILE}\" $(shell_quote "${BOARD_LOG_DIR}")/run-latest.log \
         && ${ENV_LINE} setsid nohup ./$(shell_quote "${APP_TARGET}") > \"\${LOGFILE}\" 2>&1 & \
         sleep 1; echo started"
    printf '\nFollow the log with:\n  ssh %s '"'"'tail -f %s/run-latest.log'"'"'\n' \
        "${BOARD_HOST}" "${BOARD_LOG_DIR}"
else
    step "Running ${APP_TARGET} on ${BOARD_HOST} (foreground; Ctrl-C stops it)"
    ssh "${BOARD_SSH_OPTS[@]}" -t "${BOARD_SSH_TARGET}" \
        "cd $(shell_quote "${BOARD_DEPLOY_DIR}") && chmod +x $(shell_quote "${APP_TARGET}") && ${ENV_LINE} ./$(shell_quote "${APP_TARGET}")"
fi
