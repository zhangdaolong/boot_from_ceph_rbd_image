# Legacy BIOS second-stage implementation notes

The first BIOS target is a firmware-loaded second-stage executable, not a complete BIOS Option ROM.

## Phase 1

- Use SeaBIOS or an architecture-compatible test ROM.
- Obtain a packet/network base through a PXE/UNDI compatible stack.
- Use the shared Ceph wire, CephX and RBD metadata core in `uefi-edk2/CephRbdPkg/CephRbdDxe`.
- Provide a `CEPH_BIOS_READ_AT` adapter that reads bytes from the authenticated RBD image.
- Use `bios/stage2/ceph_stage2.c` to locate and validate the single active MBR partition and load its first sector.
- Transfer control to that sector only after the PXE/UNDI-to-Ceph transport adapter and real-mode transition are implemented.

## Phase 2

- Package the second-stage loader as an Option ROM.
- Register or emulate an INT 13h disk service.
- Add GPT/MBR boot-sector or boot-loader support.
- Test with a real PXE/UNDI-compatible network adapter only after the SeaBIOS prototype passes.

`UNDI`/PXE code is used only as a network bootstrap API. The current C module is a host-testable partition-loader core, not yet a BIOS executable: PXE/UNDI transport, Ceph network sessions, a real-mode entry/trampoline, and control transfer remain unimplemented. It must not depend on guest initramfs mapping.
