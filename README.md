# Direct Ceph RBD Firmware Boot

This repository is developing firmware-side paths that make a Ceph RBD image visible directly during UEFI or Legacy BIOS startup.

> Status: the repository now contains host-tested Ceph messenger v1 framing and connect negotiation, bounded connect retry and connect-time `SEQ` recovery with a retransmission queue and reconnect replay, CephX AES-128 challenge-response plus ticket decryption/validation and authorizer verification, Monitor/OSD RPC codecs with automatic reconnect on recoverable transport errors, RBD Format 2 metadata discovery and read-only block reads, a read-only `EFI_BLOCK_IO_PROTOCOL` publication path, a firmware configuration source (`CephRbdConfig`) with an `EFI_DRIVER_BINDING_PROTOCOL` that maps the configured image (trying each configured monitor), and a BIOS MBR partition-loader core. The CephX MAuth challenge-response handshake is driven from the messenger connect flow, including parsing of the final AUTH ticket session key and retention of the AUTH ticket blob. A host-side live probe has validated, against a real Ceph 22 cluster: connect + authentication + `MON_GET_MAP` + `osd getmap` (canonical OSDMap), OSD service-ticket request, CRUSH/placement of object names to PGs and acting OSD sets, decoding of primary OSD v1 endpoints, an authenticated primary OSD connection (CephX challenge + `SEQ` recovery), and a **real `CEPH_OSD_OP_READ` returning the correct 16 bytes of object data** (`result=0`). The wire details required for that read are covered: 13-byte legacy footers when `MSG_AUTH` is not advertised, leaving `RESEND_ON_SPLIT` unadvertised so the OSD canonicalizes the object-hash PG, correct `old_pg_t` (`u16 preferred`/`u16 ps`/`u32 pool`) encoding, and the `MOSDOpReply` v8 front layout. Both the CRUSH decoder and OSDMap placement decoder are verified against real-cluster vectors and a live OSDMap. It is still not a bootable Ceph client: no EDK II/OVMF build has been run, the DXE `BlockIo` read path still issues OSD ops over the monitor connection rather than reusing the validated primary-OSD client, GPT/ESP discovery is absent, `KEY_REFERENCE` credentials and AES-256-KRB5 are unsupported, and BIOS PXE/control transfer remains incomplete. Do not use this firmware to boot a production system.

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
docs/                     Protocol contract and VM boot guides
scripts/                  Build and QEMU test helpers
tests/                    Host-side protocol/layout/loader tests and vectors
```

## Booting a VM from Ceph RBD

See [`docs/boot-vm-from-ceph-rbd.md`](docs/boot-vm-from-ceph-rbd.md) for the
complete guide. In short:

- **Path A — hypervisor-side RBD (works today):** QEMU/libvirt authenticates to
  Ceph through `librbd` and presents the image as a virtio-blk disk. The guest
  does not know the disk is RBD. Use this for a bootable VM now.
- **Path B — firmware-side RBD (this project, in progress):** OVMF loads
  `CephRbdDxe.efi`, which reads the image itself and exposes a read-only
  `EFI_BLOCK_IO_PROTOCOL`. The host needs no Ceph client or keyring. This path
  is not yet able to boot a VM; see
  [`IMPLEMENTATION_STATUS.md`](uefi-edk2/IMPLEMENTATION_STATUS.md).

Both paths use the same development cluster values
(FSID `223a96f9-5a21-47e8-98a8-722553609174`, pool `rbd`/id `4`, monitor v1
ports `40338/40340/40342`) documented in the guide.

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

Run the host-side protocol/layout/BIOS-core tests with `sh tests/run-core-tests.sh`. These compile the DXE sources against host stubs and cover wire codecs, CephX cryptography and ticket validation, the retransmit queue, monitor/OSD RPC encoders and decoders, RBD header decoding and block-device bounds checks. Build the DXE package with `scripts/build-edk2.sh <EDK2_WORKSPACE> <PACKAGES_PATH>` after installing an EDK II workspace. An OVMF firmware-start smoke test can be run with `bash scripts/run-ovmf-ceph-rbd.sh [OVMF_CODE_FD] [KVM|TCG] [OVMF_VARS_FD]`; it uses a temporary copy of the variable store and does not load this driver or connect to a Ceph cluster. Neither host tests nor the OVMF smoke test substitute for an EDK II build or end-to-end boot.

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
