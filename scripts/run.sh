#!/usr/bin/env bash
set -euo pipefail

# Build, deploy and run the Linux userspace app as the board boot service.
#
# The board opens its own WebSocket session to the Nemotron server, so no
# PC-side bridge is started here. The production image obtains its clock from
# NTP before the firmware attempts its TLS connection.

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

BOARD_MAC="${BOARD_MAC:-00:0a:35:00:1e:53}"
BOARD_IP="${BOARD_IP:-}"
BOARD_USER="${BOARD_USER:-root}"
BOARD_DEPLOY_DIR="${BOARD_DEPLOY_DIR:-/home/root}"
BOARD_LOG_DIR="${BOARD_LOG_DIR:-/home/root/logs}"
BOARD_SSH_TARGET="${BOARD_SSH_TARGET:-}"
BOARD_SSH_IDENTITY="${BOARD_SSH_IDENTITY:-}"
BOARD_SSH_HOST_KEY_ALIAS="${BOARD_SSH_HOST_KEY_ALIAS:-subtitle-overlay-board}"
BOARD_SSH_OPTS=(
    -o "HostKeyAlgorithms=+ssh-rsa"
    -o "PubkeyAcceptedAlgorithms=+ssh-rsa"
    -o "HostKeyAlias=${BOARD_SSH_HOST_KEY_ALIAS}"
    -o "StrictHostKeyChecking=accept-new"
)

APP_TARGET="${APP_TARGET:-subtitle_overlay_fw}"
LOCAL_ARTIFACT_DIR="${LOCAL_ARTIFACT_DIR:-${REPO_ROOT}/build/vm-artifacts}"
LOCAL_BINARY="${LOCAL_BINARY:-${LOCAL_ARTIFACT_DIR}/${APP_TARGET}}"

SKIP_BUILD=0
PROFILE=0

usage() {
    cat <<EOF
Usage: ${0##*/} [-x] [-p]

Options:
  -x    Skip the VM rebuild and deploy the latest local artifact.
  -p    Build with firmware tracing enabled for Perfetto profiling.
  -h    Show this help.

Environment:
  BOARD_MAC              MAC used for automatic IP discovery
                         (default: 00:0a:35:00:1e:53)
  BOARD_IP               Explicit board IP; skips MAC discovery
  BOARD_SSH_TARGET       Explicit SSH target; overrides BOARD_IP and discovery
  BOARD_SSH_IDENTITY     Optional SSH private-key path
  SUBTITLE_STT_WS_URL    Streaming STT endpoint (default: reserved ngrok domain)
  USB_AUDIO_PCM_DEVICE   ALSA capture device (default: hw:0,0)

The command always installs/updates the subtitle-overlay boot service and
restarts it. Use -x after a separate scripts/build.sh invocation.
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

while getopts ":xph" opt; do
    case "${opt}" in
        x) SKIP_BUILD=1 ;;
        p) PROFILE=1 ;;
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

if [[ "${SKIP_BUILD}" -eq 1 && "${PROFILE}" -eq 1 ]]; then
    echo "Options -x and -p cannot be combined: -x skips the build that -p configures." >&2
    exit 2
fi

if [[ -n "${BOARD_SSH_IDENTITY}" ]]; then
    BOARD_SSH_OPTS+=( -i "${BOARD_SSH_IDENTITY}" )
fi

if [[ -n "${BOARD_SSH_TARGET}" ]]; then
    BOARD_DISPLAY="${BOARD_SSH_TARGET} (explicit target)"
elif [[ -n "${BOARD_IP}" ]]; then
    BOARD_SSH_TARGET="${BOARD_USER}@${BOARD_IP}"
    BOARD_DISPLAY="${BOARD_IP} (explicit IP)"
else
    step "Locating board MAC ${BOARD_MAC} on the LAN"
    BOARD_IP="$(python3 "${SCRIPT_DIR}/board/find_board_ip.py" --mac "${BOARD_MAC}")"
    BOARD_SSH_TARGET="${BOARD_USER}@${BOARD_IP}"
    BOARD_DISPLAY="${BOARD_IP} (discovered from ${BOARD_MAC})"
fi

step "Deployment configuration"
printf '  board: %s\n' "${BOARD_DISPLAY}"
printf '  mode : persistent boot service\n'
if [[ "${SKIP_BUILD}" -eq 1 ]]; then
    BUILD_MODE="existing artifact"
elif [[ "${PROFILE}" -eq 1 ]]; then
    BUILD_MODE="profiling"
else
    BUILD_MODE="production"
fi
printf '  build: %s\n' "${BUILD_MODE}"

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
    step "Refreshing VM build artifact"
    if [[ "${PROFILE}" -eq 1 ]]; then
        "${SCRIPT_DIR}/build.sh" -p
    else
        "${SCRIPT_DIR}/build.sh"
    fi
else
    step "Using latest local artifact"
fi

if [[ ! -f "${LOCAL_BINARY}" ]]; then
    echo "Missing binary: ${LOCAL_BINARY}" >&2
    echo "Run scripts/build.sh first, or set LOCAL_BINARY=/path/to/${APP_TARGET}." >&2
    exit 2
fi

step "Stopping the installed service before replacing the executable"
ssh "${BOARD_SSH_OPTS[@]}" "${BOARD_SSH_TARGET}" \
    "if [ -x /etc/init.d/subtitle-overlay ]; then /etc/init.d/subtitle-overlay stop; fi"

step "Copying ${LOCAL_BINARY} to ${BOARD_SSH_TARGET}:${BOARD_DEPLOY_DIR}/"
scp -O "${BOARD_SSH_OPTS[@]}" "${LOCAL_BINARY}" \
    "${BOARD_SSH_TARGET}:${BOARD_DEPLOY_DIR}/${APP_TARGET}"

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

step "Installing/updating the boot service on ${BOARD_DISPLAY}"
scp -O "${BOARD_SSH_OPTS[@]}" "${SCRIPT_DIR}/board/subtitle-overlay.init" \
    "${BOARD_SSH_TARGET}:/etc/init.d/subtitle-overlay"
# The endpoint lives in /etc/default so the service can be repointed without
# touching the init script or rebuilding the image.
{
    printf '# Written by scripts/run.sh\n'
    for assignment in "${ENV_ASSIGNMENTS[@]}"; do
        printf 'export %s\n' "${assignment}"
    done
} | ssh "${BOARD_SSH_OPTS[@]}" "${BOARD_SSH_TARGET}" "cat > /etc/default/subtitle-overlay"
ssh "${BOARD_SSH_OPTS[@]}" "${BOARD_SSH_TARGET}" \
    "chmod +x $(shell_quote "${BOARD_DEPLOY_DIR}/${APP_TARGET}") /etc/init.d/subtitle-overlay \
     && ln -sf ../init.d/subtitle-overlay /etc/rc5.d/S95subtitle-overlay \
     && /etc/init.d/subtitle-overlay start \
     && /etc/init.d/subtitle-overlay status"

printf '\nService installed and running. Follow the log with:\n'
printf '  ssh %s %s\n' "${BOARD_SSH_TARGET}" "$(shell_quote "tail -f ${BOARD_LOG_DIR}/run-latest.log")"
