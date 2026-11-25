#!/bin/sh
# Build the host-only live protocol probe. The probe links the real firmware
# messenger / CephX / RPC sources against POSIX sockets so the wire behaviour
# can be exercised against a live Ceph v1 monitor without EDK II or a guest.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir="$repo_root/tests/live/.build"
mkdir -p "$build_dir"

dxe="$repo_root/uefi-edk2/CephRbdPkg/CephRbdDxe"

cc -std=c11 -Wall -Wextra \
  -I"$repo_root/tests/host/include" \
  -I"$dxe" \
  "$repo_root/tests/live/live_probe.c" \
  "$repo_root/tests/live/live_tcp4.c" \
  "$repo_root/tests/host/test_crypto.c" \
  "$dxe/CephRbdWire.c" \
  "$dxe/CephRbdMessenger.c" \
  "$dxe/CephRbdCephX.c" \
  "$dxe/CephRbdMsgrV1Handshake.c" \
  "$dxe/CephRbdMsgrV1Transport.c" \
  "$dxe/CephRbdMsgrV1Session.c" \
  "$dxe/CephRbdMsgrV1Recovery.c" \
  "$dxe/CephRbdRpc.c" \
  "$dxe/CephRbdAuth.c" \
  -lcrypto \
  -o "$build_dir/live_probe"

echo "built $build_dir/live_probe"
