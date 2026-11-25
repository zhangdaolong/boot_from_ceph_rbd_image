## Firmware implementation status

The current UEFI package has host-tested protocol code, a validated firmware configuration source, a Driver Binding protocol that maps the configured image, a read-only Block I/O publication path, and a CephX MAuth handshake driven from the messenger connect flow. A host-side live probe has validated connect + CephX authentication + `MON_GET_MAP` against a real Ceph 22 monitor. It is still **not a working Ceph boot driver**: no EDK II/OVMF build has been run, the final AUTH ticket session key is not parsed, CRUSH/OSD placement is absent, and GPT/ESP discovery is missing.

| Area | Implemented | Still missing |
|---|---|---|
| UEFI DXE | EDK II package/build entrypoint; TCP4 child transport; read-only `EFI_BLOCK_IO_PROTOCOL` wrapper (`ReadBlocks`, write/flush return `EFI_WRITE_PROTECTED`, media/BlockIo setup and `InstallProtocolInterface` in `CephRbdDriverInitialize`); validated fixed-size configuration blob in a UEFI variable (`CephRbdConfig`); `EFI_DRIVER_BINDING_PROTOCOL` with Supported/Start/Stop, per-monitor failover in Start, and optional entry-point mapping | DHCP/static network configuration, booting through UEFI Boot Manager/GPT/ESP |
| Ceph messenger | Messenger v1 connect codec, TCP4-backed handshake/frame I/O, send/receive sequence checks, ACK/keepalive control tags, bounded connect retry, connect-time `SEQ` recovery, retransmit queue and reconnect replay; recoverable RPC errors automatically reconnect and replay via `CephRpcCallAuto` | Messenger v2, live cluster validation |
| CephX | Challenge/authenticate/request/ticket-request/authorizer payload codecs; AES-128-CBC challenge-response; ticket blob decryption and service-ticket validation; authorizer reply `nonce + 1` verification with connection-secret parsing | Ticket renewal/renewal session, AES-256-KRB5, full multi-service authorizer exchange, live cluster validation |
| Monitor/OSD RPC | `MON_GET_MAP` and `MON_COMMAND` encode, `MON_COMMAND_ACK` and OSD map epoch decode, `MOSDOp`/`MOSDOpReply` codecs for `READ` and `CALL`, synchronous transaction-matched RPC call, bounded failover runner with reconnect-on-timeout, monitor selection at driver start | CRUSH map parsing, OSD placement discovery, reconnect triggered by send failures rather than reply timeouts |
| RBD Format 2 | Validate decoded metadata, plain/striping-v2 layouts, extent mapping and object names; `rbd/get_id`/`get_size`/`get_features`/`get_object_prefix`/`get_stripe_unit_count` discovery; read-safe feature whitelist; block range reads with bounds checks | CRUSH placement and pool/OSD resolution, multi-pool data layouts, layered images with a parent, sparse/unwritten extents, snapshots, cluster-backed reads |
| Legacy BIOS | Host-testable active-MBR partition selection and boot-sector load | PXE/UNDI adapter, Ceph read backend, real-mode trampoline/control transfer |
| Tests | Host tests for wire/CephX/recovery codecs and vectors, retransmit queue, RPC encoders/decoders and failover policy, RBD header decode and bounds, layout helpers, BIOS core, mocked TCP4 I/O, v1 handshake/frame/session, configuration parsing/variable loading and Driver Binding Supported/Stop; DXE sources compile against host stubs | EDK II build, OVMF integration, Ceph cluster interoperability, BlockIo installation/read against a live cluster |

See [`docs/firmware-rbd-protocol.md`](../docs/firmware-rbd-protocol.md) for the
protocol contract, [`docs/boot-vm-from-ceph-rbd.md`](../docs/boot-vm-from-ceph-rbd.md)
for how to configure a VM to boot from RBD (both the working hypervisor-side
QEMU/libvirt path and the in-progress firmware-side path), and
[`tests/rbd-layout-vectors.md`](../tests/rbd-layout-vectors.md) for layout vectors.
