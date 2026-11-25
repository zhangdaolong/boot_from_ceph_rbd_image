# Direct Ceph RBD Firmware Boot

This repository is developing firmware-side paths that make a Ceph RBD image visible directly during UEFI or Legacy BIOS startup.

> Status: the repository now contains independently testable Ceph messenger v1 connect/frame codecs, CephX payload codecs and AES-128 challenge-response, RBD Format 2 layout/object-name helpers, and a BIOS MBR partition-loader core. A libvirt/QEMU example can boot an existing UEFI disk image stored on Ceph RBD using QEMU's host-side RBD backend. This is separate from the still-incomplete native firmware Ceph client: banner/address exchange, TCP, CephX ticket decryption/validation and authorizer verification, Monitor/OSD sessions, RBD metadata RPCs, UEFI Block I/O publication, and BIOS PXE/control transfer remain incomplete.

## Architecture

```text
UEFI firmware / BIOS second-stage loader
  ├── NIC and IP stack
  ├── CephX authentication
  ├── Ceph monitor/OSD messenger
  ├── RBD metadata and object mapping
  └── Block device / boot loader interface
             │
             ▼
        Ceph RBD image
             │
             ▼
        OS root and boot chain
```

UEFI firmware provides network and device abstractions, making EDK II the primary implementation target. Legacy BIOS has no standard equivalent, so its implementation uses a network Option ROM or a BIOS-loaded second-stage executable and an explicit block-device interface.

## Supported MVP scope

The initial implementation intentionally restricts the protocol surface:

- x86-64 UEFI/OVMF
- Ceph messengers and CephX
- RBD format 2 metadata
- fixed pool/image and monitor configuration
- read-only block access for the first firmware release
- GPT/ESP discovery for UEFI
- Legacy BIOS option-ROM or second-stage loader as a separate implementation

The MVP does not initially implement arbitrary CRUSH discovery, every RBD feature flag, snapshots, writes, `FS-LOCK`, or hot network reconfiguration.

## Repository layout

```text
uefi-edk2/CephRbdPkg/     EDK II DXE package and protocol core
bios/                     Legacy BIOS Option ROM/second-stage design and code
config/                   Example Ceph and boot configuration
scripts/                  Build and QEMU test helpers
tests/                    Host-side protocol/layout/loader tests and vectors
```

## UEFI implementation sequence

1. Add an EDK II DXE driver package with `EFI_DRIVER_BINDING_PROTOCOL` and `EFI_BLOCK_IO2_PROTOCOL`.
2. Bring up IPv4/IPv6 and TCP using UEFI network protocols.
3. Implement the required Ceph messenger framing and CephX authentication.
4. Query Monitor metadata and discover the RBD image and OSD map.
5. Read RBD format 2 metadata and translate block ranges into ordered Ceph objects.
6. Publish a read-only `EFI_BLOCK_IO_PROTOCOL` device and let UEFI Boot Manager discover ESP/GPT boot entries.
7. Add OVMF-based positive and negative tests, firmware logs, and recovery behavior.

Credentials must be stored using a firmware-specific protected or authenticated configuration mechanism. A Ceph key is never passed through an iPXE command line or guest kernel command line by this design.

## Legacy BIOS implementation sequence

1. Establish NIC discovery and UNDI/PXE-compatible network access, or provide a dedicated NIC Option ROM.
2. Reuse the Ceph/RBD protocol core where language and memory constraints permit.
3. Detect a BIOS boot image layout and expose the RBD block range through the loader's own internal block interface.
4. Load the kernel/initramfs or BIOS boot sector and transfer control to the guest.
5. Test with SeaBIOS/libvirt before any physical-NIC validation.

The first Legacy BIOS target is a second-stage loader. A BIOS Option ROM requires substantially more ROM/RAM and firmware integration work and should be built only after the protocol core is stable.

## Building and testing

Requirements vary by EDK II release and Linux distribution. The repository will provide scripts for:

- downloading or mounting an EDK II workspace
- building the UEFI DXE/UEFI driver module
- creating an OVMF test ROM/flash image
- starting a libvirt or QEMU VM
- validating GPT, ESP, and EFI executable discovery

No physical BIOS, firmware image, or EDK II build tree should be committed to this repository. OVMF, EDK II, Ceph packages, and generated firmware binaries must remain host/build artifacts.

Run the host-side parser/layout/BIOS-core tests with `sh tests/run-core-tests.sh`. Build the DXE package with `scripts/build-edk2.sh <EDK2_WORKSPACE> <PACKAGES_PATH>` after installing an EDK II workspace. An OVMF firmware-start smoke test can be run with `bash scripts/run-ovmf-ceph-rbd.sh [OVMF_CODE_FD] [KVM|TCG] [OVMF_VARS_FD]`; it uses a temporary copy of the variable store and does not load this driver or connect to a Ceph cluster. Neither host tests nor the OVMF smoke test substitute for an EDK II build or end-to-end boot.

For a libvirt VM that boots an existing UEFI disk image directly from Ceph RBD through QEMU's native RBD backend, see [the libvirt/OVMF guide](docs/libvirt-rbd-uefi.md) and its domain/secret examples under `config/`. This host-side storage path does not implement Ceph in guest firmware.

## Existing guest-image prototype

`scripts/local-top/ceph-rbd` remains an initramfs-tools prototype for a guest that starts first and maps RBD later. It is **not** firmware-side RBD boot and is not used by the UEFI or BIOS firmware paths.

## Security

- Restrict the CephX identity to the required pool and image operations.
- Protect Ceph credentials and firmware configuration from unauthenticated updates.
- Verify signed firmware and EFI executables with Secure Boot where supported.
- Run the first boot tests from read-only clones or snapshots.
- Do not share an ordinary writable filesystem across multiple firmware-started machines; use one RBD clone per host unless the guest filesystem explicitly supports cluster-wide access.

## Validation checklist

- OVMF starts the UEFI driver and identifies a Ceph-backed block device.
- GPT partition discovery exposes the ESP.
- UEFI can read and launch the EFI executable.
- SeaBIOS or a second-stage loader reaches the Ceph Monitor and reads the RBD boot image.
- Monitor/OSD failure and retry behavior is observable in firmware logs.
- The image boot chain works with an equivalent read-only snapshot for repeatable tests.
