## CephRbdBoot EDK II package

This directory contains the EDK II package and portable protocol helpers for the UEFI firmware-side Ceph RBD prototype.

The package is not yet an end-to-end Ceph implementation and does not publish an EFI Block I/O device. It currently provides:

- `CephRbdPkg/CephRbdPkg.dec`: package declarations
- `CephRbdPkg/CephRbdPkg.dsc`: package build description
- `CephRbdPkg/CephRbdDxe/CephRbdDxe.inf`: driver module
- `CephRbdPkg/CephRbdDxe/CephRbdMessenger.c`: Ceph messenger v1 connect codec/reply classifier, bounded message-frame decoder, and CRC32C checks
- `CephRbdPkg/CephRbdDxe/CephRbdCephX.c`: bounded CephX challenge, request, ticket-request, authenticate, and authorizer payload codecs
- `CephRbdPkg/CephRbdDxe/CephRbdFormat2.c`: normalized Format 2 metadata validation and object-name construction
- `CephRbdPkg/CephRbdDxe/CephRbdDxe.c`: current DXE entrypoint placeholder

These helpers do not implement messenger banner/TCP connection handling, CephX cryptography or ticket verification, Monitor/OSD RPCs, Format 2 metadata RPCs, UEFI networking, or Block I/O publication. Run host tests with `sh tests/run-core-tests.sh`; build with `scripts/build-edk2.sh <EDK2_WORKSPACE> <EDK2_PACKAGES_PATH>`.
