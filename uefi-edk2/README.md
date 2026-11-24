## CephRbdBoot EDK II package

This directory is a build scaffold for the UEFI firmware-side Ceph RBD prototype.

The current package intentionally exposes no end-to-end Ceph implementation yet. It defines the package layout and build entrypoint needed to add protocol parsing, block I/O, and OVMF tests in subsequent commits.

- `CephRbdPkg/CephRbdPkg.dec`: package declarations
- `CephRbdPkg/CephRbdPkg.dsc`: package build description
- `CephRbdPkg/CephRbdDxe/CephRbdDxe.inf`: driver module
- `CephRbdPkg/CephRbdDxe/CephRbdDxe.c`: protocol and driver skeleton

Build with `scripts/build-edk2.sh <EDK2_WORKSPACE> <EDK2_PACKAGES_PATH>`.
