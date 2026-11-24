#ifndef CEPH_RBD_WIRE_H_
#define CEPH_RBD_WIRE_H_

#include <Uefi.h>

typedef struct {
  CONST UINT8  *Data;
  UINTN        Remaining;
} CEPH_WIRE_CURSOR;

EFI_STATUS
CephWireInit (
  OUT CEPH_WIRE_CURSOR  *Cursor,
  IN CONST VOID         *Data,
  IN UINTN              Length
  );

EFI_STATUS
CephWireReadU8 (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  OUT UINT8                *Value
  );

EFI_STATUS
CephWireReadLe16 (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  OUT UINT16               *Value
  );

EFI_STATUS
CephWireReadLe32 (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  OUT UINT32               *Value
  );

EFI_STATUS
CephWireReadLe64 (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  OUT UINT64               *Value
  );

EFI_STATUS
CephWireReadBytes (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  OUT VOID                *Buffer,
  IN UINTN                 Length
  );

EFI_STATUS
CephWireSkip (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  IN UINTN                 Length
  );

#endif
