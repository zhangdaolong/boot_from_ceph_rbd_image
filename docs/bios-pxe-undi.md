# Legacy BIOS PXE/UNDI network layer

This document describes the legacy BIOS PXE 2.1 / UNDI layer used by the
stage-2 loader to reach a Ceph monitor before the disk path is used. The UEFI
(`uefi-edk2/`) path is unchanged.

## Files

- `bios/stage2/pxe_undi.h` - `!PXE` layout, UNDI opcodes and parameter blocks,
  the platform abstraction and the public API.
- `bios/stage2/pxe_undi.c` - discovery, parameter-block encoding, Ethernet
  frame transport, operations registration.
- `bios/stage2/ceph_stage2_network.h` - stage-2 network transport types.
- `bios/stage2/ceph_stage2.c` - network bring-up that prefers PXE/UNDI and
  falls back to the original disk-only behaviour.
- `tests/host/test_pxe_undi.c` - host test with a mock UNDI implementation.

## `!PXE` discovery

`PxeUndiDiscover()` scans physical memory from `0x000C0000` to `0x000FFFFF`
byte by byte (this covers the `0xF000` ROM segment) through the platform's
`ReadBiosMemory()` callback. A candidate must:

- start with the ASCII signature `!PXE`;
- declare a `StructLength` between 8 and `sizeof(PXE_UNDI_PXE_STRUCT)`;
- have a byte checksum of zero over `StructLength` bytes;
- have `Revision == 0` (PXE 2.1);
- expose at least one non-zero code entry point.

On success the caller receives a `PXE_UNDI_CONTEXT` holding the parsed
structure, the ROM physical address and the real-mode `PXEnv` far-call entry
taken from `PXEnvCodeSeg:PXEnvCodeOffset`. A signature with a bad checksum is
reported as `PXE_UNDI_BAD_CHECKSUM`; no candidate at all is
`PXE_UNDI_NOT_FOUND`.

## Far-call abstraction

The real-mode far call is hidden behind `PXE_UNDI_PLATFORM`:

- `ReadBiosMemory()` reads ROM bytes;
- `CallApi()` performs the far call with an opcode and parameter block;
- `MakeFarPointer()` / `FarPointerToHost()` translate between host addresses
  and the 16:16 far pointers stored in UNDI parameter blocks.

Every UNDI parameter block begins with a 16-bit `Status` word, which the
wrapper zeroes before the call and re-checks afterwards. The wrappers encode
`PXENV_UNDI_STARTUP` (0x0001), `INITIALIZE` (0x0003), `GET_NIC_TYPE` (0x0012),
`GET_INFORMATION` (0x000C), `SET_STATION_ADDRESS` (0x000A), `OPEN` (0x0006),
`TRANSMIT` (0x0008), `CLOSE` (0x0007) and `SHUTDOWN` (0x0005), plus the
filter hooks `SET_PACKET_FILTER` (0x000B) and `SET_MCAST_ADDRESS` (0x0009).

PXE 2.1 has no standalone receive function; received frames are drained with
`PXENV_UNDI_ISR` (0x0014). `PXENV_UNDI_RECEIVE` in this layer is the logical
name for that ISR-based operation, so the transport stays symmetric with
transmit.

## Ethernet transport and stage-2 integration

`PxeUndiSendFrame()` builds a transmit buffer descriptor (TBD) with a single
immediate data block, sets `P_UNKNOWN` (the Ethernet header is prebuilt) and
selects unicast or broadcast from the destination MAC. `PxeUndiReceiveFrame()`
issues one ISR call and reports `PXE_UNDI_NO_PACKET` or
`PXE_UNDI_BUFFER_TOO_SMALL` as appropriate. Promiscuous and multicast
filtering are exposed through `PxeUndiSetPacketFilter()` and
`PxeUndiSetMulticast()` and are only invoked by the caller when the NIC
supports them.

`PxeUndiInstallNetOps()` registers a table of operations into
`gCephBiosPxeUndiOps`, which is defined as empty in `ceph_stage2.c`. This keeps
the disk-only core free of link-time references to the PXE module.
`CephBiosStage2InitNetwork()` then discovers and brings up the NIC, reads its
permanent MAC, opens the adapter and returns a `CEPH_BIOS_NETWORK` with
send/receive hooks. If no operations are registered or no `!PXE` is found it
returns `CEPH_BIOS_NETWORK_NOT_AVAILABLE` and the loader keeps its existing
behaviour.

## Host test

The suite is appended as a standalone entry to `tests/run-core-tests.sh` and
runs automatically:

```
sh tests/run-core-tests.sh
```

`test_pxe_undi.c` builds a fake ROM image containing a valid `!PXE`, mocks all
four platform callbacks and verifies discovery, checksum/revision rejection,
each UNDI parameter-block encoding, a transmit/receive round trip, and the
stage-2 call sequence including the no-PXE fallback.

## Limitations

- The host test verifies discovery, parameter-block layout and stage-2
  wiring only. The real-mode far call, segment selectors and physical ROM
  contents cannot be executed on a host and are represented by mocks.
- Receive uses a single ISR call per frame; the ISR `GET_NEXT` chaining used
  to drain multiple buffers in one interrupt is not implemented.
- The stage-2 transport is a raw Ethernet frame pipe. ARP/IP, DHCP and the
  Ceph monitor socket layer are out of scope for this layer.
- The scan assumes the `!PXE` signature is reachable as memory-mapped ROM in
  the `0xC0000`-`0xFFFFF` window; discovery via a `PXENV+` pointer or a
  firmware-provided far pointer is not attempted.
