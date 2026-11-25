## CephRbdBoot EDK II package

This directory contains the EDK II package and portable protocol helpers for the UEFI firmware-side Ceph RBD prototype.

The package is host-testable but is **not** an end-to-end Ceph boot driver. It provides:

- `CephRbdPkg/CephRbdPkg.dec`: package declarations including `gCephRbdConfigGuid`
- `CephRbdPkg/CephRbdPkg.dsc`: package build description
- `CephRbdPkg/CephRbdDxe/CephRbdDxe.inf`: driver module
- `CephRbdPkg/CephRbdDxe/CephRbdConfig.c`: validated fixed-size configuration blob loaded from the `CephRbdConfig` UEFI variable
- `CephRbdPkg/CephRbdDxe/CephRbdDxe.c`: `EFI_DRIVER_BINDING_PROTOCOL`, read-only `EFI_BLOCK_IO_PROTOCOL`, per-monitor failover at driver start
- `CephRbdPkg/CephRbdDxe/CephRbdWire.c`, `CephRbdMessenger.c`, `CephRbdMsgrV1*.c`, `CephRbdTcp4.c`: messenger v1 framing, handshake, session, recovery/retransmit and TCP4 transport
- `CephRbdPkg/CephRbdDxe/CephRbdCephX.c`: CephX payload codecs, AES-128 challenge-response, ticket and authorizer validation
- `CephRbdPkg/CephRbdDxe/CephRbdRpc.c`: Monitor/OSD codecs, transaction-matched RPC, reconnect/failover runner
- `CephRbdPkg/CephRbdDxe/CephRbdFormat2.c`, `CephRbdLayout.c`, `CephRbdBlockIo.c`: RBD format 2 metadata, extent mapping and read-only block reads

### Firmware configuration

The driver reads a fixed-size `CEPH_RBD_CONFIG` blob from the UEFI variable named `CephRbdConfig` with GUID `9E5F1A2B-3C4D-4E5F-8A9B-0C1D2E3F4A5B`. The blob carries the FSID, up to four monitor endpoints, pool id, image name, CephX identity and a key reference. `Flag = 0x2` (`START_ON_ENTRY`) makes the DXE entry point map the image directly when no network controller can be enumerated. Keep real Ceph key material in protected firmware storage; the example INI under `config/` only demonstrates the fields. Generate the blob with `python3 scripts/make-ceph-rbd-config.py config/ceph-rbd-example.ini CephRbdConfig.bin` and provision it into the firmware variable store.

### Remaining work

No EDK II/OVMF build or live Ceph interoperability test has been run. CephX challenge/ticket exchange is not yet driven from the messenger connect flow, CRUSH-based OSD placement and GPT/ESP discovery are absent, and automatic reconnect is limited to recoverable RPC reply errors. Run host tests with `sh tests/run-core-tests.sh`; build with `scripts/build-edk2.sh <EDK2_WORKSPACE> <PACKAGES_PATH>`.
