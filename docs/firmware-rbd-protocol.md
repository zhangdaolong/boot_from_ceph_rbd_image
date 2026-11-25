# Ceph/RBD firmware protocol contract

This document separates the UEFI and BIOS firmware work into stable interfaces and explicitly marks elements that are not implemented yet.

## Implementation status

The package currently has bounds-checked little-endian wire cursors, bounded Ceph messenger v1 framing/connect negotiation/session sequencing, UEFI TCP4 transport, CephX payload codecs with AES-128 challenge-response, ticket decryption/validation and authorizer verification, Monitor/OSD RPC codecs with automatic reconnect on recoverable errors, RBD Format 2 metadata discovery and read mapping, a validated firmware configuration variable, an `EFI_DRIVER_BINDING_PROTOCOL` that maps the configured image with per-monitor failover, and a read-only `EFI_BLOCK_IO_PROTOCOL` publication path. The BIOS directory has an independently testable MBR partition loader core. The v1 session handles framed messages, monotonic send/receive sequences, ACKs, duplicates, and keepalive tags; a bounded retransmit queue adds connect-time `SEQ` recovery and reconnect replay. All of this is verified by host-side codecs/logic tests only. There is no EDK II build, OVMF integration, live Ceph interoperability test, CRUSH/OSD placement discovery, CephX challenge/ticket exchange driven from the connect flow, GPT/ESP discovery, or BIOS PXE/control transfer. Do not deploy this code to boot a system.

## MVP operations

| Operation | Required for MVP | Status |
|---|---:|---|
| UEFI NIC discovery | Yes | Not implemented; the driver relies on an already configured TCP4 controller and caller-supplied handles |
| IPv4 TCP | Yes | TCP4 child connect/send/receive/close primitive, covered by a host protocol mock; relies on an already configured firmware IPv4 address |
| Ceph messenger framing/session | Yes | Bounded v1 handshake and TCP4 session message I/O, sequence tracking, ACK/keepalive handling, bounded connect retry, connect-time `SEQ` recovery, retransmit queue and reconnect replay; not tested against a live Ceph cluster |
| CephX authentication | Yes | AES-128-CBC challenge-response (Ceph `encode_encrypt` wrapper), ticket decryption/validation and authorizer reply verification; AES-256-KRB5 and ticket renewal are not supported |
| Monitor session | Yes | `get_map` encode, `command` encode and `MON_COMMAND_ACK` decode via a synchronous RPC call; per-monitor failover at driver start and a bounded failover runner; no CRUSH map parsing |
| OSD session | Yes | `MOSDOp`/`MOSDOpReply` codecs for `READ` and `CALL` via a synchronous RPC call; recoverable reply errors reconnect and replay; no CRUSH placement/OSD discovery |
| RBD format 2 metadata | Yes | `rbd/get_id`, `get_size`, `get_features`, `get_object_prefix` and (for striping v2) `get_stripe_unit_count` class-method RPCs against `rbd_id.<name>` and `rbd_header.<id>`; no CRUSH-based placement or multi-pool data layouts |
| RBD object extent mapping | Yes | Implemented for validated plain and striping-v2 layouts |
| GPT partition discovery | Yes | Not implemented |
| Read block range | Yes | LBA-to-extent mapping, bounded object reads and read-only Block I/O wrapper implemented; host-tested for bounds/header paths, not against a cluster |
| Writes/flush | No | Out of MVP scope; both return `EFI_WRITE_PROTECTED` |

## Bounded wire cursor

`CephRbdWire.c` provides safe little-endian integer reads/writes, bounded byte copies, and skips. `CephRbdMessenger.c` encodes/decodes fixed Ceph messenger v1 connect records, bounds optional authorizer bytes, classifies ready/retry/reset/wait responses, encodes complete v1 message frames, and decodes frames with aggregate-length and CRC32C validation. `CephRbdMsgrV1Handshake.c` performs the banner/address/connect exchange. `CephRbdMsgrV1Transport.c` sends encoded frames and reads one bounded message frame, rejecting unsupported tags, oversized payloads, and invalid headers. `CephRbdMsgrV1Session.c` assigns outgoing sequence numbers; validates incoming sequence order, ignores duplicates while re-ACKing, tracks peer ACKs, and handles ACK, CLOSE, KEEPALIVE, KEEPALIVE2 and KEEPALIVE2_ACK tags. `CephRbdMsgrV1Recovery.c` adds a fixed-capacity retransmission queue, bounded connect attempts that consume `RETRY_SESSION`, `RETRY_GLOBAL`, `RESETSESSION` and `WAIT`, connect-time `SEQ` recovery using `newly_acked_seq`, and reconnect replay of unacknowledged frames. `CephRbdRpc.c` adds a bounded failover runner: recoverable RPC reply errors (`EFI_TIMEOUT`, `EFI_CONNECTION_FIN`, `EFI_DEVICE_ERROR`, `EFI_NOT_READY`) reconnect the client and retry, while hard errors advance to the next monitor. Messenger v2 is not implemented and signed message frames are rejected.

The firmware configuration blob must use a v1-enabled monitor port (6789), not the v2-only 3300. A complete client must implement v2 or explicitly negotiate/connect to a cluster endpoint with v1 enabled. `CephRbdTcp4.c` creates a TCP4 child on a selected UEFI TCP4 controller and can connect, send, receive, and close with per-operation timeouts; it uses the firmware's already configured IPv4 address. `CephRbdConfig.c` validates the fixed-size `CephRbdConfig` UEFI variable (FSID, up to four monitors, pool id, image, identity, key reference), and the DXE Driver Binding reads it to select a monitor.

`CephRbdCephX.c` parses version-1 server/authorizer challenges and response headers; encodes request headers, service-ticket requests, version-2 authorizer payloads, and version-3 authenticate payloads; bounds optional connection-secret parsing; and implements AES-128-CBC/PKCS#7 using Ceph's fixed IV and encrypted-payload wrapper for the challenge-response calculation. It also decrypts `CephXTicketBlob` payloads, parses `CephXServiceTicketInfo`/`AuthTicket`/`CryptoKey`, validates the global ID, caps and checks the timestamp window, rejects `allow_all` and non-AES-128 session keys, and verifies the authorizer reply's `nonce + 1` with optional connection-secret extraction. Only 16-byte AES keys are supported. AES-256-KRB5, ticket renewal, and a full multi-service authorizer exchange are not implemented; host vector tests verify the codecs and cryptography but do not establish interoperability with a live Ceph cluster.

## RBD striping map

`CephRbdFormat2.c` accepts already-decoded metadata, validates image size/order/features and either the default layout or striping-v2 parameters, then constructs `<object-prefix>.<16-hex-digit-object-number>` names. `CephRbdLayout.c` maps an image byte offset to one object extent. `CephRbdRpc.c` adds the `MON_GET_MAP`, `MON_COMMAND`, `OSD_OP` and `OSD_OPREPLY` codecs plus a synchronous transaction-ID-matched RPC call. `CephRbdBlockIo.c` fetches the OSD epoch, resolves `rbd_id.<name>` to an image id with `rbd/get_id`, then queries `rbd_header.<id>` with `get_size` (order then size), `get_features` (features then incompatible mask), `get_object_prefix` and, when striping v2 is enabled, `get_stripe_unit_count`. It then discovers a format-2 image and reads block ranges as bounded object reads. Discovery accepts a read-safe feature whitelist (layering, exclusive-lock, object-map, fast-diff, deep-flatten, journaling, striping v2) and rejects data-pool, migrating, non-primary, incompatible and unknown feature bits. The MVP does not implement CRUSH placement, pool/OSD resolution, multi-pool data layouts, layered images with a parent, sparse/unwritten extents (a missing object fails the read instead of returning zeroes), snapshots or writes.

## UEFI driver state machine

```text
Stopped -> Connecting -> Monitoring -> Authenticating
         -> MappingImage -> DeviceReady -> Stopped
```

`CephRbdDxe.c` implements this path in `CephRbdDriverInitialize()`: it connects the messenger client, discovers the image, fills read-only media (512-byte blocks), installs `EFI_BLOCK_IO_PROTOCOL` on the caller's controller handle, and returns `EFI_WRITE_PROTECTED` for write/flush. The DXE entry point now installs an `EFI_DRIVER_BINDING_PROTOCOL`; `Supported` accepts a controller when the configuration variable is valid, `Start` maps the image by trying each configured monitor, and `Stop` uninstalls Block I/O and closes the session. When the configuration sets `START_ON_ENTRY`, the entry point maps the image directly. Boot-manager/GPT/ESP discovery and OVMF validation are also outstanding.

## Security and protocol compatibility requirements

- Do not extend CephX cryptography from an informal description; use a vetted, compatible cryptographic implementation and test against supported Ceph releases.
- Never log Ceph keys or messenger session keys.
- Keep credentials in protected firmware storage and reject unauthenticated configuration updates.
- Reject malformed monitor, OSD, and RBD metadata lengths; cap all allocations.
- Authenticate each Ceph session using the negotiated CephX protocol.
- Fail closed on unsupported messenger features, image features, and striping layouts.

See `tests/rbd-layout-vectors.md` for deterministic layout vectors.

Run `sh tests/run-core-tests.sh` for native checks of wire bounds, CephX payload encoding and fixed AES-128 challenge-response/ticket vectors, messenger frame encoding against a fixed Ceph v1 wire vector and frame integrity, retransmit-queue acknowledgement and retry limits, monitor/OSD request and reply codecs, the RPC failover/retry policy, RBD header decoding and block-device bounds, Format 2 layout/object naming, configuration parsing/variable loading, Driver Binding Supported/Stop, and BIOS active-partition loading. The DXE sources are compiled against host stubs as part of this run, but no live TCP session or cluster read is exercised. These tests do not replace EDK II, OVMF, or Ceph interoperability testing.
