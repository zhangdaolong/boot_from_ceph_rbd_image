# Ceph/RBD firmware protocol contract

This document separates the UEFI and BIOS firmware work into stable interfaces and explicitly marks elements that are not implemented yet.

## Implementation status

The package currently has bounds-checked little-endian wire cursor helpers and an RBD striping extent mapper. These are protocol building blocks only; they do **not** implement a Ceph connection, CephX authentication, Monitor/OSD messaging, RBD metadata negotiation, or a bootable Block I/O device. Do not deploy this code to boot a system.

## MVP operations

| Operation | Required for MVP | Status |
|---|---:|---|
| UEFI NIC discovery | Yes | Not implemented |
| IPv4 TCP | Yes | Not implemented |
| Ceph messenger framing/session | Yes | Not implemented |
| CephX authentication | Yes | Not implemented; requires audited crypto and protocol compatibility tests |
| Monitor session | Yes | Not implemented |
| OSD session | Yes | Not implemented |
| RBD format 2 metadata | Yes | Not implemented |
| RBD object extent mapping | Yes | Layout math helper only |
| GPT partition discovery | Yes | Not implemented |
| Read block range | Yes | Not implemented |
| Writes/flush | No | Out of MVP scope |

## Bounded wire cursor

`CephRbdWire.c` provides safe little-endian integer reads, bounded byte copies, and skips. Every operation checks the remaining buffer length before advancing the cursor. These helpers are deliberately transport-neutral and do not assume a particular Ceph messenger version or message structure.

## RBD striping map

`CephRbdLayout.c` maps an image byte offset to one object extent using the format 2 layout tuple (image size, object size, stripe unit, stripe count). The caller must repeat the mapping for remaining bytes. Layout validation rejects zero values and object sizes that are not multiples of stripe unit. The helper does not parse RBD headers, resolve object names, discover OSDs, or fetch data.

## UEFI driver state machine

```text
Stopped -> Connecting -> Monitoring -> Authenticating
         -> MappingImage -> DeviceReady -> Stopped
```

A future driver must expose media only after authentication and RBD metadata validation complete. Until then this package intentionally publishes no Block I/O device.

## Security and protocol compatibility requirements

- Do not implement CephX cryptography from an informal description; use a vetted, compatible cryptographic implementation and test against supported Ceph releases.
- Never log Ceph keys or messenger session keys.
- Keep credentials in protected firmware storage and reject unauthenticated configuration updates.
- Reject malformed monitor, OSD, and RBD metadata lengths; cap all allocations.
- Authenticate each Ceph session using the negotiated CephX protocol.
- Fail closed on unsupported messenger features, image features, and striping layouts.

See `tests/rbd-layout-vectors.md` for deterministic layout vectors.
