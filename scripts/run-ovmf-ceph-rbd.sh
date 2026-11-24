#!/usr/bin/bash
set -euo pipefail

usage() {
  echo "Usage: $0 [UEFI_BIOS_FD] [KVM|TCG]" >&2
  exit 2
}

bios_fd=${1:-/usr/share/OVMF/OVMF_CODE.fd}
accel=${2:-tcg}

exec qemu-system-x86_64 \
  -machine q35,accel="$accel" \
  -cpu max \
  -m 2048 \
  -bios "$bios_fd" \
  -serial stdio \
  -nographic \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0
