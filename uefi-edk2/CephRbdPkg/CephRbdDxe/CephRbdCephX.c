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
CephXAes128CbcDecrypt (
  IN CONST UINT8  *Key,
  IN CONST VOID   *Ciphertext,
  IN UINTN        CiphertextLength,
  OUT VOID        *Plaintext,
  IN UINTN        PlaintextCapacity,
  OUT UINTN       *PlaintextLength
  )
{
  VOID     *AesContext;
  UINT8    *Unpadded;
  UINTN     AesContextSize;
  UINTN     PaddingLength;
  UINTN     Index;
  BOOLEAN   Result;
  EFI_STATUS Status;

  if ((Key == NULL) || (Ciphertext == NULL) || (Plaintext == NULL) ||
      (PlaintextLength == NULL) || (CiphertextLength == 0) ||
      (CiphertextLength > CEPHX_MAX_ENCRYPTED_PAYLOAD) ||
      ((CiphertextLength & 15) != 0) ||
      (CiphertextLength > PlaintextCapacity))
  {
    return EFI_INVALID_PARAMETER;
  }

  *PlaintextLength = 0;
  AesContextSize = AesGetContextSize ();
  if (AesContextSize == 0) {
    return EFI_UNSUPPORTED;
  }

  AesContext = AllocateZeroPool (AesContextSize);
  Unpadded = AllocatePool (CiphertextLength);
  if ((AesContext == NULL) || (Unpadded == NULL)) {
    if (AesContext != NULL) {
      ZeroMem (AesContext, AesContextSize);
      FreePool (AesContext);
    }

    if (Unpadded != NULL) {
      ZeroMem (Unpadded, CiphertextLength);
      FreePool (Unpadded);
    }

    return EFI_OUT_OF_RESOURCES;
  }

  Result = AesInit (AesContext, Key, 128);
  if (Result) {
    Result = AesCbcDecrypt (
               AesContext,
               Ciphertext,
               CiphertextLength,
               mCephAesIv,
               Unpadded
               );
  }

  Status = Result ? EFI_SUCCESS : EFI_UNSUPPORTED;
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  // PKCS#7 padding: the final byte is 1..16 and every padding byte equals it.
  PaddingLength = Unpadded[CiphertextLength - 1];
  if ((PaddingLength == 0) || (PaddingLength > 16) ||
      (PaddingLength > CiphertextLength))
  {
    Status = EFI_COMPROMISED_DATA;
    goto Done;
  }

  for (Index = CiphertextLength - PaddingLength; Index < CiphertextLength;
       Index++)
  {
    if (Unpadded[Index] != PaddingLength) {
      Status = EFI_COMPROMISED_DATA;
      goto Done;
    }
  }

  CopyMem (Plaintext, Unpadded, CiphertextLength - PaddingLength);
  *PlaintextLength = CiphertextLength - PaddingLength;

Done:
  ZeroMem (Unpadded, CiphertextLength);
  FreePool (Unpadded);
  ZeroMem (AesContext, AesContextSize);
  FreePool (AesContext);
  return Status;
}

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
  UINT8             Challenge[16];
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

  // CephXChallengeBlob::encode emits only the two u64 challenges; the outer
  // struct_v/magic wrapper is added by CephXEncryptAes128Payload. Adding a
  // struct_v here would produce a different AES plaintext and the monitor
  // would reject the response with EACCES.
  Status = CephWireWriterInit (&Writer, Challenge, sizeof (Challenge));
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

EFI_STATUS
CephXDecryptAes128Payload (
  IN CONST UINT8  *Key,
  IN UINTN        KeyLength,
  IN CONST VOID   *Buffer,
  IN UINTN        BufferLength,
  OUT VOID        *Payload,
  IN UINTN        PayloadCapacity,
  OUT UINTN       *PayloadLength
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  CEPH_WIRE_CURSOR  Plain;
  EFI_STATUS        Status;
  CONST UINT8      *Ciphertext;
  UINT32            CiphertextLength;
  UINT8             Plaintext[CEPHX_MAX_ENCRYPTED_PAYLOAD];
  UINTN             PlaintextLength;
  UINT8             StructVersion;
  UINT64            Magic;

  if ((Key == NULL) || (Buffer == NULL) || (Payload == NULL) ||
      (PayloadLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (KeyLength != CEPHX_AES_KEY_SIZE) {
    return EFI_UNSUPPORTED;
  }

  *PayloadLength = 0;
  Status = CephWireInit (&Cursor, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireReadLe32 (&Cursor, &CiphertextLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((CiphertextLength == 0) ||
      (CiphertextLength > Cursor.Remaining) ||
      ((CiphertextLength & 15) != 0) ||
      (CiphertextLength > sizeof (Plaintext)))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  Ciphertext = Cursor.Data;
  Status = CephXAes128CbcDecrypt (
             Key,
             Ciphertext,
             CiphertextLength,
             Plaintext,
             sizeof (Plaintext),
             &PlaintextLength
             );
  if (EFI_ERROR (Status)) {
    ZeroMem (Plaintext, sizeof (Plaintext));
    return Status;
  }

  Status = CephWireInit (&Plain, Plaintext, PlaintextLength);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Plain, &StructVersion);
  }

  if (!EFI_ERROR (Status) && (StructVersion != 1)) {
    Status = EFI_UNSUPPORTED;
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe64 (&Plain, &Magic);
  }

  if (!EFI_ERROR (Status) && (Magic != CEPHX_AUTH_ENC_MAGIC)) {
    Status = EFI_COMPROMISED_DATA;
  }

  if (!EFI_ERROR (Status) && (Plain.Remaining > PayloadCapacity)) {
    Status = EFI_BUFFER_TOO_SMALL;
  }

  if (!EFI_ERROR (Status)) {
    CopyMem (Payload, Plain.Data, Plain.Remaining);
    *PayloadLength = Plain.Remaining;
  }

  ZeroMem (Plaintext, sizeof (Plaintext));
  return Status;
}

// Extract the AUTH ticket session key from the final GET_AUTH_SESSION_KEY
// reply's result_bl. The buffer is:
//   u16 request_type, s32 status                     (CephXResponseHeader)
//   u8 service_ticket_reply_v (=1), u32 num_tickets
//   repeated { u32 service_id, u8 ticket_v (=1),
//              <encode_encrypt(CephXServiceTicket, client_secret)>,
//              u8 should_encrypt_ticket, bufferlist service_ticket_bl }
// The CephXServiceTicket inside the encrypted blob is:
//   u8 struct_v (=1), CryptoKey session_key, utime_t validity.
// The session key is encrypted with the client's own secret, so no service
// secret is required to recover it. Only AES-128 keys are accepted.
EFI_STATUS
CephXDecodeAuthSessionKey (
  IN  CONST VOID   *Buffer,
  IN  UINTN        BufferLength,
  IN  CONST UINT8  *ClientSecret,
  IN  UINTN        ClientSecretLength,
  OUT UINT8        *SessionKey,
  IN  UINTN        SessionKeyCapacity,
  OUT UINT64       *TicketSecretId,  OPTIONAL
  OUT VOID         *TicketBlob,  OPTIONAL
  IN  UINTN        TicketBlobCapacity,
  OUT UINTN        *TicketBlobLength  OPTIONAL
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT8             Plaintext[CEPHX_MAX_ENCRYPTED_PAYLOAD];
  UINTN             PlaintextLength;
  UINT16            RequestType;
  UINT32            StatusCode;
  UINT8             ReplyVersion;
  UINT32            TicketCount;
  UINT32            ServiceId;
  UINT8             TicketVersion;
  UINT8             ShouldEncrypt;
  UINT32            BlobLength;
  UINT8             StructVersion;
  UINT16            KeyType;
  UINT32            CreatedSeconds;
  UINT32            CreatedNanoseconds;
  UINT16            KeyLength;
  UINT32            ValiditySeconds;
  UINT32            ValidityNanoseconds;
  UINT32            Index;
  BOOLEAN           Found;
  UINT8             EncodedTicketBuf[CEPHX_MAX_TICKET_BLOB];

  if ((Buffer == NULL) || (ClientSecret == NULL) || (SessionKey == NULL) ||
      (SessionKeyCapacity < CEPHX_AES_KEY_SIZE))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (ClientSecretLength != CEPHX_AES_KEY_SIZE) {
    return EFI_UNSUPPORTED;
  }

  Status = CephWireInit (&Cursor, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // CephXResponseHeader.
  Status = CephWireReadLe16 (&Cursor, &RequestType);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &StatusCode);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (StatusCode != 0) {
    return EFI_ACCESS_DENIED;
  }

  // Service ticket reply header.
  Status = CephWireReadU8 (&Cursor, &ReplyVersion);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &TicketCount);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((ReplyVersion != 1) || (TicketCount == 0) || (TicketCount > 16)) {
    return EFI_UNSUPPORTED;
  }

  Found = FALSE;
  for (Index = 0; Index < TicketCount; Index++) {
    // service_id, then ticket struct version.
    Status = CephWireReadLe32 (&Cursor, &ServiceId);
    if (!EFI_ERROR (Status)) {
      Status = CephWireReadU8 (&Cursor, &TicketVersion);
    }
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (TicketVersion != 1) {
      return EFI_UNSUPPORTED;
    }

    // encode_encrypt(CephXServiceTicket, client_secret): bufferlist prefix
    // followed by ciphertext. CephXDecryptAes128Payload validates the
    // struct_v/magic wrapper and returns the inner CephXServiceTicket.
    Status = CephXDecryptAes128Payload (
               ClientSecret,
               ClientSecretLength,
               Cursor.Data,
               Cursor.Remaining,
               Plaintext,
               sizeof (Plaintext),
               &PlaintextLength
               );
    if (EFI_ERROR (Status)) {
      ZeroMem (Plaintext, sizeof (Plaintext));
      return Status;
    }

    // Advance the outer cursor past the encrypted bufferlist. The decrypted
    // ciphertext length equals the encoded length minus its u32 prefix; we
    // recover the consumed bytes from the decrypt helper's input cursor.
    {
      CEPH_WIRE_CURSOR  EncCursor;
      UINT32            EncLength;

      Status = CephWireInit (&EncCursor, Cursor.Data, Cursor.Remaining);
      if (!EFI_ERROR (Status)) {
        Status = CephWireReadLe32 (&EncCursor, &EncLength);
      }
      if (EFI_ERROR (Status) || (EncLength > EncCursor.Remaining)) {
        ZeroMem (Plaintext, sizeof (Plaintext));
        return EFI_BAD_BUFFER_SIZE;
      }
      Status = CephWireSkip (&Cursor, sizeof (UINT32) + EncLength);
    }
    if (EFI_ERROR (Status)) {
      ZeroMem (Plaintext, sizeof (Plaintext));
      return Status;
    }

    // Parse the inner CephXServiceTicket only for the AUTH ticket; the
    // session key there is what authenticates subsequent monitor RPCs.
    if (ServiceId == CEPHX_ENTITY_TYPE_AUTH) {
      {
        CEPH_WIRE_CURSOR  TicketCursor;

        Status = CephWireInit (&TicketCursor, Plaintext, PlaintextLength);
        if (!EFI_ERROR (Status)) {
          Status = CephWireReadU8 (&TicketCursor, &StructVersion);
        }
        if (!EFI_ERROR (Status) && (StructVersion != 1)) {
          Status = EFI_UNSUPPORTED;
        }

        // CryptoKey session_key: u16 type, utime_t created, u16 len, bytes.
        if (!EFI_ERROR (Status)) {
          Status = CephWireReadLe16 (&TicketCursor, &KeyType);
        }
        if (!EFI_ERROR (Status)) {
          Status = CephWireReadLe32 (&TicketCursor, &CreatedSeconds);
        }
        if (!EFI_ERROR (Status)) {
          Status = CephWireReadLe32 (&TicketCursor, &CreatedNanoseconds);
        }
        if (!EFI_ERROR (Status)) {
          Status = CephWireReadLe16 (&TicketCursor, &KeyLength);
        }
        if (!EFI_ERROR (Status) && (KeyLength != CEPHX_AES_KEY_SIZE)) {
          Status = EFI_UNSUPPORTED;
        }
        if (!EFI_ERROR (Status)) {
          Status = CephWireReadBytes (&TicketCursor, SessionKey, KeyLength);
        }
        // utime_t validity: u32 sec, u32 nsec. Present but not needed.
        if (!EFI_ERROR (Status)) {
          Status = CephWireReadLe32 (&TicketCursor, &ValiditySeconds);
        }
        if (!EFI_ERROR (Status)) {
          Status = CephWireReadLe32 (&TicketCursor, &ValidityNanoseconds);
        }
        (VOID)CreatedSeconds;
        (VOID)CreatedNanoseconds;
        (VOID)ValiditySeconds;
        (VOID)ValidityNanoseconds;
        (VOID)KeyType;
      }
      ZeroMem (Plaintext, sizeof (Plaintext));
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Found = TRUE;
    }

    // should_encrypt_ticket + service_ticket_bl (bufferlist).
    Status = CephWireReadU8 (&Cursor, &ShouldEncrypt);
    if (!EFI_ERROR (Status)) {
      Status = CephWireReadLe32 (&Cursor, &BlobLength);
    }
    if (EFI_ERROR (Status) || (BlobLength > Cursor.Remaining)) {
      return EFI_BAD_BUFFER_SIZE;
    }

    // For the AUTH ticket, retain the CephXTicketBlob so a later
    // GET_PRINCIPAL_SESSION_KEY request can present it as old_ticket. The
    // blob bytes stay opaque: they are encrypted with the AUTH service
    // secret that only the monitor knows. All we need is secret_id and the
    // raw ciphertext.
    if ((ServiceId == CEPHX_ENTITY_TYPE_AUTH) && Found &&
        (TicketBlob != NULL) && (TicketBlobLength != NULL))
    {
      CEPHX_TICKET_BLOB  Ticket;
      CONST UINT8        *EncodedTicket;
      UINTN              EncodedTicketLength;

      if (ShouldEncrypt == 0) {
        // Fresh global_id: service_ticket_bl is a plain encoded CephXTicketBlob.
        EncodedTicket = Cursor.Data;
        EncodedTicketLength = BlobLength;
      } else {
        // Reclaimed global_id: the blob was encrypted with the AUTH session
        // key we just recovered. Decrypt it before decoding the wrapper.
        UINT8  TicketPlain[CEPHX_MAX_TICKET_BLOB];
        UINTN  TicketPlainLength;

        Status = CephXDecryptAes128Payload (
                   SessionKey,
                   CEPHX_AES_KEY_SIZE,
                   Cursor.Data,
                   BlobLength,
                   TicketPlain,
                   sizeof (TicketPlain),
                   &TicketPlainLength
                   );
        if (EFI_ERROR (Status)) {
          ZeroMem (TicketPlain, sizeof (TicketPlain));
          ZeroMem (Plaintext, sizeof (Plaintext));
          return Status;
        }

        CopyMem (EncodedTicketBuf, TicketPlain, TicketPlainLength);
        EncodedTicket = EncodedTicketBuf;
        EncodedTicketLength = TicketPlainLength;
        ZeroMem (TicketPlain, sizeof (TicketPlain));
      }

      Status = CephXDecodeTicketBlob (
                 EncodedTicket,
                 EncodedTicketLength,
                 &Ticket
                 );
      if (EFI_ERROR (Status)) {
        ZeroMem (Plaintext, sizeof (Plaintext));
        return Status;
      }

      if (TicketBlobCapacity < Ticket.BlobLength) {
        ZeroMem (Plaintext, sizeof (Plaintext));
        return EFI_BUFFER_TOO_SMALL;
      }

      if (TicketSecretId != NULL) {
        *TicketSecretId = Ticket.SecretId;
      }
      CopyMem (TicketBlob, Ticket.Blob, Ticket.BlobLength);
      *TicketBlobLength = Ticket.BlobLength;
    }

    Status = CephWireSkip (&Cursor, BlobLength);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  ZeroMem (Plaintext, sizeof (Plaintext));
  return Found ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS
CephXDecodeTicketBlob (
  IN CONST VOID         *Buffer,
  IN UINTN              BufferLength,
  OUT CEPHX_TICKET_BLOB  *Ticket
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT8             StructVersion;
  UINT32            BlobLength;

  if ((Buffer == NULL) || (Ticket == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Ticket->SecretId = 0;
  Ticket->Blob = NULL;
  Ticket->BlobLength = 0;

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

  Status = CephWireReadLe64 (&Cursor, &Ticket->SecretId);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &BlobLength);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((BlobLength == 0) || (BlobLength > Cursor.Remaining)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Ticket->Blob = Cursor.Data;
  Ticket->BlobLength = BlobLength;
  return EFI_SUCCESS;
}

EFI_STATUS
CephXDecodeAndValidateTicket (
  IN CONST CEPHX_TICKET_BLOB       *Ticket,
  IN CONST UINT8                   *ServiceSecret,
  IN UINTN                         ServiceSecretLength,
  IN UINT64                        ExpectedGlobalId,
  IN UINT64                        NowSeconds,
  OUT CEPHX_SERVICE_TICKET_INFO    *Info
  )
{
  EFI_STATUS        Status;
  UINT8             Plaintext[CEPHX_MAX_ENCRYPTED_PAYLOAD];
  UINTN             PlaintextLength;
  CEPH_WIRE_CURSOR  Cursor;
  UINT8             StructVersion;
  UINT64            Value64;
  UINT32            Value32;
  UINT8             EntityType;
  UINT8             AllowAll;
  UINTN             NameLength;
  UINTN             CapsLength;

  if ((Ticket == NULL) || (Ticket->Blob == NULL) ||
      (Ticket->BlobLength == 0) || (ServiceSecret == NULL) ||
      (Info == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (ServiceSecretLength != CEPHX_AES_KEY_SIZE) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (Info, sizeof (*Info));
  Status = CephXDecryptAes128Payload (
             ServiceSecret,
             ServiceSecretLength,
             Ticket->Blob,
             Ticket->BlobLength,
             Plaintext,
             sizeof (Plaintext),
             &PlaintextLength
             );
  if (EFI_ERROR (Status)) {
    ZeroMem (Plaintext, sizeof (Plaintext));
    return Status;
  }

  Status = CephWireInit (&Cursor, Plaintext, PlaintextLength);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Cursor, &StructVersion);
  }

  // CephXServiceTicketInfo v1: struct_v, AuthTicket, CryptoKey.
  if (!EFI_ERROR (Status) && (StructVersion != 1)) {
    Status = EFI_UNSUPPORTED;
  }

  // AuthTicket v2: struct_v, EntityName, global_id, auid, created, expires,
  // AuthCapsInfo, flags.
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Cursor, &StructVersion);
  }

  if (!EFI_ERROR (Status) &&
      ((StructVersion != 1) && (StructVersion != 2)))
  {
    Status = EFI_UNSUPPORTED;
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Cursor, &EntityType);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &Value32);
  }

  NameLength = Value32;
  if (!EFI_ERROR (Status) && (NameLength > CEPHX_MAX_ENTITY_NAME)) {
    Status = EFI_BAD_BUFFER_SIZE;
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, NameLength);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe64 (&Cursor, &Info->GlobalId);
  }

  // auid was introduced in AuthTicket v2. The wire always carries the
  // CEPH_AUTH_UID_DEFAULT constant in modern encoders.
  if (!EFI_ERROR (Status) && (StructVersion >= 2)) {
    Status = CephWireReadLe64 (&Cursor, &Info->Auid);
  } else if (!EFI_ERROR (Status)) {
    Info->Auid = CEPHX_AUTH_UID_DEFAULT;
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &Info->CreatedSeconds);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &Info->CreatedNanoseconds);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &Info->ExpiresSeconds);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &Info->ExpiresNanoseconds);
  }

  // AuthCapsInfo v1: struct_v, allow_all, bufferlist caps.
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Cursor, &StructVersion);
  }

  if (!EFI_ERROR (Status) && (StructVersion != 1)) {
    Status = EFI_UNSUPPORTED;
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Cursor, &AllowAll);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &Value32);
  }

  CapsLength = Value32;
  if (!EFI_ERROR (Status) && (CapsLength > CEPHX_MAX_CAPS_BYTES)) {
    Status = EFI_BAD_BUFFER_SIZE;
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireSkip (&Cursor, CapsLength);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &Info->Flags);
  }

  // CryptoKey: type (u16), created utime_t, u16 secret length, raw secret.
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe16 (&Cursor, &Info->SessionKey.Type);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor, &Info->SessionKey.CreatedSeconds);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe32 (&Cursor,
                              &Info->SessionKey.CreatedNanoseconds);
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe16 (&Cursor, &Info->SessionKey.KeyLength);
  }

  if (!EFI_ERROR (Status)) {
    if (Info->SessionKey.KeyLength != CEPHX_AES_KEY_SIZE) {
      Status = EFI_UNSUPPORTED;
    } else {
      Status = CephWireReadBytes (
                 &Cursor,
                 Info->SessionKey.Key,
                 Info->SessionKey.KeyLength
                 );
    }
  }

  if (!EFI_ERROR (Status) && (Cursor.Remaining != 0)) {
    Status = EFI_COMPROMISED_DATA;
  }

  Info->AllowAll = AllowAll;
  Info->SecretId = Ticket->SecretId;

  // Fail closed on tampered or unsupported ticket contents.
  if (!EFI_ERROR (Status)) {
    if (Info->Auid != CEPHX_AUTH_UID_DEFAULT) {
      Status = EFI_ACCESS_DENIED;
    } else if (AllowAll != 0) {
      Status = EFI_ACCESS_DENIED;
    } else if ((Info->SessionKey.Type != CEPHX_CRYPTO_AES) ||
               (Info->SessionKey.KeyLength != CEPHX_AES_KEY_SIZE))
    {
      Status = EFI_UNSUPPORTED;
    } else if ((Info->CreatedNanoseconds > 1000000000U) ||
               (Info->ExpiresNanoseconds > 1000000000U) ||
               (Info->SessionKey.CreatedNanoseconds > 1000000000U))
    {
      Status = EFI_COMPROMISED_DATA;
    } else if ((Info->ExpiresSeconds < Info->CreatedSeconds) ||
               ((Info->ExpiresSeconds == Info->CreatedSeconds) &&
                (Info->ExpiresNanoseconds < Info->CreatedNanoseconds)))
    {
      Status = EFI_COMPROMISED_DATA;
    } else if ((ExpectedGlobalId != 0) &&
               (Info->GlobalId != ExpectedGlobalId))
    {
      Status = EFI_ACCESS_DENIED;
    }
  }

  // Ceph tolerates a 300s clock skew; reject already-expired tickets.
  if (!EFI_ERROR (Status) && (NowSeconds != 0)) {
    Value64 = (Info->ExpiresSeconds > CEPHX_TICKET_VALIDITY_SLOP_SECONDS) ?
              Info->ExpiresSeconds - CEPHX_TICKET_VALIDITY_SLOP_SECONDS : 0;
    if (NowSeconds > Value64) {
      Status = EFI_ACCESS_DENIED;
    }
  }

  ZeroMem (Plaintext, sizeof (Plaintext));
  return Status;
}

EFI_STATUS
CephXVerifyAuthorizeReply (
  IN CONST UINT8  *SessionKey,
  IN UINTN        SessionKeyLength,
  IN CONST VOID   *Buffer,
  IN UINTN        BufferLength,
  IN UINT64       ClientNonce,
  OUT VOID        *ConnectionSecret,  OPTIONAL
  IN UINTN        ConnectionSecretCapacity,
  OUT UINTN       *ConnectionSecretLength  OPTIONAL
  )
{
  EFI_STATUS        Status;
  UINT8             Plaintext[CEPHX_MAX_ENCRYPTED_PAYLOAD];
  UINTN             PlaintextLength;
  CEPH_WIRE_CURSOR  Cursor;
  UINT8             StructVersion;
  UINT64            NoncePlusOne;
  UINT32            SecretLength;

  if ((SessionKey == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((ConnectionSecretLength != NULL) && (ConnectionSecret == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (SessionKeyLength != CEPHX_AES_KEY_SIZE) {
    return EFI_UNSUPPORTED;
  }

  if (ConnectionSecretLength != NULL) {
    *ConnectionSecretLength = 0;
  }

  Status = CephXDecryptAes128Payload (
             SessionKey,
             SessionKeyLength,
             Buffer,
             BufferLength,
             Plaintext,
             sizeof (Plaintext),
             &PlaintextLength
             );
  if (EFI_ERROR (Status)) {
    ZeroMem (Plaintext, sizeof (Plaintext));
    return Status;
  }

  Status = CephWireInit (&Cursor, Plaintext, PlaintextLength);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Cursor, &StructVersion);
  }

  if (!EFI_ERROR (Status) &&
      ((StructVersion != 1) && (StructVersion != 2)))
  {
    Status = EFI_UNSUPPORTED;
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe64 (&Cursor, &NoncePlusOne);
  }

  if (!EFI_ERROR (Status) &&
      ((ClientNonce == MAX_UINT64) || (NoncePlusOne != ClientNonce + 1)))
  {
    Status = EFI_COMPROMISED_DATA;
  }

  if (!EFI_ERROR (Status) && (StructVersion >= 2)) {
    Status = CephWireReadLe32 (&Cursor, &SecretLength);
    if (!EFI_ERROR (Status)) {
      if (SecretLength > Cursor.Remaining) {
        Status = EFI_BAD_BUFFER_SIZE;
      } else if ((SecretLength != 0) &&
                 ((ConnectionSecret == NULL) ||
                  (SecretLength > ConnectionSecretCapacity)))
      {
        Status = EFI_BUFFER_TOO_SMALL;
      } else if (SecretLength != 0) {
        Status = CephWireReadBytes (&Cursor, ConnectionSecret, SecretLength);
        if (!EFI_ERROR (Status) && (ConnectionSecretLength != NULL)) {
          *ConnectionSecretLength = SecretLength;
        }
      }
    }
  }

  if (!EFI_ERROR (Status) && (Cursor.Remaining != 0)) {
    Status = EFI_COMPROMISED_DATA;
  }

  ZeroMem (Plaintext, sizeof (Plaintext));
  return Status;
}

EFI_STATUS
CephXDecodeAuthorizeChallengeSecret (
  IN CONST UINT8  *SessionKey,
  IN UINTN        SessionKeyLength,
  IN CONST VOID   *Buffer,
  IN UINTN        BufferLength,
  OUT UINT64      *ServerChallenge
  )
{
  EFI_STATUS        Status;
  UINT8             Plaintext[CEPHX_MAX_ENCRYPTED_PAYLOAD];
  UINTN             PlaintextLength;
  CEPH_WIRE_CURSOR  Cursor;
  UINT8             StructVersion;

  if ((SessionKey == NULL) || (Buffer == NULL) ||
      (ServerChallenge == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (SessionKeyLength != CEPHX_AES_KEY_SIZE) {
    return EFI_UNSUPPORTED;
  }

  Status = CephXDecryptAes128Payload (
             SessionKey,
             SessionKeyLength,
             Buffer,
             BufferLength,
             Plaintext,
             sizeof (Plaintext),
             &PlaintextLength
             );
  if (EFI_ERROR (Status)) {
    ZeroMem (Plaintext, sizeof (Plaintext));
    return Status;
  }

  Status = CephWireInit (&Cursor, Plaintext, PlaintextLength);
  if (!EFI_ERROR (Status)) {
    Status = CephWireReadU8 (&Cursor, &StructVersion);
  }

  if (!EFI_ERROR (Status) && (StructVersion != 1)) {
    Status = EFI_UNSUPPORTED;
  }

  if (!EFI_ERROR (Status)) {
    Status = CephWireReadLe64 (&Cursor, ServerChallenge);
  }

  if (!EFI_ERROR (Status) && (Cursor.Remaining != 0)) {
    Status = EFI_COMPROMISED_DATA;
  }

  ZeroMem (Plaintext, sizeof (Plaintext));
  return Status;
}
