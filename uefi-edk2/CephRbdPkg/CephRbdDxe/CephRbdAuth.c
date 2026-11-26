#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "CephRbdAuth.h"
#include "CephRbdRpc.h"
#include "CephRbdWire.h"
#include "CephRbdTcp4.h"
#include "CephRbdMsgrV1Handshake.h"
#include "CephRbdMsgrV1Session.h"

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

EFI_STATUS
CephRbdAuthEncodeServiceTicketRequest (
  IN  UINT64                GlobalId,
  IN  CONST CEPH_RBD_TICKET  *AuthTicket,
  IN  CONST UINT8           *SessionKey,
  IN  UINT64                Nonce,
  IN  UINT32                ServiceMask,
  OUT VOID                  *Buffer,
  IN  UINTN                 BufferCapacity,
  OUT UINTN                 *EncodedLength
  )
{
  EFI_STATUS        Status;
  CEPH_WIRE_WRITER  Writer;
  UINT8             Authorizer[256];
  UINTN             AuthorizerLength;

  if ((AuthTicket == NULL) || (SessionKey == NULL) ||
      (Buffer == NULL) || (EncodedLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (!AuthTicket->Valid || (AuthTicket->BlobLength == 0) ||
      (AuthTicket->BlobLength > CEPHX_MAX_TICKET_BLOB))
  {
    return EFI_INVALID_PARAMETER;
  }

  *EncodedLength = 0;

  // CephXAuthorizer for the AUTH service, built from the AUTH ticket.
  Status = CephXEncodeServiceAuthorizer (
             GlobalId,
             CEPH_ENTITY_TYPE_AUTH_SERVICE,
             AuthTicket->SecretId,
             AuthTicket->Blob,
             AuthTicket->BlobLength,
             SessionKey,
             CEPHX_AES_KEY_SIZE,
             Nonce,
             FALSE,
             0,
             Authorizer,
             sizeof (Authorizer),
             &AuthorizerLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // MAuth front payload.
  Status = CephWireWriterInit (&Writer, Buffer, BufferCapacity);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // paxos_encode(): version, deprecated_session_mon (s16 = -1), tid.
  Status = CephWireWriteLe64 (&Writer, 0);
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe16 (&Writer, (UINT16)0xFFFF);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe64 (&Writer, 0);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, CEPH_AUTH_CEPHX);
  }

  // auth_payload = CephXRequestHeader + CephXAuthorizer +
  //                CephXServiceTicketRequest.
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (
               &Writer,
               (UINT32)(2 + AuthorizerLength + 5)
               );
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe16 (&Writer, CEPHX_GET_PRINCIPAL_SESSION_KEY);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteBytes (&Writer, Authorizer, AuthorizerLength);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteU8 (&Writer, 1);   // CephXServiceTicketRequest v
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, ServiceMask);
  }
  if (!EFI_ERROR (Status)) {
    Status = CephWireWriteLe32 (&Writer, 0);  // monmap_epoch
  }

  ZeroMem (Authorizer, sizeof (Authorizer));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *EncodedLength = Writer.Written;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdAuthGetServiceTicket (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT        *Client,
  IN  CONST CEPH_RBD_AUTH_SESSION       *Session,
  IN  UINT32                            ServiceId,
  OUT CEPH_RBD_SERVICE_AUTH             *ServiceAuth
  )
{
  EFI_STATUS           Status;
  UINT8                Request[CEPH_RBD_AUTH_MAX_PAYLOAD];
  UINT8                Reply[CEPH_RBD_AUTH_MAX_REPLY];
  UINTN                RequestLength;
  UINTN                ReplyLength;
  CEPH_RBD_AUTH_REPLY  AuthReply;
  UINT64               Nonce;

  if ((Client == NULL) || (Session == NULL) || (ServiceAuth == NULL) ||
      !Session->HasSessionKey || !Session->AuthTicket.Valid)
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (ServiceAuth, sizeof (*ServiceAuth));

  // A fixed nonce is acceptable: the authorizer is already bound to the
  // ticket and session key, and messenger v1 has no replay window.
  Nonce = 0x43455048524c444BU;  // "CEPHRLD" + 0xB

  Status = CephRbdAuthEncodeServiceTicketRequest (
             Session->GlobalId,
             &Session->AuthTicket,
             Session->SessionKey,
             Nonce,
             ServiceId,
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

  Status = CephXDecodeServiceTicket (
             AuthReply.ResultBl,
             AuthReply.ResultBlLength,
             Session->SessionKey,
             CEPHX_AES_KEY_SIZE,
             ServiceId,
             ServiceAuth->SessionKey,
             sizeof (ServiceAuth->SessionKey),
             &ServiceAuth->Ticket.SecretId,
             ServiceAuth->Ticket.Blob,
             sizeof (ServiceAuth->Ticket.Blob),
             &ServiceAuth->Ticket.BlobLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ServiceAuth->GlobalId = Session->GlobalId;
  ServiceAuth->ServiceId = ServiceId;
  ServiceAuth->Ticket.Valid = TRUE;
  ServiceAuth->Valid = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdAuthBuildServiceAuthorizer (
  IN  CONST CEPH_RBD_SERVICE_AUTH  *ServiceAuth,
  IN  UINT64                       Nonce,
  OUT VOID                         *Buffer,
  IN  UINTN                        BufferCapacity,
  OUT UINTN                        *EncodedLength
  )
{
  if ((ServiceAuth == NULL) || !ServiceAuth->Valid ||
      (Buffer == NULL) || (EncodedLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  return CephXEncodeServiceAuthorizer (
           ServiceAuth->GlobalId,
           ServiceAuth->ServiceId,
           ServiceAuth->Ticket.SecretId,
           ServiceAuth->Ticket.Blob,
           ServiceAuth->Ticket.BlobLength,
           ServiceAuth->SessionKey,
           CEPHX_AES_KEY_SIZE,
           Nonce,
           FALSE,
           0,
           Buffer,
           BufferCapacity,
           EncodedLength
           );
}

EFI_STATUS
CephRbdAuthConnectOsd (
  IN  CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG  *Config,
  IN  CONST CEPH_RBD_SERVICE_AUTH           *OsdAuth,
  OUT CEPH_RBD_MSGR_V1_CLIENT               *Client
  )
{
  EFI_STATUS        Status;
  UINT8             Authorizer[512];
  UINTN             AuthorizerLength;
  UINT64            ClientNonce;
  UINT64            ServerChallenge;
  CEPH_MSGR_V1_NEGOTIATION  Negotiation;

  if ((Config == NULL) || (OsdAuth == NULL) || (Client == NULL) ||
      !OsdAuth->Valid || (Config->RemotePort == 0) ||
      (Config->TimeoutSeconds == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Client, sizeof (*Client));
  CephMsgrV1RetransmitQueueInit (&Client->Retransmit, 0);
  Client->GlobalSequence = 1;
  // A fresh messenger-v1 client starts at connect_seq 0 (ProtocolV1.h).
  // Using 1 here makes the OSD treat the challenge round-trip as a reconnect
  // against its not-yet-opened session (connect_seq 0) and RESET the session.
  Client->ConnectSequence = 0;
  Client->ClientNonce = Config->ClientNonce;
  Client->RecoveryConfig = Config;

  // Open the TCP connection to the OSD messenger-v1 endpoint.
  Status = CephRbdTcp4Connect (
             Config->ImageHandle,
             Config->ControllerHandle,
             &Config->RemoteAddress,
             Config->RemotePort,
             Config->TimeoutSeconds,
             &Client->Connection
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Client->Connected = TRUE;

  Status = CephMsgrV1ExchangeBanners (
             &Client->Connection,
             &Config->RemoteAddress,
             Config->RemotePort,
             Config->TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  // Fixed nonce: the authorizer is bound to the ticket and session key, and
  // messenger v1 does not enforce a challenge replay window.
  ClientNonce = 0x4f5344415554484BU;  // "OSDAUTHK" + 0xB

  Status = CephRbdAuthBuildServiceAuthorizer (
             OsdAuth,
             ClientNonce,
             Authorizer,
             sizeof (Authorizer),
             &AuthorizerLength
             );
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  // First connect carries the base authorizer (no challenge).
  Status = CephMsgrV1SendConnectAndReadReply (
             &Client->Connection,
             Config->SupportedFeatures,
             Config->HostType,
             Client->GlobalSequence,
             Client->ConnectSequence,
             Config->ProtocolVersion,
             CEPH_AUTH_CEPHX,
             Authorizer,
             (UINT32)AuthorizerLength,
             Config->Flags,
             Config->RequiredFeatures,
             Config->TimeoutSeconds,
             &Client->Negotiation
             );
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  // The OSD answers the first authorizer with a CHALLENGE_AUTHORIZER reply.
  if (Client->Negotiation.Action == CephMsgrConnectNeedAuthorizerChallenge) {
    if ((Client->Negotiation.AuthorizerReplyLength == 0) ||
        (Client->Negotiation.AuthorizerReplyLength >
         sizeof (Client->Negotiation.AuthorizerReply)))
    {
      Status = EFI_BAD_BUFFER_SIZE;
      goto Fail;
    }

    Status = CephXDecodeAuthorizeChallengeSecret (
               OsdAuth->SessionKey,
               CEPHX_AES_KEY_SIZE,
               Client->Negotiation.AuthorizerReply,
               Client->Negotiation.AuthorizerReplyLength,
               &ServerChallenge
               );
    if (EFI_ERROR (Status)) {
      goto Fail;
    }

    // Rebuild the authorizer with have_challenge=1 and challenge+1.
    ZeroMem (Authorizer, sizeof (Authorizer));
    Status = CephXEncodeServiceAuthorizer (
               OsdAuth->GlobalId,
               OsdAuth->ServiceId,
               OsdAuth->Ticket.SecretId,
               OsdAuth->Ticket.Blob,
               OsdAuth->Ticket.BlobLength,
               OsdAuth->SessionKey,
               CEPHX_AES_KEY_SIZE,
               ClientNonce,
               TRUE,
               ServerChallenge + 1,
               Authorizer,
               sizeof (Authorizer),
               &AuthorizerLength
               );
    if (EFI_ERROR (Status)) {
      goto Fail;
    }

    Status = CephMsgrV1SendConnectAndReadReply (
               &Client->Connection,
               Config->SupportedFeatures,
               Config->HostType,
               Client->GlobalSequence,
               Client->ConnectSequence,
               Config->ProtocolVersion,
               CEPH_AUTH_CEPHX,
               Authorizer,
               (UINT32)AuthorizerLength,
               Config->Flags,
               Config->RequiredFeatures,
               Config->TimeoutSeconds,
               &Negotiation
               );
    if (EFI_ERROR (Status)) {
      goto Fail;
    }
    CopyMem (&Client->Negotiation, &Negotiation, sizeof (Negotiation));
  }

  if ((Client->Negotiation.Action != CephMsgrConnectReady) &&
      (Client->Negotiation.Action != CephMsgrConnectAckSequence)) {
    Status = EFI_ACCESS_DENIED;
    goto Fail;
  }

  // Verify the final authorize reply (nonce+1 encrypted with the session key)
  // before completing the SEQ exchange. The OSD embeds this in the READY/SEQ
  // connect reply, not in a separate message.
  if (Client->Negotiation.AuthorizerReplyLength != 0) {
    Status = CephXVerifyAuthorizeReply (
               OsdAuth->SessionKey,
               CEPHX_AES_KEY_SIZE,
               Client->Negotiation.AuthorizerReply,
               Client->Negotiation.AuthorizerReplyLength,
               ClientNonce,
               Client->ConnectionSecret,
               sizeof (Client->ConnectionSecret),
               &Client->ConnectionSecretLength
               );
    if (EFI_ERROR (Status)) {
      goto Fail;
    }
  }

  if (Client->Negotiation.Action == CephMsgrConnectAckSequence) {
    // CEPH_MSGR_TAG_SEQ: the OSD is ready but expects the connect-time
    // sequence exchange before any frames are sent. FinishSeqRecovery also
    // initializes the session, so do not call CephMsgrV1SessionInit here.
    Status = CephRbdMsgrV1FinishSeqRecovery (Config, Client);
  } else {
    Status = CephMsgrV1SessionInit (
               &Client->Session,
               &Client->Connection,
               &Client->Negotiation,
               CEPH_MSGR_V1_SESSION_FRAME_MAX,
               Config->TimeoutSeconds
               );
  }
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  ZeroMem (Authorizer, sizeof (Authorizer));
  return EFI_SUCCESS;

Fail:
  ZeroMem (Authorizer, sizeof (Authorizer));
  CephRbdTcp4Close (&Client->Connection, Config->TimeoutSeconds);
  Client->Connected = FALSE;
  Client->Session.Ready = FALSE;
  return Status;
}
