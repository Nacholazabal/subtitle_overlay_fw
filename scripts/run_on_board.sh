#!/usr/bin/env bash
set -euo pipefail

# Copy the VM-built userspace app to the PetaLinux board and run it.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BOARD_HOST="${BOARD_HOST:-hdmi-board}"
BOARD_DEPLOY_DIR="${BOARD_DEPLOY_DIR:-/home/root}"
APP_TARGET="${APP_TARGET:-subtitle_overlay_fw}"
LOCAL_BINARY="${LOCAL_BINARY:-${REPO_ROOT}/build/vm-artifacts/${APP_TARGET}}"

SSH_COMPAT_OPTS=(
    -o HostKeyAlgorithms=+ssh-rsa
    -o PubkeyAcceptedAlgorithms=+ssh-rsa
)

step() {
    printf '\n==> %s\n' "$1"
}

if [ ! -f "${LOCAL_BINARY}" ]; then
    echo "Missing binary: ${LOCAL_BINARY}" >&2
    echo "Run scripts/build_on_vm.sh first, or set LOCAL_BINARY=/path/to/${APP_TARGET}." >&2
    exit 2
fi

step "Copying ${LOCAL_BINARY} to ${BOARD_HOST}:${BOARD_DEPLOY_DIR}/"
scp -O "${SSH_COMPAT_OPTS[@]}" "${LOCAL_BINARY}" "${BOARD_HOST}:${BOARD_DEPLOY_DIR}/"

step "Running ${APP_TARGET} on ${BOARD_HOST}"
ssh -t "${SSH_COMPAT_OPTS[@]}" "${BOARD_HOST}" \
    "cd '${BOARD_DEPLOY_DIR}' && chmod +x '${APP_TARGET}' && ./'${APP_TARGET}'"
