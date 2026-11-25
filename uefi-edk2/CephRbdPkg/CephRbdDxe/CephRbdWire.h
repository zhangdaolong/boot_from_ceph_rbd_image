#ifndef CEPH_RBD_WIRE_H_
#define CEPH_RBD_WIRE_H_

#include <Uefi.h>

typedef struct {
  CONST UINT8  *Data;
  UINTN        Remaining;
} CEPH_WIRE_CURSOR;

typedef struct {
  UINT8  *Data;
  UINTN  Remaining;
  UINTN  Written;
} CEPH_WIRE_WRITER;

EFI_STATUS
CephWireInit (
  OUT CEPH_WIRE_CURSOR  *Cursor,
  IN CONST VOID         *Data,
  IN UINTN              Length
  );

EFI_STATUS
CephWireWriterInit (
  OUT CEPH_WIRE_WRITER  *Writer,
  OUT VOID              *Data,
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
CephWireWriteU8 (
  IN OUT CEPH_WIRE_WRITER  *Writer,
  IN UINT8                 Value
  );

EFI_STATUS
CephWireWriteLe16 (
  IN OUT CEPH_WIRE_WRITER  *Writer,
  IN UINT16                Value
  );

EFI_STATUS
CephWireWriteLe32 (
  IN OUT CEPH_WIRE_WRITER  *Writer,
  IN UINT32                Value
  );

EFI_STATUS
CephWireWriteLe64 (
  IN OUT CEPH_WIRE_WRITER  *Writer,
  IN UINT64                Value
  );

EFI_STATUS
CephWireWriteBytes (
  IN OUT CEPH_WIRE_WRITER  *Writer,
  IN CONST VOID            *Buffer,
  IN UINTN                 Length
  );

EFI_STATUS
CephWireSkip (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  IN UINTN                 Length
  );

#endif
