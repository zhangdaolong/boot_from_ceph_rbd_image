#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/ceph-rbd-core.XXXXXX")
test_binary=$build_dir/core-tests
cleanup() {
  rm -f "$test_binary"
  rmdir "$build_dir"
}
trap cleanup EXIT HUP INT TERM

cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/tests/host/include" \
  -I"$repo_root/uefi-edk2/CephRbdPkg/CephRbdDxe" \
  -I"$repo_root/bios/stage2" \
  "$repo_root/tests/host/test_core.c" \
  "$repo_root/uefi-edk2/CephRbdPkg/CephRbdDxe/CephRbdWire.c" \
  "$repo_root/uefi-edk2/CephRbdPkg/CephRbdDxe/CephRbdMessenger.c" \
  "$repo_root/uefi-edk2/CephRbdPkg/CephRbdDxe/CephRbdCephX.c" \
  "$repo_root/uefi-edk2/CephRbdPkg/CephRbdDxe/CephRbdLayout.c" \
  "$repo_root/uefi-edk2/CephRbdPkg/CephRbdDxe/CephRbdFormat2.c" \
  "$repo_root/bios/stage2/ceph_stage2.c" \
  -o "$test_binary"
"$test_binary"
