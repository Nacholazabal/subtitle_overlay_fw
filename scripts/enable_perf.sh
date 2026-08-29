#!/usr/bin/env bash
set -euo pipefail

# Enable perf tool in PetaLinux rootfs and rebuild

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

VM_HOST="${VM_HOST:-petalinux-vm}"
VM_PETALINUX_PROJECT="${VM_PETALINUX_PROJECT:-/home/tesislinux/tesis/hdmi-overlay}"
REMOTE_SETTINGS="${REMOTE_SETTINGS:-/home/tesislinux/tesis/settings.sh}"

step() {
    printf '\n==> %s\n' "$1" >&2
}

ssh_vm() {
    ssh "${VM_HOST}" "$@"
}

step "Enabling perf in PetaLinux rootfs configuration"

ssh_vm "cat > /tmp/enable_perf.sh <<'OUTER_EOF'
#!/usr/bin/env bash
set -euo pipefail

cd '${VM_PETALINUX_PROJECT}'

# Source PetaLinux environment
set +u
source '${REMOTE_SETTINGS}'
set -u

echo \"==> Checking current perf configuration\"
grep -E \"CONFIG_perf\" project-spec/configs/rootfs_config || echo \"  (not found)\"

echo \"==> Enabling perf in rootfs_config\"
# Enable perf package in rootfs
sed -i 's/# CONFIG_perf is not set/CONFIG_perf=y/' project-spec/configs/rootfs_config

# Also enable perf-python for better Python support
sed -i 's/# CONFIG_perf-python is not set/CONFIG_perf-python=y/' project-spec/configs/rootfs_config

echo \"==> Verifying changes\"
grep -E \"CONFIG_perf=\" project-spec/configs/rootfs_config || echo \"  WARNING: perf not enabled\"

echo \"==> Rebuilding rootfs (this may take 5-10 minutes)\"
petalinux-build

echo \"==> Build complete\"
echo \"Next steps:\"
echo \"  1. Package boot image (when ready to deploy):\"
echo \"     petalinux-package --boot --force --fsbl images/linux/zynq_fsbl.elf \\\\\"
echo \"       --fpga images/linux/system.bit --u-boot\"
echo \"  2. Copy BOOT.BIN and image.ub to SD card\"

OUTER_EOF
chmod +x /tmp/enable_perf.sh
/tmp/enable_perf.sh"

step "perf tool enabled and rootfs rebuilt successfully"
printf '\nTo package the boot image for deployment, run:\n'
printf '  ssh %s "cd %s && petalinux-package --boot --force --fsbl images/linux/zynq_fsbl.elf --fpga images/linux/system.bit --u-boot"\n' \
    "${VM_HOST}" "${VM_PETALINUX_PROJECT}"
