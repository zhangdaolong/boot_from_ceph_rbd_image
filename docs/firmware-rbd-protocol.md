# Ceph/RBD firmware protocol contract

This document separates the UEFI and BIOS firmware work into stable interfaces and explicitly marks elements that are not implemented yet.

## Implementation status

The package currently has bounds-checked little-endian wire cursors, bounded Ceph messenger v1 framing/connect negotiation/session sequencing, UEFI TCP4 transport, CephX payload codecs and AES-128 challenge-response, and normalized RBD Format 2 layout/object-name helpers. The BIOS directory has an independently testable MBR partition loader core. The v1 session handles framed messages, monotonic send/receive sequences, ACKs, duplicates, and keepalive tags. Connect retry/reconnect, CephX ticket decryption/validation and authorizer verification, Monitor/OSD RPCs, RBD metadata discovery, a bootable EFI Block I/O device, and BIOS PXE/control transfer are not implemented. Do not deploy this code to boot a system.

## MVP operations

| Operation | Required for MVP | Status |
|---|---:|---|
| UEFI NIC discovery | Yes | Not implemented |
| IPv4 TCP | Yes | TCP4 child connect/send/receive/close primitive, covered by a host protocol mock; relies on an already configured firmware IPv4 address |
| Ceph messenger framing/session | Yes | Bounded v1 handshake and TCP4 session message I/O, sequence tracking, ACK and keepalive handling; connect retry/reconnect and full CephX authorizer flow are not implemented |
| CephX authentication | Yes | AES-128-CBC challenge-response (Ceph `encode_encrypt` wrapper) and payload codecs are implemented; ticket decryption/validation and authorizer verification are not implemented |
| Monitor session | Yes | Not implemented |
| OSD session | Yes | Not implemented |
| RBD format 2 metadata | Yes | Normalized metadata validation only; on-wire metadata RPCs are not implemented |
| RBD object extent mapping | Yes | Implemented for validated plain and striping-v2 layouts |
| GPT partition discovery | Yes | Not implemented |
| Read block range | Yes | Not implemented |
| Writes/flush | No | Out of MVP scope |

## Bounded wire cursor

`CephRbdWire.c` provides safe little-endian integer reads/writes, bounded byte copies, and skips. `CephRbdMessenger.c` encodes/decodes fixed Ceph messenger v1 connect records, bounds optional authorizer bytes, classifies ready/retry/reset/wait responses, encodes complete v1 message frames, and decodes frames with aggregate-length and CRC32C validation. `CephRbdMsgrV1Handshake.c` performs the banner/address/connect exchange. `CephRbdMsgrV1Transport.c` sends encoded frames and reads one bounded message frame, rejecting unsupported tags, oversized payloads, and invalid headers. `CephRbdMsgrV1Session.c` assigns outgoing sequence numbers; validates incoming sequence order, ignores duplicates while re-ACKing, tracks peer ACKs, and handles ACK, CLOSE, KEEPALIVE, KEEPALIVE2 and KEEPALIVE2_ACK tags. It has no retry/reconnect or retransmission queue, does not complete the connect-time `SEQ` recovery path, and does not implement messenger v2. Signed message frames are rejected.

The sample monitor configuration uses the standard messenger v2 port (3300), while this messenger core only handles v1. A complete client must implement v2 or explicitly negotiate/connect to a cluster endpoint with v1 enabled (typically port 6789). `CephRbdTcp4.c` creates a TCP4 child on a selected UEFI TCP4 controller and can connect, send, receive, and close with per-operation timeouts; it uses the firmware's already configured IPv4 address. The v1 handshake and reliable message-session primitives are wired to TCP4, but authentication, retry/reconnect, and Monitor/OSD operations are not.

`CephRbdCephX.c` parses version-1 server/authorizer challenges and response headers; encodes request headers, service-ticket requests, version-2 authorizer payloads, and version-3 authenticate payloads; bounds optional connection-secret parsing; and implements AES-128-CBC/PKCS#7 using Ceph's fixed IV and encrypted-payload wrapper for the challenge-response calculation. Only 16-byte AES keys are supported. AES-256-KRB5, ticket decryption/validation, and authorizer cryptography are not implemented; successful challenge-response calculation or payload encoding alone does not establish an authenticated session.

## RBD striping map

`CephRbdFormat2.c` accepts already-decoded metadata, validates image size/order/features and either the default layout or striping-v2 parameters, then constructs `<object-prefix>.<16-hex-digit-object-number>` names. `CephRbdLayout.c` maps an image byte offset to one object extent. These helpers do not parse OSD class-method replies, discover image IDs, resolve pools/OSDs, or fetch data.

## UEFI driver state machine

```text
Stopped -> Connecting -> Monitoring -> Authenticating
         -> MappingImage -> DeviceReady -> Stopped
```

A future driver must expose media only after authentication and RBD metadata validation complete. Until then this package intentionally publishes no Block I/O device.

## Security and protocol compatibility requirements

- Do not extend CephX cryptography from an informal description; use a vetted, compatible cryptographic implementation and test against supported Ceph releases.
- Never log Ceph keys or messenger session keys.
- Keep credentials in protected firmware storage and reject unauthenticated configuration updates.
- Reject malformed monitor, OSD, and RBD metadata lengths; cap all allocations.
- Authenticate each Ceph session using the negotiated CephX protocol.
- Fail closed on unsupported messenger features, image features, and striping layouts.

See `tests/rbd-layout-vectors.md` for deterministic layout vectors.

Run `sh tests/run-core-tests.sh` for native checks of wire bounds, CephX payload encoding and a fixed AES-128 challenge-response vector, messenger frame encoding against a fixed Ceph v1 wire vector and frame integrity, Format 2 layout/object naming, and BIOS active-partition loading. These do not replace EDK II, OVMF, or Ceph interoperability testing.
