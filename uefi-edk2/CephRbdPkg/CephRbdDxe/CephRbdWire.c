#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "CephRbdWire.h"

STATIC
EFI_STATUS
CephWireRequire (
  IN CONST CEPH_WIRE_CURSOR  *Cursor,
  IN UINTN                   Length
  )
{
  if ((Cursor == NULL) || (Length > Cursor->Remaining) ||
      ((Cursor->Data == NULL) && (Length != 0)))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CephWireWriterRequire (
  IN CONST CEPH_WIRE_WRITER  *Writer,
  IN UINTN                   Length
  )
{
  if ((Writer == NULL) || (Length > Writer->Remaining) ||
      ((Writer->Data == NULL) && (Length != 0)))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephWireInit (
  OUT CEPH_WIRE_CURSOR  *Cursor,
  IN CONST VOID         *Data,
  IN UINTN              Length
  )
{
  if ((Cursor == NULL) || ((Data == NULL) && (Length != 0))) {
    return EFI_INVALID_PARAMETER;
  }

  Cursor->Data      = (CONST UINT8 *)Data;
  Cursor->Remaining = Length;
  return EFI_SUCCESS;
}

EFI_STATUS
CephWireWriterInit (
  OUT CEPH_WIRE_WRITER  *Writer,
  OUT VOID              *Data,
  IN UINTN              Length
  )
{
  if ((Writer == NULL) || ((Data == NULL) && (Length != 0))) {
    return EFI_INVALID_PARAMETER;
  }

  Writer->Data      = (UINT8 *)Data;
  Writer->Remaining = Length;
  Writer->Written   = 0;
  return EFI_SUCCESS;
}

EFI_STATUS
CephWireReadU8 (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  OUT UINT8                *Value
  )
{
  EFI_STATUS  Status;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephWireRequire (Cursor, sizeof (*Value));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Value = Cursor->Data[0];
  Cursor->Data++;
  Cursor->Remaining--;
  return EFI_SUCCESS;
}

EFI_STATUS
CephWireReadLe16 (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  OUT UINT16               *Value
  )
{
  EFI_STATUS  Status;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephWireRequire (Cursor, sizeof (*Value));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Value = (UINT16)Cursor->Data[0] | ((UINT16)Cursor->Data[1] << 8);
  Cursor->Data += 2;
  Cursor->Remaining -= 2;
  return EFI_SUCCESS;
}

EFI_STATUS
CephWireReadLe32 (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  OUT UINT32               *Value
  )
{
  EFI_STATUS  Status;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephWireRequire (Cursor, sizeof (*Value));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Value = (UINT32)Cursor->Data[0]
           | ((UINT32)Cursor->Data[1] << 8)
           | ((UINT32)Cursor->Data[2] << 16)
           | ((UINT32)Cursor->Data[3] << 24);
  Cursor->Data += 4;
  Cursor->Remaining -= 4;
  return EFI_SUCCESS;
}

EFI_STATUS
CephWireReadLe64 (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  OUT UINT64               *Value
  )
{
  EFI_STATUS  Status;
  UINT32      Low;
  UINT32      High;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephWireRequire (Cursor, sizeof (*Value));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Low  = (UINT32)Cursor->Data[0]
         | ((UINT32)Cursor->Data[1] << 8)
         | ((UINT32)Cursor->Data[2] << 16)
         | ((UINT32)Cursor->Data[3] << 24);
  High = (UINT32)Cursor->Data[4]
         | ((UINT32)Cursor->Data[5] << 8)
         | ((UINT32)Cursor->Data[6] << 16)
         | ((UINT32)Cursor->Data[7] << 24);
  *Value = ((UINT64)High << 32) | Low;
  Cursor->Data += 8;
  Cursor->Remaining -= 8;
  return EFI_SUCCESS;
}

EFI_STATUS
CephWireReadBytes (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  OUT VOID                *Buffer,
  IN UINTN                 Length
  )
{
  EFI_STATUS  Status;

  if ((Buffer == NULL) && (Length != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephWireRequire (Cursor, Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Length != 0) {
    CopyMem (Buffer, Cursor->Data, Length);
    Cursor->Data += Length;
    Cursor->Remaining -= Length;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephWireWriteBytes (
  IN OUT CEPH_WIRE_WRITER  *Writer,
  IN CONST VOID            *Buffer,
  IN UINTN                 Length
  )
{
  EFI_STATUS  Status;

  if ((Buffer == NULL) && (Length != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephWireWriterRequire (Writer, Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Length != 0) {
    CopyMem (Writer->Data, Buffer, Length);
    Writer->Data      += Length;
    Writer->Remaining -= Length;
    Writer->Written   += Length;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephWireWriteU8 (
  IN OUT CEPH_WIRE_WRITER  *Writer,
  IN UINT8                 Value
  )
{
  return CephWireWriteBytes (Writer, &Value, sizeof (Value));
}

EFI_STATUS
CephWireWriteLe16 (
  IN OUT CEPH_WIRE_WRITER  *Writer,
  IN UINT16                Value
  )
{
  UINT8  Bytes[2];

  Bytes[0] = (UINT8)Value;
  Bytes[1] = (UINT8)(Value >> 8);
  return CephWireWriteBytes (Writer, Bytes, sizeof (Bytes));
}

EFI_STATUS
CephWireWriteLe32 (
  IN OUT CEPH_WIRE_WRITER  *Writer,
  IN UINT32                Value
  )
{
  UINT8  Bytes[4];

  Bytes[0] = (UINT8)Value;
  Bytes[1] = (UINT8)(Value >> 8);
  Bytes[2] = (UINT8)(Value >> 16);
  Bytes[3] = (UINT8)(Value >> 24);
  return CephWireWriteBytes (Writer, Bytes, sizeof (Bytes));
}

EFI_STATUS
CephWireWriteLe64 (
  IN OUT CEPH_WIRE_WRITER  *Writer,
  IN UINT64                Value
  )
{
  EFI_STATUS  Status;

  Status = CephWireWriteLe32 (Writer, (UINT32)Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return CephWireWriteLe32 (Writer, (UINT32)(Value >> 32));
}

EFI_STATUS
CephWireSkip (
  IN OUT CEPH_WIRE_CURSOR  *Cursor,
  IN UINTN                 Length
  )
{
  EFI_STATUS  Status;

  Status = CephWireRequire (Cursor, Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Length != 0) {
    Cursor->Data += Length;
    Cursor->Remaining -= Length;
  }

  return EFI_SUCCESS;
}
