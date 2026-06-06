#!/usr/bin/env bash
set -euo pipefail

# Build the Linux userspace app inside the PetaLinux VM and copy the binary
# back into this repo's build folder.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

VM_HOST="${VM_HOST:-petalinux-vm}"
VM_PROJECT_ROOT="${VM_PROJECT_ROOT:-/home/tesislinux/tesis}"
REMOTE_PROJECT_NAME="${REMOTE_PROJECT_NAME:-subtitle_overlay_fw}"
REMOTE_SETTINGS="${REMOTE_SETTINGS:-/home/tesislinux/tesis/settings.sh}"
REMOTE_CC="${REMOTE_CC:-arm-linux-gnueabihf-gcc}"
REMOTE_STRIP="${REMOTE_STRIP:-arm-linux-gnueabihf-strip}"
USB_AUDIO_ENABLE_ALSA="${USB_AUDIO_ENABLE_ALSA:-0}"

APP_TARGET="${APP_TARGET:-subtitle_overlay_fw}"
LOCAL_ARTIFACT_DIR="${LOCAL_ARTIFACT_DIR:-${REPO_ROOT}/build/vm-artifacts}"

REMOTE_PROJECT_DIR="${VM_PROJECT_ROOT}/${REMOTE_PROJECT_NAME}"
REMOTE_BINARY="${REMOTE_PROJECT_DIR}/build/app/${APP_TARGET}"
LOCAL_BINARY="${LOCAL_BINARY:-${LOCAL_ARTIFACT_DIR}/${APP_TARGET}}"

step() {
    printf '\n==> %s\n' "$1"
}

ssh_vm() {
    ssh "${VM_HOST}" "$@"
}

step "Preparing local artifact folder"
mkdir -p "${LOCAL_ARTIFACT_DIR}"

step "Syncing repository to ${VM_HOST}:${REMOTE_PROJECT_DIR}"
ssh_vm "rm -rf '${REMOTE_PROJECT_DIR}' && mkdir -p '${REMOTE_PROJECT_DIR}'"
tar \
    --exclude='./build' \
    --exclude='./.git' \
    --exclude='./.llm_context' \
    --exclude='./.agents' \
    --exclude='./.codex' \
    -C "${REPO_ROOT}" \
    -cf - . | ssh_vm "tar -xf - -C '${REMOTE_PROJECT_DIR}'"

step "Building ${APP_TARGET} inside the VM"
ssh_vm "cat > /tmp/subtitle_overlay_fw_build.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
cd '${REMOTE_PROJECT_DIR}'
if [ ! -f '${REMOTE_SETTINGS}' ]; then
    echo 'Missing PetaLinux SDK environment: ${REMOTE_SETTINGS}' >&2
    exit 2
fi
set +u
source '${REMOTE_SETTINGS}'
set -u
echo \"PATH=\${PATH}\"
command -v '${REMOTE_CC}' || true
make clean-app
make app CC='${REMOTE_CC}' STRIP='${REMOTE_STRIP}' USB_AUDIO_ENABLE_ALSA='${USB_AUDIO_ENABLE_ALSA}'
if command -v readelf >/dev/null 2>&1; then
    readelf -V '${REMOTE_BINARY}' | grep GLIBC || true
fi
EOF
chmod +x /tmp/subtitle_overlay_fw_build.sh
/tmp/subtitle_overlay_fw_build.sh"

step "Copying built binary back to ${LOCAL_BINARY}"
scp "${VM_HOST}:${REMOTE_BINARY}" "${LOCAL_BINARY}"
chmod +x "${LOCAL_BINARY}"

printf '\nBuilt VM artifact: %s\n' "${LOCAL_BINARY}"
