# Ceph/RBD firmware implementation status

## Phase 1: UEFI package skeleton

The EDK II package currently provides build structure and an entrypoint. The next implementation milestones are:

1. Driver binding and network-stack discovery.
2. Ceph messenger framing and CephX authentication.
3. Monitor, OSD and RBD metadata parsing.
4. Read-only `EFI_BLOCK_IO2_PROTOCOL` publication.
5. GPT/ESP boot verification under OVMF.

## Explicit non-goals of the skeleton

- It does not contain a plaintext Ceph key.
- It does not invent a fake Block I/O device.
- It does not expose firmware bootable media until Ceph and RBD metadata succeed.
- It does not implement writes or live resizing.
