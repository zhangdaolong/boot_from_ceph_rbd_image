# Ceph/RBD firmware protocol contract

This document separates the UEFI and BIOS firmware work into stable interfaces and explicitly marks elements that are not implemented yet.

## MVP operations

| Operation | Required for MVP | Notes |
|---|---:|---|
| UEFI NIC discovery | Yes | EDK II SNP/DPC/TCP stack |
| IPv4 TCP | Yes | Initial target |
| IPv6 TCP | Later | Optional after IPv4 |
| CephX authentication | Yes | X509/TLS is not sufficient in place of CephX |
| Monitor session | Yes | Fixed list and reconnect |
| OSD session | Yes | Read operations only |
| RBD format 2 metadata | Yes | Header, object prefix and stripe layout |
| GPT partition discovery | Yes | UEFI Block I/O layer |
| Read block range | Yes | Aligned reads may be translated through object reads |
| Writes/flush | No | Read-only is the first test target |
| Snapshots | No | Fixed image or clone only |
| Live resizing | No | Fail safely if media size changes |

## UEFI driver state machine

```text
Stopped -> Connecting -> Monitoring -> Authenticating
         -> MappingImage -> DeviceReady -> Stopped
```

A driver instance must expose media only after the RBD size and object layout are known. Block reads before `MappingImage` completes must return `EFI_NOT_READY` or `EFI_DEVICE_ERROR`.

## Device interface

`EFI_BLOCK_IO_PROTOCOL` mappings must satisfy:

- `MediaId` changes when media is remounted or the backing image is replaced.
- `ReadOnly` is `TRUE` for every MVP device.
- `BlockSize` is a firmware-chosen value; exposing 512-byte logical sectors is the safest initial target.
- `LastBlock = Ceil(RBD image size / 512) - 1`.
- Out-of-range reads must return `EFI_INVALID_PARAMETER`.
- Partial object reads must be assembled in the caller buffer.
- Sparse areas read as zeroes only after RBD metadata/objects confirm a missing object.

## Legacy BIOS interface

The BIOS second-stage loader does not expose UEFI protocol tables. Its internal contract is:

```c
typedef struct {
  int (*read_sectors)(void *context, UINT64 lba,
                      UINT32 sectors, void *buffer);
  UINT64 capacity_sectors;
  UINT32 sector_size;
} BIOS_RBD_BLOCK_DEVICE;
```

A BIOS implementation can initially use `UINT32` LBAs and later add an overflow-safe `UINT64` variant. The second-stage code must fail safely rather than infer a bootable filesystem from a partial read.

## Security requirements

A firmware implementation must:

- never log Ceph keys or messenger session keys;
- keep credentials in explicit protected firmware storage;
- reject malformed monitor, OSD, and RBD metadata lengths;
- bound all object and block sizes before allocations;
- authenticate each Ceph session according to CephX;
- provide a clear recovery path after network or OSD loss.
