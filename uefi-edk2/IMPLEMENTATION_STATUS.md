## Firmware implementation status

The current UEFI package has host-tested protocol code and a read-only Block I/O publication path, but it is **not a working Ceph boot driver**. The DXE entry point does not yet call `CephRbdDriverInitialize()` because no firmware configuration source is defined, and no EDK II/OVMF build or live Ceph interoperability test has been run.

| Area | Implemented | Still missing |
|---|---|---|
| UEFI DXE | EDK II package/build entrypoint; TCP4 child transport; read-only `EFI_BLOCK_IO_PROTOCOL` wrapper (`ReadBlocks`, write/flush return `EFI_WRITE_PROTECTED`, media/BlockIo setup and `InstallProtocolInterface` in `CephRbdDriverInitialize`) | Driver binding, DHCP/static network configuration, firmware config source (monitor address, identity/key reference, pool/image, FSID), booting through UEFI Boot Manager/GPT/ESP |
| Ceph messenger | Messenger v1 connect codec, TCP4-backed handshake/frame I/O, send/receive sequence checks, ACK/keepalive control tags, bounded connect retry, connect-time `SEQ` recovery, retransmit queue and reconnect replay | Messenger v2, automatic reconnect on RPC timeout, live cluster validation |
| CephX | Challenge/authenticate/request/ticket-request/authorizer payload codecs; AES-128-CBC challenge-response; ticket blob decryption and service-ticket validation; authorizer reply `nonce + 1` verification with connection-secret parsing | Ticket renewal/renewal session, AES-256-KRB5, full multi-service authorizer exchange, live cluster validation |
| Monitor/OSD RPC | `MON_GET_MAP` and `MON_COMMAND` encode, `MON_COMMAND_ACK` and OSD map epoch decode, `MOSDOp`/`MOSDOpReply` codecs for `READ` and `CALL`, synchronous transaction-matched RPC call | Multi-monitor failover, CRUSH map parsing, OSD placement discovery, automatic retry/reconnect on RPC timeout |
| RBD Format 2 | Validate decoded metadata, plain/striping-v2 layouts, extent mapping and object names; `rbd/get_id`/`get_size`/`get_features`/`get_object_prefix`/`get_stripe_unit_count` discovery; read-safe feature whitelist; block range reads with bounds checks | CRUSH placement and pool/OSD resolution, multi-pool data layouts, layered images with a parent, sparse/unwritten extents, snapshots, cluster-backed reads |
| Legacy BIOS | Host-testable active-MBR partition selection and boot-sector load | PXE/UNDI adapter, Ceph read backend, real-mode trampoline/control transfer |
| Tests | Host tests for wire/CephX/recovery codecs and vectors, retransmit queue, RPC encoders/decoders, RBD header decode and bounds, layout helpers, BIOS core, mocked TCP4 I/O, v1 handshake/frame/session; DXE sources compile against host stubs | EDK II build, OVMF integration, Ceph cluster interoperability, BlockIo protocol installation/read exercise |

See [`docs/firmware-rbd-protocol.md`](../docs/firmware-rbd-protocol.md) and [`tests/rbd-layout-vectors.md`](../tests/rbd-layout-vectors.md).
