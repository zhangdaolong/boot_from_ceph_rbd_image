# Ceph/RBD firmware protocol contract

This document separates the UEFI and BIOS firmware work into stable interfaces and explicitly marks elements that are not implemented yet.

## Implementation status

The package currently has bounds-checked little-endian wire cursors, a bounded Ceph messenger v1 message decoder, CephX payload codecs, and normalized RBD Format 2 layout/object-name helpers. The BIOS directory has an independently testable MBR partition loader core. These are protocol building blocks only; they do **not** implement network connections, CephX cryptography/ticket handling, Monitor/OSD RPCs, RBD metadata discovery, a bootable EFI Block I/O device, or BIOS PXE/control transfer. Do not deploy this code to boot a system.

## MVP operations

| Operation | Required for MVP | Status |
|---|---:|---|
| UEFI NIC discovery | Yes | Not implemented |
| IPv4 TCP | Yes | Not implemented |
| Ceph messenger framing/session | Yes | Bounded v1 message decoder only; connection/session state is not implemented |
| CephX authentication | Yes | Challenge/authenticate payload codecs only; secret-key crypto, tickets and authorizers are not implemented |
| Monitor session | Yes | Not implemented |
| OSD session | Yes | Not implemented |
| RBD format 2 metadata | Yes | Normalized metadata validation only; on-wire metadata RPCs are not implemented |
| RBD object extent mapping | Yes | Implemented for validated plain and striping-v2 layouts |
| GPT partition discovery | Yes | Not implemented |
| Read block range | Yes | Not implemented |
| Writes/flush | No | Out of MVP scope |

## Bounded wire cursor

`CephRbdWire.c` provides safe little-endian integer reads/writes, bounded byte copies, and skips. `CephRbdMessenger.c` encodes/decodes the fixed Ceph messenger v1 connect records, bounds the optional authorizer bytes, classifies ready/retry/reset/wait responses, and decodes message frames with aggregate-length and CRC32C validation. It rejects incomplete and signed frames and exposes front/middle/data slices. It does not implement banner/address exchange, TCP, retries, or message sequence/ACK handling; it is not messenger v2 and does not establish a session.

The sample monitor configuration uses the standard messenger v2 port (3300), while this decoder only handles v1 message frames. A complete client must implement v2 or explicitly negotiate/connect to a cluster endpoint with v1 enabled (typically port 6789); no transport is wired in yet.

`CephRbdCephX.c` decodes the version-1 server challenge and response header, and encodes the version-3 authenticate payload with its nested ticket-blob encoding. Challenge-response computation and ticket decryption are deliberately outside this codec and have not yet been implemented; do not treat successful payload encoding as authentication.

## RBD striping map

`CephRbdFormat2.c` accepts already-decoded metadata, validates image size/order/features and either the default layout or striping-v2 parameters, then constructs `<object-prefix>.<16-hex-digit-object-number>` names. `CephRbdLayout.c` maps an image byte offset to one object extent. These helpers do not parse OSD class-method replies, discover image IDs, resolve pools/OSDs, or fetch data.

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

Run `sh tests/run-core-tests.sh` for native checks of wire bounds, CephX payload encoding, messenger frame integrity, Format 2 layout/object naming, and BIOS active-partition loading. These do not replace EDK II, OVMF, or Ceph interoperability testing.
