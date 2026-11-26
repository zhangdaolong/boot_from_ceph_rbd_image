#ifndef CEPH_RBD_AUTH_H_
#define CEPH_RBD_AUTH_H_

#include <Uefi.h>
#include "CephRbdMessenger.h"
#include "CephRbdMsgrV1Recovery.h"
#include "CephRbdCephX.h"

// Message types from include/ceph_fs.h.
#define CEPH_MSG_AUTH                17
#define CEPH_MSG_AUTH_REPLY          18

// Auth protocol constants from include/ceph_fs.h.
#define CEPH_AUTH_UNKNOWN            0x0
#define CEPH_AUTH_NONE               0x1
#define CEPH_AUTH_CEPHX              0x2

// Messenger-v1 connect protocol versions from include/ceph_fs.h. The monitor
// and OSD use DIFFERENT versions; sending the wrong one is rejected by the
// peer with CEPH_MSGR_TAG_BADPROTOVER before authorizer verification.
#define CEPH_MONC_PROTOCOL           15
#define CEPH_OSDC_PROTOCOL           24

// CephX request types from auth/cephx/CephxProtocol.h (CephXRequestHeader).
#define CEPHX_GET_AUTH_SESSION_KEY       0x0100
#define CEPHX_GET_PRINCIPAL_SESSION_KEY  0x0200

// Entity type for the AUTH service; used as the initial CephX service id.
#define CEPH_ENTITY_TYPE_AUTH_SERVICE   0x20

// need mask: request the AUTH session key plus MON and OSD service tickets.
// CEPH_ENTITY_TYPE_AUTH(0x20) | MON(0x01) | OSD(0x04).
#define CEPH_RBD_AUTH_NEED_KEYS  \
  (CEPH_ENTITY_TYPE_AUTH_SERVICE | CEPH_ENTITY_TYPE_MON | CEPH_ENTITY_TYPE_OSD)

// Maximum sizes for auth wire buffers.
#define CEPH_RBD_AUTH_MAX_IDENTITY   CEPHX_MAX_ENTITY_NAME
#define CEPH_RBD_AUTH_MAX_REPLY      2048
#define CEPH_RBD_AUTH_MAX_PAYLOAD    1024

// A CephXTicketBlob retained from the monitor handshake. Blob bytes are a
// copy of the encrypted CephXServiceTicketInfo; SecretId selects the service
// secret used to decrypt it on the service side.
typedef struct {
  UINT64  SecretId;
  UINT8   Blob[CEPHX_MAX_TICKET_BLOB];
  UINTN   BlobLength;
  BOOLEAN Valid;
} CEPH_RBD_TICKET;

// The complete result of a successful monitor CephX handshake.
typedef struct {
  UINT64  GlobalId;        // Assigned global id from MAuthReply.
  UINT64  ServerChallenge; // Challenge used in the final authenticate step.
  UINT64  ClientChallenge; // Our nonce.
  UINT8   SessionKey[CEPHX_MAX_KEY_SIZE]; // AUTH ticket session key.
  UINTN   SessionKeyLength;               // 16 (AES-128) or 32 (AES-256-KRB5).
  BOOLEAN HasSessionKey;
  CEPH_RBD_TICKET  AuthTicket;  // AUTH ticket for building service authorizers.
} CEPH_RBD_AUTH_SESSION;

// Encode the first MAuth message front payload. protocol=CEPH_AUTH_UNKNOWN,
// auth_payload = { u8 struct_v=1, vector<u32> supported={CEPHX},
//                  EntityName(u32 type, string id), u64 global_id=0 }.
EFI_STATUS
CephRbdAuthEncodeInitialRequest (
  IN CONST CHAR8  *EntityId,
  IN UINTN         EntityIdLength,
  OUT VOID        *Buffer,
  IN UINTN         BufferLength,
  OUT UINTN       *EncodedLength
  );

// Decode an MAuthReply front payload into its public fields. The result_bl and
// result_msg byte ranges are returned as pointers into Buffer (valid only while
// Buffer is alive).
typedef struct {
  UINT32        Protocol;
  INT32         Result;
  UINT64        GlobalId;
  CONST UINT8  *ResultBl;
  UINTN         ResultBlLength;
  CONST UINT8  *ResultMsg;
  UINTN         ResultMsgLength;
} CEPH_RBD_AUTH_REPLY;

EFI_STATUS
CephRbdAuthDecodeReply (
  IN CONST VOID            *Buffer,
  IN UINTN                 BufferLength,
  OUT CEPH_RBD_AUTH_REPLY  *Reply
  );

// Encode the second MAuth message front payload (protocol=CEPH_AUTH_CEPHX).
// auth_payload = CephXRequestHeader(GET_AUTH_SESSION_KEY) + CephXAuthenticate.
EFI_STATUS
CephRbdAuthEncodeAuthenticateRequest (
  IN UINT64         ClientChallenge,
  IN UINT64         ChallengeResponse,
  IN UINT32         NeedKeys,
  OUT VOID         *Buffer,
  IN UINTN          BufferLength,
  OUT UINTN        *EncodedLength
  );

// Run the complete CephX challenge-response handshake over an established
// messenger v1 client: send MAuth(UNKNOWN), read challenge, send
// MAuth(CEPHX, authenticate), read final reply and validate the AUTH ticket.
// On success Session is filled and the monitor session is authenticated, so
// subsequent MON/OSD RPCs are accepted.
EFI_STATUS
CephRbdAuthHandshake (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN CONST CHAR8                 *EntityId,
  IN UINTN                        EntityIdLength,
  IN CONST UINT8                 *Secret,
  IN UINTN                        SecretLength,
  OUT CEPH_RBD_AUTH_SESSION       *Session
  );

// The result of a GET_PRINCIPAL_SESSION_KEY exchange for one service. Ticket
// is the service CephXTicketBlob (opaque, encrypted with the service secret),
// SessionKey is the service session key recovered with the AUTH session key.
typedef struct {
  CEPH_RBD_TICKET  Ticket;
  UINT8            SessionKey[CEPHX_MAX_KEY_SIZE];
  UINTN            SessionKeyLength;
  UINT64           GlobalId;
  UINT32           ServiceId;
  BOOLEAN          Valid;
} CEPH_RBD_SERVICE_AUTH;

// Encode the GET_PRINCIPAL_SESSION_KEY MAuth front payload. The auth_payload
// is CephXRequestHeader(GET_PRINCIPAL_SESSION_KEY) + CephXAuthorizer(built
// from the AUTH ticket) + CephXServiceTicketRequest(ServiceId mask).
EFI_STATUS
CephRbdAuthEncodeServiceTicketRequest (
  IN  UINT64               GlobalId,
  IN  CONST CEPH_RBD_TICKET  *AuthTicket,
  IN  CONST UINT8          *SessionKey,
  IN  UINTN                SessionKeyLength,
  IN  UINT64               Nonce,
  IN  UINT32               ServiceMask,
  OUT VOID                 *Buffer,
  IN  UINTN                BufferCapacity,
  OUT UINTN                *EncodedLength
  );

// Run a GET_PRINCIPAL_SESSION_KEY exchange over the already-authenticated
// monitor connection. ServiceAuth receives the service ticket and session
// key for ServiceId (e.g. CEPH_ENTITY_TYPE_OSD).
EFI_STATUS
CephRbdAuthGetServiceTicket (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT        *Client,
  IN  CONST CEPH_RBD_AUTH_SESSION       *Session,
  IN  UINT32                            ServiceId,
  OUT CEPH_RBD_SERVICE_AUTH             *ServiceAuth
  );

// Build a CephXAuthorizer for a service connection from a service auth.
EFI_STATUS
CephRbdAuthBuildServiceAuthorizer (
  IN  CONST CEPH_RBD_SERVICE_AUTH  *ServiceAuth,
  IN  UINT64                       Nonce,
  OUT VOID                         *Buffer,
  IN  UINTN                        BufferCapacity,
  OUT UINTN                        *EncodedLength
  );

// Connect to an OSD messenger-v1 endpoint and authenticate with the OSD
// service ticket. Config supplies the OSD address/port and transport settings;
// its Authorizer fields are ignored (the authorizer is built from OsdAuth).
// The function performs the connect-time CephX challenge round-trip and
// leaves Client ready for OSD RPCs on success.
EFI_STATUS
CephRbdAuthConnectOsd (
  IN  CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG  *Config,
  IN  CONST CEPH_RBD_SERVICE_AUTH           *OsdAuth,
  OUT CEPH_RBD_MSGR_V1_CLIENT               *Client
  );

#endif
