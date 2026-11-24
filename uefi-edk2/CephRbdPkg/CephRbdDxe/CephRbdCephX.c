#include <Uefi.h>
#include "CephRbdCephX.h"
#include "CephRbdWire.h"

EFI_STATUS
CephXDecodeServerChallenge (
  IN CONST VOID              *Buffer,
  IN UINTN                   BufferLength,
  OUT CEPHX_SERVER_CHALLENGE *Challenge
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT8             StructVersion;

  if ((Buffer == NULL) || (Challenge == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephWireInit (&Cursor, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireReadU8 (&Cursor, &StructVersion);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (StructVersion != 1) {
    return EFI_UNSUPPORTED;
  }

  Status = CephWireReadLe64 (&Cursor, &Challenge->ServerChallenge);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return (Cursor.Remaining == 0) ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}

EFI_STATUS
CephXDecodeResponseHeader (
  IN CONST VOID            *Buffer,
  IN UINTN                 BufferLength,
  OUT CEPHX_RESPONSE_HEADER *Response
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT32            EncodedStatus;

  if ((Buffer == NULL) || (Response == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephWireInit (&Cursor, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireReadLe16 (&Cursor, &Response->RequestType);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireReadLe32 (&Cursor, &EncodedStatus);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Cursor.Remaining != 0) {
    return EFI_COMPROMISED_DATA;
  }

  Response->Status = (INT32)EncodedStatus;
  return EFI_SUCCESS;
}

EFI_STATUS
CephXEncodeRequestHeader (
  IN UINT16        RequestType,
  OUT VOID         *Buffer,
  IN UINTN         BufferLength,
  OUT UINTN        *EncodedLength
  )
{
  CEPH_WIRE_WRITER  Writer;
  EFI_STATUS        Status;

  if ((Buffer == NULL) || (EncodedLength == NULL) ||
      ((RequestType != CEPHX_REQUEST_GET_AUTH_SESSION_KEY) &&
       (RequestType != CEPHX_REQUEST_GET_PRINCIPAL_KEY) &&
       (RequestType != CEPHX_REQUEST_GET_ROTATING_KEY)))
  {
    return EFI_INVALID_PARAMETER;
  }

  *EncodedLength = 0;
  if (BufferLength < sizeof (RequestType)) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = CephWireWriterInit (&Writer, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireWriteLe16 (&Writer, RequestType);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
  return EFI_SUCCESS;
}

EFI_STATUS
CephXEncodeServiceTicketRequest (
  IN UINT32        ServiceMask,
  OUT VOID         *Buffer,
  IN UINTN         BufferLength,
  OUT UINTN        *EncodedLength
  )
{
  CEPH_WIRE_WRITER  Writer;
  EFI_STATUS        Status;

  if ((Buffer == NULL) || (EncodedLength == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *EncodedLength = 0;
  if (BufferLength < 5) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = CephWireWriterInit (&Writer, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireWriteU8 (&Writer, 1);
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, ServiceMask);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
  return EFI_SUCCESS;
}

EFI_STATUS
CephXDecodeAuthorizeChallenge (
  IN CONST VOID                  *Buffer,
  IN UINTN                       BufferLength,
  OUT CEPHX_AUTHORIZE_CHALLENGE  *Challenge
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT8             StructVersion;

  if ((Buffer == NULL) || (Challenge == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephWireInit (&Cursor, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireReadU8 (&Cursor, &StructVersion);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (StructVersion != 1) {
    return EFI_UNSUPPORTED;
  }

  Status = CephWireReadLe64 (&Cursor, &Challenge->ServerChallenge);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return (Cursor.Remaining == 0) ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}

EFI_STATUS
CephXEncodeAuthorize (
  IN UINT64        Nonce,
  IN BOOLEAN       HaveChallenge,
  IN UINT64        ServerChallengePlusOne,
  OUT VOID         *Buffer,
  IN UINTN         BufferLength,
  OUT UINTN        *EncodedLength
  )
{
  CEPH_WIRE_WRITER  Writer;
  EFI_STATUS        Status;

  if ((Buffer == NULL) || (EncodedLength == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *EncodedLength = 0;
  if (BufferLength < 18) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = CephWireWriterInit (&Writer, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireWriteU8 (&Writer, 2);
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, Nonce);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteU8 (&Writer, HaveChallenge ? 1 : 0);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (
               &Writer,
               HaveChallenge ? ServerChallengePlusOne : 0);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
  return EFI_SUCCESS;
}

EFI_STATUS
CephXDecodeAuthorizeReply (
  IN CONST VOID            *Buffer,
  IN UINTN                 BufferLength,
  OUT CEPHX_AUTHORIZE_REPLY *Reply,
  OUT VOID                 *ConnectionSecret,
  IN UINTN                 ConnectionSecretCapacity
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT8             StructVersion;
  UINT32            SecretLength;

  if ((Buffer == NULL) || (Reply == NULL) ||
      ((ConnectionSecret == NULL) && (ConnectionSecretCapacity != 0)))
  {
    return EFI_INVALID_PARAMETER;
  }

  Reply->NoncePlusOne = 0;
  Reply->ConnectionSecretLength = 0;
  Status = CephWireInit (&Cursor, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireReadU8 (&Cursor, &StructVersion);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((StructVersion != 1) && (StructVersion != 2)) {
    return EFI_UNSUPPORTED;
  }

  Status = CephWireReadLe64 (&Cursor, &Reply->NoncePlusOne);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (StructVersion == 2) {
    Status = CephWireReadLe32 (&Cursor, &SecretLength);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Reply->ConnectionSecretLength = SecretLength;
    if ((SecretLength > ConnectionSecretCapacity) ||
        ((ConnectionSecret == NULL) && (SecretLength != 0)))
    {
      return EFI_BUFFER_TOO_SMALL;
    }

    Status = CephWireReadBytes (&Cursor, ConnectionSecret, SecretLength);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return (Cursor.Remaining == 0) ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}

EFI_STATUS
CephXEncodeAuthenticate (
  IN UINT64         ClientChallenge,
  IN UINT64         ChallengeResponse,
  IN UINT64         OldTicketSecretId,
  IN CONST VOID     *OldTicketBlob,
  IN UINTN          OldTicketBlobLength,
  IN UINT32         RequestedKeys,
  OUT VOID         *Buffer,
  IN UINTN          BufferLength,
  OUT UINTN         *EncodedLength
  )
{
  CEPH_WIRE_WRITER  Writer;
  EFI_STATUS        Status;
  UINTN             RequiredLength;

  if ((Buffer == NULL) || (EncodedLength == NULL) ||
      ((OldTicketBlob == NULL) && (OldTicketBlobLength != 0)) ||
      (OldTicketBlobLength > MAX_UINT32) ||
      (OldTicketBlobLength > MAX_UINTN - 34))
  {
    return EFI_INVALID_PARAMETER;
  }

  *EncodedLength = 0;
  RequiredLength = 34 + OldTicketBlobLength;
  if (BufferLength < RequiredLength) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = CephWireWriterInit (&Writer, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // CephXAuthenticate v3 and nested CephXTicketBlob v1.
  Status = CephWireWriteU8 (&Writer, 3);
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, ClientChallenge);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, ChallengeResponse);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteU8 (&Writer, 1);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, OldTicketSecretId);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, (UINT32)OldTicketBlobLength);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteBytes (&Writer, OldTicketBlob, OldTicketBlobLength);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, RequestedKeys);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
  return EFI_SUCCESS;
}
