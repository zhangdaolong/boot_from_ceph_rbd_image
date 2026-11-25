#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "CephRbdMessenger.h"
#include "CephRbdMsgrV1Transport.h"
#include "CephRbdTcp4.h"

STATIC
UINT32
CephMsgrV1TransportReadLe32 (
  IN CONST UINT8  *Data
  )
{
  return (UINT32)Data[0]
         | ((UINT32)Data[1] << 8)
         | ((UINT32)Data[2] << 16)
         | ((UINT32)Data[3] << 24);
}

EFI_STATUS
CephMsgrV1SendFrame (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN CONST CEPH_MSGR_V1_FRAME  *Frame,
  IN UINTN                     MaximumPayload,
  IN UINT32                    TimeoutSeconds,
  OUT VOID                     *FrameBuffer,
  IN UINTN                     FrameBufferCapacity,
  OUT UINTN                    *FrameLength
  )
{
  EFI_STATUS  Status;

  if ((Connection == NULL) || (Frame == NULL) ||
      (FrameBuffer == NULL) || (FrameLength == NULL) ||
      (TimeoutSeconds == 0) || !Connection->Connected ||
      (Connection->Tcp4 == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *FrameLength = 0;
  Status = CephMsgrEncodeV1Frame (
             Frame,
             MaximumPayload,
             FrameBuffer,
             FrameBufferCapacity,
             FrameLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdTcp4Send (
             Connection,
             FrameBuffer,
             *FrameLength,
             TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    *FrameLength = 0;
  }

  return Status;
}

EFI_STATUS
CephMsgrV1ReceiveFrameAfterTag (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN UINT8                     Tag,
  IN UINTN                     MaximumPayload,
  IN UINT32                    TimeoutSeconds,
  OUT VOID                     *FrameBuffer,
  IN UINTN                     FrameBufferCapacity,
  OUT CEPH_MSGR_V1_FRAME       *Frame,
  OUT UINTN                    *FrameLength
  )
{
  EFI_STATUS  Status;
  UINT8       *Bytes;
  UINT32      FrontLength;
  UINT32      MiddleLength;
  UINT32      DataLength;
  UINTN       PayloadLength;
  UINTN       RequiredLength;

  if ((Connection == NULL) || (FrameBuffer == NULL) ||
      (Frame == NULL) || (FrameLength == NULL) ||
      (TimeoutSeconds == 0) || !Connection->Connected ||
      (Connection->Tcp4 == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *FrameLength = 0;
  if (FrameBufferCapacity <
      (1 + CEPH_MSGR_V1_HEADER_SIZE + CEPH_MSGR_V1_FOOTER_SIZE))
  {
    return EFI_BUFFER_TOO_SMALL;
  }

  Bytes = (UINT8 *)FrameBuffer;
  if (Tag != CEPH_MSGR_V1_TAG_MESSAGE) {
    return EFI_UNSUPPORTED;
  }

  Bytes[0] = Tag;
  Status = CephRbdTcp4ReceiveExact (
             Connection,
             Bytes + 1,
             CEPH_MSGR_V1_HEADER_SIZE,
             TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (CephMsgrCrc32c (
        Bytes + 1,
        CEPH_MSGR_V1_HEADER_CRC_OFFSET
        ) != CephMsgrV1TransportReadLe32 (
               Bytes + 1 + CEPH_MSGR_V1_HEADER_CRC_OFFSET
               ))
  {
    return EFI_CRC_ERROR;
  }

  if ((Bytes[1 + 47] != 0) || (Bytes[1 + 48] != 0)) {
    return EFI_COMPROMISED_DATA;
  }

  FrontLength  = CephMsgrV1TransportReadLe32 (Bytes + 1 + 22);
  MiddleLength = CephMsgrV1TransportReadLe32 (Bytes + 1 + 26);
  DataLength   = CephMsgrV1TransportReadLe32 (Bytes + 1 + 30);
  if ((FrontLength > MAX_UINT32 - MiddleLength) ||
      (FrontLength + MiddleLength > MAX_UINT32 - DataLength))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  PayloadLength = (UINTN)FrontLength + MiddleLength + DataLength;
  if (PayloadLength > MaximumPayload) {
    return EFI_BAD_BUFFER_SIZE;
  }

  RequiredLength = 1 + CEPH_MSGR_V1_HEADER_SIZE + PayloadLength +
                   CEPH_MSGR_V1_FOOTER_SIZE;
  if (RequiredLength > FrameBufferCapacity) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = CephRbdTcp4ReceiveExact (
             Connection,
             Bytes + 1 + CEPH_MSGR_V1_HEADER_SIZE,
             PayloadLength + CEPH_MSGR_V1_FOOTER_SIZE,
             TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephMsgrDecodeV1Frame (
             FrameBuffer,
             RequiredLength,
             MaximumPayload,
             Frame
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *FrameLength = RequiredLength;
  return EFI_SUCCESS;
}

EFI_STATUS
CephMsgrV1ReceiveFrame (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN UINTN                     MaximumPayload,
  IN UINT32                    TimeoutSeconds,
  OUT VOID                     *FrameBuffer,
  IN UINTN                     FrameBufferCapacity,
  OUT CEPH_MSGR_V1_FRAME       *Frame,
  OUT UINTN                    *FrameLength
  )
{
  EFI_STATUS  Status;
  UINT8       Tag;

  if ((Connection == NULL) || (FrameBuffer == NULL) ||
      (Frame == NULL) || (FrameLength == NULL) ||
      (TimeoutSeconds == 0) || !Connection->Connected ||
      (Connection->Tcp4 == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *FrameLength = 0;
  Status = CephRbdTcp4ReceiveExact (
             Connection,
             &Tag,
             sizeof (Tag),
             TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return CephMsgrV1ReceiveFrameAfterTag (
           Connection,
           Tag,
           MaximumPayload,
           TimeoutSeconds,
           FrameBuffer,
           FrameBufferCapacity,
           Frame,
           FrameLength
           );
}
