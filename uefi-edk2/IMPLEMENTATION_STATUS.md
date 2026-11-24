## Firmware implementation status

The current UEFI package is **not a working Ceph boot driver**. It contains tested message framing and metadata/layout primitives, but no network session or device publication path.

| Area | Implemented | Still missing |
|---|---|---|
| UEFI DXE | EDK II package and build entrypoint | Driver binding, network configuration, Block I/O child handle |
| Ceph messenger | Messenger v1 message-frame parsing, length bounds, CRC32C checks | Banner/connect negotiation, sequence/ACK handling, reconnects, TCP transport |
| CephX | Server-challenge and authenticate payload codecs | Secret-key crypto, ticket validation/renewal, authorizers |
| RBD Format 2 | Validates decoded image metadata, supports plain/striping-v2 layouts, maps extents, builds object names | Monitor/OSD method requests and replies, image-id/header discovery, unsupported image feature handling in RPC flow |
| Legacy BIOS | Host-testable active-MBR partition selection and boot-sector load | PXE/UNDI adapter, Ceph read backend, real-mode trampoline/control transfer |
| Tests | Host tests for parsers, RBD helper and BIOS core | EDK II build, OVMF integration, Ceph cluster interoperability |

See [`docs/firmware-rbd-protocol.md`](../docs/firmware-rbd-protocol.md) and [`tests/rbd-layout-vectors.md`](../tests/rbd-layout-vectors.md).
