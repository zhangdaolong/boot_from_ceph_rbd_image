# Legacy BIOS second-stage implementation notes

The first BIOS target is a firmware-loaded second-stage executable, not a complete BIOS Option ROM.

## Phase 1

- Use SeaBIOS or an architecture-compatible test ROM.
- Obtain a packet/network base through a PXE/UNDI compatible stack.
- Implement Ceph messenger, CephX and RBD read operations.
- Read the BIOS boot partition or an explicit boot loader image from RBD.

## Phase 2

- Package the second-stage loader as an Option ROM.
- Register or emulate an INT 13h disk service.
- Add GPT/MBR boot-sector or boot-loader support.
- Test with a real PXE/UNDI-compatible network adapter only after the SeaBIOS prototype passes.

`UNDI`/PXE code is used only as a network bootstrap API. The loader still needs a Ceph/RBD protocol implementation and must not depend on guest initramfs mapping.
