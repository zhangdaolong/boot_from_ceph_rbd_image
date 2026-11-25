#!/usr/bin/bash
set -euo pipefail

usage() {
  echo "Usage: $0 [OVMF_CODE_FD] [KVM|TCG] [OVMF_VARS_FD]" >&2
  exit 2
}

ovmf_code=${1:-/usr/share/OVMF/OVMF_CODE_4M.fd}
accel=${2:-tcg}
ovmf_vars=${3:-/usr/share/OVMF/OVMF_VARS_4M.fd}

case "$accel" in
  KVM|kvm) accel=kvm ;;
  TCG|tcg) accel=tcg ;;
  *) usage ;;
esac

[ -r "$ovmf_code" ] || {
  echo "OVMF code image is not readable: $ovmf_code" >&2
  exit 1
}
[ -r "$ovmf_vars" ] || {
  echo "OVMF variable template is not readable: $ovmf_vars" >&2
  exit 1
}

vars_copy=$(mktemp "${TMPDIR:-/tmp}/ceph-rbd-ovmf-vars.XXXXXX")
cleanup() {
  rm -f "$vars_copy"
}
trap cleanup EXIT HUP INT TERM
cp "$ovmf_vars" "$vars_copy"

qemu-system-x86_64 \
  -machine q35,accel="$accel" \
  -cpu max \
  -m 2048 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$ovmf_code" \
  -drive if=pflash,format=raw,unit=1,file="$vars_copy" \
  -display none \
  -monitor none \
  -serial stdio \
  -no-reboot \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0
