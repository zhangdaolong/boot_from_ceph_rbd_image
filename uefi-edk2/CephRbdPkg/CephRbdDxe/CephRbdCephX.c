#include <Uefi.h>
#include <Library/BaseCryptLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include "CephRbdCephX.h"
#include "CephRbdWire.h"

#define CEPHX_AUTH_ENC_MAGIC  0xff009cad8826aa55ULL

STATIC CONST UINT8  mCephAesIv[16] = {
  'c', 'e', 'p', 'h', 's', 'a', 'g', 'e',
  'y', 'u', 'd', 'a', 'g', 'r', 'e', 'g'
};

STATIC
EFI_STATUS
CephXAes128CbcEncrypt (
  IN CONST UINT8  *Key,
  IN CONST VOID   *Plaintext,
  IN UINTN        PlaintextLength,
  OUT VOID        *Ciphertext,
  IN UINTN        CiphertextCapacity,
  OUT UINTN       *CiphertextLength
  )
{
  VOID     *AesContext;
  UINT8    *Padded;
  UINTN    PaddedLength;
  UINTN    PaddingLength;
  UINTN    AesContextSize;
  BOOLEAN  Result;
  EFI_STATUS Status;

  if ((Key == NULL) || (Plaintext == NULL) || (Ciphertext == NULL) ||
      (CiphertextLength == NULL) || (PlaintextLength == 0) ||
      (PlaintextLength > CEPHX_MAX_ENCRYPTED_PAYLOAD) ||
      (PlaintextLength > MAX_UINTN - 16))
  {
    return EFI_INVALID_PARAMETER;
  }

  *CiphertextLength = 0;
  PaddingLength = 16 - (PlaintextLength & 15);
  PaddedLength = PlaintextLength + PaddingLength;
  if (CiphertextCapacity < PaddedLength) {
    return EFI_BUFFER_TOO_SMALL;
  }

  AesContextSize = AesGetContextSize ();
  if (AesContextSize == 0) {
    return EFI_UNSUPPORTED;
  }

  AesContext = AllocateZeroPool (AesContextSize);
  if (AesContext == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Padded = AllocatePool (PaddedLength);
  if (Padded == NULL) {
    ZeroMem (AesContext, AesContextSize);
    FreePool (AesContext);
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Padded, Plaintext, PlaintextLength);
  SetMem (Padded + PlaintextLength, PaddingLength, (UINT8)PaddingLength);
  Result = AesInit (AesContext, Key, 128);
  if (Result) {
    Result = AesCbcEncrypt (
               AesContext,
               Padded,
               PaddedLength,
               mCephAesIv,
               (UINT8 *)Ciphertext);
  }

  Status = Result ? EFI_SUCCESS : EFI_UNSUPPORTED;
  if (!EFI_ERROR (Status)) {
    *CiphertextLength = PaddedLength;
  }

  ZeroMem (Padded, PaddedLength);
  FreePool (Padded);
  ZeroMem (AesContext, AesContextSize);
  FreePool (AesContext);
  return Status;
}

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

EFI_STATUS
CephXEncryptAes128Payload (
  IN CONST UINT8  *Key,
  IN UINTN        KeyLength,
  IN CONST VOID   *Payload,
  IN UINTN        PayloadLength,
  OUT VOID        *Buffer,
  IN UINTN        BufferLength,
  OUT UINTN       *EncodedLength
  )
{
  CEPH_WIRE_WRITER  Writer;
  EFI_STATUS        Status;
  UINT8             Plaintext[CEPHX_MAX_ENCRYPTED_PAYLOAD];
  UINT8             Ciphertext[CEPHX_MAX_ENCRYPTED_PAYLOAD];
  UINTN             PlaintextLength;
  UINTN             CiphertextLength;

  if ((Key == NULL) || ((Payload == NULL) && (PayloadLength != 0)) ||
      (Buffer == NULL) || (EncodedLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (KeyLength != CEPHX_AES_KEY_SIZE) {
    return EFI_UNSUPPORTED;
  }

  if (PayloadLength > CEPHX_MAX_ENCRYPTED_PAYLOAD - 10) {
    return EFI_INVALID_PARAMETER;
  }

  *EncodedLength = 0;
  PlaintextLength = 9 + PayloadLength;
  Plaintext[0] = 1;
  Plaintext[1] = (UINT8)CEPHX_AUTH_ENC_MAGIC;
  Plaintext[2] = (UINT8)(CEPHX_AUTH_ENC_MAGIC >> 8);
  Plaintext[3] = (UINT8)(CEPHX_AUTH_ENC_MAGIC >> 16);
  Plaintext[4] = (UINT8)(CEPHX_AUTH_ENC_MAGIC >> 24);
  Plaintext[5] = (UINT8)(CEPHX_AUTH_ENC_MAGIC >> 32);
  Plaintext[6] = (UINT8)(CEPHX_AUTH_ENC_MAGIC >> 40);
  Plaintext[7] = (UINT8)(CEPHX_AUTH_ENC_MAGIC >> 48);
  Plaintext[8] = (UINT8)(CEPHX_AUTH_ENC_MAGIC >> 56);
  if (PayloadLength != 0) {
    CopyMem (Plaintext + 9, Payload, PayloadLength);
  }
  Status = CephXAes128CbcEncrypt (
             Key,
             Plaintext,
             PlaintextLength,
             Ciphertext,
             sizeof (Ciphertext),
             &CiphertextLength);
  ZeroMem (Plaintext, sizeof (Plaintext));
  if (EFI_ERROR (Status)) {
    ZeroMem (Ciphertext, sizeof (Ciphertext));
    return Status;
  }

  if (BufferLength < (4 + CiphertextLength)) {
    ZeroMem (Ciphertext, sizeof (Ciphertext));
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = CephWireWriterInit (&Writer, Buffer, BufferLength);
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, (UINT32)CiphertextLength);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteBytes (&Writer, Ciphertext, CiphertextLength);
  }

  ZeroMem (Ciphertext, sizeof (Ciphertext));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
  return EFI_SUCCESS;
}

EFI_STATUS
CephXComputeAesChallengeResponse (
  IN CONST UINT8  *Key,
  IN UINTN        KeyLength,
  IN UINT64       ServerChallenge,
  IN UINT64       ClientChallenge,
  OUT UINT64      *ChallengeResponse
  )
{
  CEPH_WIRE_WRITER  Writer;
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT8             Challenge[17];
  UINT8             Encoded[64];
  UINTN             EncodedLength;
  UINT64            Word;
  UINT64            Result;

  if ((Key == NULL) || (ChallengeResponse == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (KeyLength != CEPHX_AES_KEY_SIZE) {
    return EFI_UNSUPPORTED;
  }

  Status = CephWireWriterInit (&Writer, Challenge, sizeof (Challenge));
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteU8 (&Writer, 1);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, ServerChallenge);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, ClientChallenge);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephXEncryptAes128Payload (
             Key,
             KeyLength,
             Challenge,
             sizeof (Challenge),
             Encoded,
             sizeof (Encoded),
             &EncodedLength);
  ZeroMem (Challenge, sizeof (Challenge));
  if (EFI_ERROR (Status)) {
    ZeroMem (Encoded, sizeof (Encoded));
    return Status;
  }

  Status = CephWireInit (&Cursor, Encoded, EncodedLength);
  Result = 0;
  while (!EFI_ERROR (Status) && (Cursor.Remaining >= sizeof (Word))) {
    Status = CephWireReadLe64 (&Cursor, &Word);
    if (!EFI_ERROR (Status)) {
      Result ^= Word;
    }
  }

  ZeroMem (Encoded, sizeof (Encoded));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *ChallengeResponse = Result;
  return EFI_SUCCESS;
}
