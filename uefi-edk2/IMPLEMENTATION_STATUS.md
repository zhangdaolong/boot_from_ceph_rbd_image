## Firmware implementation status

The current UEFI package is **not a working Ceph boot driver**. It contains tested message framing and metadata/layout primitives, but no network session or device publication path.

| Area | Implemented | Still missing |
|---|---|---|
| UEFI DXE | EDK II package and build entrypoint; low-level TCP4 child transport module | Driver binding, DHCP/static network configuration, Ceph session wiring, Block I/O child handle |
| Ceph messenger | Messenger v1 connect record codec/reply classifier, complete message-frame encoding/decoding, length bounds, CRC32C checks | Banner/address exchange, sequence/ACK handling, reconnect driver, TCP integration |
| CephX | Challenge/authenticate/request/ticket-request/authorizer payload codecs; AES-128-CBC challenge-response | Ticket decryption/validation/renewal, AES-256-KRB5, authorizer encryption/verification |
| RBD Format 2 | Validates decoded image metadata, supports plain/striping-v2 layouts, maps extents, builds object names | Monitor/OSD method requests and replies, image-id/header discovery, unsupported image feature handling in RPC flow |
| Legacy BIOS | Host-testable active-MBR partition selection and boot-sector load | PXE/UNDI adapter, Ceph read backend, real-mode trampoline/control transfer |
| Tests | Host tests for parsers, RBD helpers, BIOS core and a mocked TCP4 protocol | EDK II build, OVMF integration, Ceph cluster interoperability |

See [`docs/firmware-rbd-protocol.md`](../docs/firmware-rbd-protocol.md) and [`tests/rbd-layout-vectors.md`](../tests/rbd-layout-vectors.md).
