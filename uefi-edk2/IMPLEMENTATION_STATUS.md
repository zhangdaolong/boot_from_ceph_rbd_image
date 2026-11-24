## Firmware implementation status

The current UEFI package is a **scaffold**, not a working Ceph boot driver. It contains bounded Ceph wire cursor primitives and a pure RBD striping extent mapper. Ceph messenger sessions, CephX, networking, RBD metadata negotiation, Block I/O publication, and actual boot remain unimplemented. See [`docs/firmware-rbd-protocol.md`](docs/firmware-rbd-protocol.md) and [`tests/rbd-layout-vectors.md`](tests/rbd-layout-vectors.md).
