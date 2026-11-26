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
  UINT8   SessionKey[CEPHX_AES_KEY_SIZE]; // AUTH ticket session key (AES-128).
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

#endif
