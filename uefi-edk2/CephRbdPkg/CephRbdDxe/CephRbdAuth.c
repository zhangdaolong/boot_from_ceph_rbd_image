#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "CephRbdAuth.h"
#include "CephRbdRpc.h"
#include "CephRbdWire.h"

// Encode a Ceph bufferlist length prefix: u32 byte count followed by raw bytes.
// Ceph's encode(bufferlist) uses exactly this layout.
STATIC
EFI_STATUS
CephAuthWriteBufferlist (
  IN OUT CEPH_WIRE_WRITER  *Writer,
  IN CONST VOID            *Data,
  IN UINTN                  Length
  )
{
  EFI_STATUS  Status;

  if (Length > MAX_UINT32) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = CephWireWriteLe32 (Writer, (UINT32)Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Length != 0) {
    return CephWireWriteBytes (Writer, Data, Length);
  }

  return EFI_SUCCESS;
}

// Encode a Ceph std::string: u32 length + bytes (no NUL terminator).
STATIC
EFI_STATUS
CephAuthWriteString (
  IN OUT CEPH_WIRE_WRITER  *Writer,
  IN CONST CHAR8           *Text,
  IN UINTN                  Length
  )
{
  return CephAuthWriteBufferlist (Writer, Text, Length);
}

EFI_STATUS
CephRbdAuthEncodeInitialRequest (
  IN CONST CHAR8  *EntityId,
  IN UINTN         EntityIdLength,
  OUT VOID        *Buffer,
  IN UINTN         BufferLength,
  OUT UINTN       *EncodedLength
  )
{
  CEPH_WIRE_WRITER  Writer;
  EFI_STATUS        Status;
  UINT8             AuthPayload[CEPH_RBD_AUTH_MAX_PAYLOAD];
  CEPH_WIRE_WRITER  Payload;
  UINTN             PayloadLength;

  if ((EntityId == NULL) || (EntityIdLength == 0) ||
      (EntityIdLength > CEPH_RBD_AUTH_MAX_IDENTITY) ||
      (Buffer == NULL) || (EncodedLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *EncodedLength = 0;

  // Build auth_payload: struct_v=1, supported={CEPHX}, EntityName, global_id.
  Status = CephWireWriterInit (&Payload, AuthPayload, sizeof (AuthPayload));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // u8 struct_v = 1
  Status = CephWireWriteU8 (&Payload, 1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // std::vector<u32> supported methods = { CEPH_AUTH_CEPHX }.
  Status = CephWireWriteLe32 (&Payload, 1);
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Payload, CEPH_AUTH_CEPHX);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // EntityName: u32 type (CLIENT), string id.
  Status = CephWireWriteLe32 (&Payload, CEPH_ENTITY_TYPE_CLIENT);
  if (!EFI_ERROR (Status)) {
    Status = CephAuthWriteString (&Payload, EntityId, EntityIdLength);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // u64 global_id = 0 (assigned by the monitor).
  Status = CephWireWriteLe64 (&Payload, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  PayloadLength = Payload.Written;

  // Build the MAuth front payload: paxos_encode() prefix, protocol,
  // auth_payload bufferlist, monmap_epoch.
  Status = CephWireWriterInit (&Writer, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // paxos_encode(): version (u64), deprecated_session_mon (s16 = -1),
  // deprecated_session_mon_tid (u64).
  Status = CephWireWriteLe64 (&Writer, 0);
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe16 (&Writer, (UINT16)0xFFFF);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, 0);
  }

  // protocol = CEPH_AUTH_UNKNOWN.
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, CEPH_AUTH_UNKNOWN);
  }

  // auth_payload bufferlist.
  if (!EFI_ERROR (Status)) {
    Status = CephAuthWriteBufferlist (&Writer, AuthPayload, PayloadLength);
  }

  // monmap_epoch = 0 (we accept whatever the monitor sends).
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdAuthDecodeReply (
  IN CONST VOID            *Buffer,
  IN UINTN                 BufferLength,
  OUT CEPH_RBD_AUTH_REPLY  *Reply
  )
{
  CEPH_WIRE_CURSOR  Cursor;
  EFI_STATUS        Status;
  UINT32            Value32;

  if ((Buffer == NULL) || (Reply == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Reply, sizeof (*Reply));
  Status = CephWireInit (&Cursor, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // protocol (u32).
  Status = CephWireReadLe32 (&Cursor, &Reply->Protocol);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // result (errorcode32_t = s32, encoded as its 32-bit pattern).
  Status = CephWireReadLe32 (&Cursor, &Value32);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Reply->Result = (INT32)Value32;

  // global_id (u64).
  Status = CephWireReadLe64 (&Cursor, &Reply->GlobalId);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // result_bl bufferlist.
  Status = CephWireReadLe32 (&Cursor, &Value32);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Value32 > Cursor.Remaining) {
    return EFI_BAD_BUFFER_SIZE;
  }
  Reply->ResultBlLength = Value32;
  Reply->ResultBl = (Value32 != 0) ? Cursor.Data : NULL;
  Status = CephWireSkip (&Cursor, Value32);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // result_msg string.
  Status = CephWireReadLe32 (&Cursor, &Value32);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Value32 > Cursor.Remaining) {
    return EFI_BAD_BUFFER_SIZE;
  }
  Reply->ResultMsgLength = Value32;
  Reply->ResultMsg = (Value32 != 0) ? Cursor.Data : NULL;
  Status = CephWireSkip (&Cursor, Value32);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return (Cursor.Remaining == 0) ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}

EFI_STATUS
CephRbdAuthEncodeAuthenticateRequest (
  IN UINT64         ClientChallenge,
  IN UINT64         ChallengeResponse,
  IN UINT32         NeedKeys,
  OUT VOID         *Buffer,
  IN UINTN          BufferLength,
  OUT UINTN        *EncodedLength
  )
{
  CEPH_WIRE_WRITER  Writer;
  EFI_STATUS        Status;
  UINT8             AuthPayload[CEPH_RBD_AUTH_MAX_PAYLOAD];
  UINTN             PayloadLength;

  if ((Buffer == NULL) || (EncodedLength == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *EncodedLength = 0;

  // auth_payload = CephXRequestHeader(u16 request_type) + CephXAuthenticate.
  Status = CephXEncodeRequestHeader (
             CEPHX_GET_AUTH_SESSION_KEY,
             AuthPayload,
             sizeof (AuthPayload),
             &PayloadLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Append CephXAuthenticate after the request header.
  {
    UINT8  Authenticate[CEPH_RBD_AUTH_MAX_PAYLOAD];
    UINTN  AuthenticateLength;

    if (PayloadLength > sizeof (AuthPayload)) {
      return EFI_BAD_BUFFER_SIZE;
    }

    Status = CephXEncodeAuthenticate (
               ClientChallenge,
               ChallengeResponse,
               0,              // OldTicketSecretId
               NULL,           // OldTicketBlob
               0,              // OldTicketBlobLength
               NeedKeys,
               Authenticate,
               sizeof (Authenticate),
               &AuthenticateLength
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (AuthenticateLength > sizeof (AuthPayload) - PayloadLength) {
      return EFI_BAD_BUFFER_SIZE;
    }

    CopyMem (AuthPayload + PayloadLength, Authenticate, AuthenticateLength);
    PayloadLength += AuthenticateLength;
  }

  // MAuth front payload.
  Status = CephWireWriterInit (&Writer, Buffer, BufferLength);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephWireWriteLe64 (&Writer, 0);                    // version
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe16 (&Writer, (UINT16)0xFFFF);     // session_mon
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, 0);                  // session_mon_tid
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, CEPH_AUTH_CEPHX);    // protocol
  }
  if (!EFI_ERROR (Status)) {
    Status = CephAuthWriteBufferlist (&Writer, AuthPayload, PayloadLength);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);                  // monmap_epoch
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdAuthHandshake (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN CONST CHAR8                 *EntityId,
  IN UINTN                        EntityIdLength,
  IN CONST UINT8                 *Secret,
  IN UINTN                        SecretLength,
  OUT CEPH_RBD_AUTH_SESSION       *Session
  )
{
  EFI_STATUS           Status;
  UINT8                Request[CEPH_RBD_AUTH_MAX_PAYLOAD];
  UINT8                Reply[CEPH_RBD_AUTH_MAX_REPLY];
  UINTN                RequestLength;
  UINTN                ReplyLength;
  CEPH_RBD_AUTH_REPLY  AuthReply;
  CEPHX_SERVER_CHALLENGE  Challenge;
  UINT64               ClientChallenge;
  UINT64               ChallengeResponse;

  if ((Client == NULL) || (EntityId == NULL) || (EntityIdLength == 0) ||
      (Secret == NULL) || (Session == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (SecretLength != CEPHX_AES_KEY_SIZE) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (Session, sizeof (*Session));

  // Step 1: send MAuth with protocol=UNKNOWN and our supported methods.
  Status = CephRbdAuthEncodeInitialRequest (
             EntityId,
             EntityIdLength,
             Request,
             sizeof (Request),
             &RequestLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRpcCall (
             Client,
             CEPH_MSG_AUTH,
             CEPH_MSG_AUTH_REPLY,
             Request,
             RequestLength,
             NULL,
             0,
             Reply,
             sizeof (Reply),
             &ReplyLength,
             NULL,
             0,
             NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdAuthDecodeReply (Reply, ReplyLength, &AuthReply);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (AuthReply.Result != 0) {
    return EFI_ACCESS_DENIED;
  }

  // Step 2: decode the CephX server challenge from result_bl.
  Status = CephXDecodeServerChallenge (
             AuthReply.ResultBl,
             AuthReply.ResultBlLength,
             &Challenge
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Use a deterministic, nonzero client challenge. A firmware client has no
  // CSPRNG; the challenge is a nonce, and replay protection comes from the
  // server challenge, so a per-session fixed value is acceptable here.
  ClientChallenge = 0x434550485242444BU;  // "CEPHRBD" + 0xB

  Status = CephXComputeAesChallengeResponse (
             Secret,
             SecretLength,
             Challenge.ServerChallenge,
             ClientChallenge,
             &ChallengeResponse
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Step 3: send MAuth(CEPHX) with CephXRequestHeader + CephXAuthenticate.
  Status = CephRbdAuthEncodeAuthenticateRequest (
             ClientChallenge,
             ChallengeResponse,
             CEPH_RBD_AUTH_NEED_KEYS,
             Request,
             sizeof (Request),
             &RequestLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRpcCall (
             Client,
             CEPH_MSG_AUTH,
             CEPH_MSG_AUTH_REPLY,
             Request,
             RequestLength,
             NULL,
             0,
             Reply,
             sizeof (Reply),
             &ReplyLength,
             NULL,
             0,
             NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdAuthDecodeReply (Reply, ReplyLength, &AuthReply);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // A zero result means CephxServiceHandler::handle_request returned 1 and the
  // monitor called ms_handle_fast_authentication(), marking the session
  // authenticated. All subsequent MON/OSD RPCs are now accepted.
  if (AuthReply.Result != 0) {
    return EFI_ACCESS_DENIED;
  }

  Session->GlobalId = AuthReply.GlobalId;
  Session->ServerChallenge = Challenge.ServerChallenge;
  Session->ClientChallenge = ClientChallenge;

  // The AUTH ticket session key is carried in the reply's result_bl after
  // the CephXResponseHeader, encrypted with our own secret. The same reply
  // carries an AUTH CephXTicketBlob that must be presented as old_ticket when
  // requesting OSD/MON service tickets. Messenger v1 does not sign messages,
  // so a failure to parse either does not invalidate the authenticated
  // session; both are retained for subsequent service-ticket exchanges.
  Status = CephXDecodeAuthSessionKey (
             AuthReply.ResultBl,
             AuthReply.ResultBlLength,
             Secret,
             SecretLength,
             Session->SessionKey,
             sizeof (Session->SessionKey),
             &Session->AuthTicket.SecretId,
             Session->AuthTicket.Blob,
             sizeof (Session->AuthTicket.Blob),
             &Session->AuthTicket.BlobLength
             );
  Session->HasSessionKey = !EFI_ERROR (Status);
  Session->AuthTicket.Valid = !EFI_ERROR (Status);

  return EFI_SUCCESS;
}
