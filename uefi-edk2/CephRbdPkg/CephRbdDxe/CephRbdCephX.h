#ifndef CEPH_RBD_CEPHX_H_
#define CEPH_RBD_CEPHX_H_

#include <Uefi.h>

#define CEPHX_REQUEST_GET_AUTH_SESSION_KEY  0x0100
#define CEPHX_REQUEST_GET_PRINCIPAL_KEY     0x0200
#define CEPHX_REQUEST_GET_ROTATING_KEY      0x0400
#define CEPHX_AES_KEY_SIZE                  16
#define CEPHX_MAX_ENCRYPTED_PAYLOAD         4096
#define CEPHX_MAX_TICKET_BLOB               2048

// Ceph crypto key types (include/linux/ceph/ceph_fs.h).
#define CEPHX_CRYPTO_NONE                   0
#define CEPHX_CRYPTO_AES                    1
#define CEPHX_CRYPTO_AES256_KRB5            2

// CEPH_ENTITY_TYPE_AUTH from include/ceph_fs.h (same value as
// CEPH_ENTITY_TYPE_AUTH_SERVICE in CephRbdAuth.h).
#define CEPHX_ENTITY_TYPE_AUTH              0x20

// CEPH_AUTH_UID_DEFAULT is ((__u64)-1); the monitor writes this constant when
// encoding an AuthTicket regardless of the decoded auid.
#define CEPHX_AUTH_UID_DEFAULT              MAX_UINT64
#define CEPHX_MAX_ENTITY_NAME               96
#define CEPHX_MAX_CAPS_BYTES                512
#define CEPHX_TICKET_VALIDITY_SLOP_SECONDS  300

typedef struct {
  UINT64  ServerChallenge;
} CEPHX_AUTHORIZE_CHALLENGE;

typedef struct {
  UINT64  NoncePlusOne;
  UINTN   ConnectionSecretLength;
} CEPHX_AUTHORIZE_REPLY;

typedef struct {
  UINT64  ServerChallenge;
} CEPHX_SERVER_CHALLENGE;

typedef struct {
  UINT16  RequestType;
  INT32   Status;
} CEPHX_RESPONSE_HEADER;

// CryptoKey wire form from auth/Crypto.cc: u16 type, utime_t (u32 sec,
// u32 nsec), u16 secret length, raw secret bytes.
typedef struct {
  UINT16  Type;
  UINT32  CreatedSeconds;
  UINT32  CreatedNanoseconds;
  UINT16  KeyLength;
  UINT8   Key[CEPHX_AES_KEY_SIZE];
} CEPHX_CRYPTO_KEY;

// Decoded fields of a CephXTicketBlob. Blob points into the caller buffer.
typedef struct {
  UINT64       SecretId;
  CONST UINT8  *Blob;
  UINTN        BlobLength;
} CEPHX_TICKET_BLOB;

// Decoded, validated CephXServiceTicketInfo. Only AES-128 session keys are
// supported by this firmware subset.
typedef struct {
  UINT64  GlobalId;
  UINT64  Auid;
  UINT32  CreatedSeconds;
  UINT32  CreatedNanoseconds;
  UINT32  ExpiresSeconds;
  UINT32  ExpiresNanoseconds;
  UINT8   AllowAll;
  UINT32  Flags;
  UINT64  SecretId;
  CEPHX_CRYPTO_KEY  SessionKey;
} CEPHX_SERVICE_TICKET_INFO;

EFI_STATUS
CephXDecodeServerChallenge (
  IN CONST VOID              *Buffer,
  IN UINTN                   BufferLength,
  OUT CEPHX_SERVER_CHALLENGE *Challenge
  );

EFI_STATUS
CephXDecodeResponseHeader (
  IN CONST VOID          *Buffer,
  IN UINTN               BufferLength,
  OUT CEPHX_RESPONSE_HEADER *Response
  );

EFI_STATUS
CephXEncodeRequestHeader (
  IN UINT16        RequestType,
  OUT VOID         *Buffer,
  IN UINTN         BufferLength,
  OUT UINTN        *EncodedLength
  );

EFI_STATUS
CephXEncodeServiceTicketRequest (
  IN UINT32        ServiceMask,
  OUT VOID         *Buffer,
  IN UINTN         BufferLength,
  OUT UINTN        *EncodedLength
  );

EFI_STATUS
CephXDecodeAuthorizeChallenge (
  IN CONST VOID                  *Buffer,
  IN UINTN                       BufferLength,
  OUT CEPHX_AUTHORIZE_CHALLENGE  *Challenge
  );

EFI_STATUS
CephXEncodeAuthorize (
  IN UINT64        Nonce,
  IN BOOLEAN       HaveChallenge,
  IN UINT64        ServerChallengePlusOne,
  OUT VOID         *Buffer,
  IN UINTN         BufferLength,
  OUT UINTN        *EncodedLength
  );

EFI_STATUS
CephXDecodeAuthorizeReply (
  IN CONST VOID          *Buffer,
  IN UINTN               BufferLength,
  OUT CEPHX_AUTHORIZE_REPLY *Reply,
  OUT VOID               *ConnectionSecret,
  IN UINTN               ConnectionSecretCapacity
  );

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
  );

EFI_STATUS
CephXEncryptAes128Payload (
  IN CONST UINT8  *Key,
  IN UINTN        KeyLength,
  IN CONST VOID   *Payload,
  IN UINTN        PayloadLength,
  OUT VOID        *Buffer,
  IN UINTN        BufferLength,
  OUT UINTN       *EncodedLength
  );

EFI_STATUS
CephXComputeAesChallengeResponse (
  IN CONST UINT8  *Key,
  IN UINTN        KeyLength,
  IN UINT64       ServerChallenge,
  IN UINT64       ClientChallenge,
  OUT UINT64      *ChallengeResponse
  );

// Decrypt a Ceph encode_encrypt payload produced by CephXEncryptAes128Payload.
// Validates the length prefix, PKCS#7 padding, struct version and AUTH_ENC_MAGIC.
EFI_STATUS
CephXDecryptAes128Payload (
  IN CONST UINT8  *Key,
  IN UINTN        KeyLength,
  IN CONST VOID   *Buffer,
  IN UINTN        BufferLength,
  OUT VOID        *Payload,
  IN UINTN        PayloadCapacity,
  OUT UINTN       *PayloadLength
  );

// Extract the AUTH ticket session key from the final GET_AUTH_SESSION_KEY
// reply's result_bl. The key is decrypted with the client's own secret; only
// AES-128 keys are accepted. SessionKeyCapacity must be at least
// CEPHX_AES_KEY_SIZE.
//
// The same reply also carries a CephXTicketBlob for the AUTH service. When
// TicketBlob and TicketBlobLength are non-NULL, the blob's secret_id and raw
// ciphertext are copied out (TicketBlobCapacity bounds the copy) so the
// client can present it as old_ticket in a GET_PRINCIPAL_SESSION_KEY request.
// TicketSecretId is optional. The blob is opaque to the client: it is
// encrypted with the AUTH service secret held only by the monitor.
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
  );

// Decode the version-1 CephXTicketBlob wrapper. The returned Blob pointer
// references Buffer and is valid only while Buffer is alive.
EFI_STATUS
CephXDecodeTicketBlob (
  IN CONST VOID         *Buffer,
  IN UINTN              BufferLength,
  OUT CEPHX_TICKET_BLOB  *Ticket
  );

// Decrypt and validate a service ticket blob with the service secret, then
// extract the session key, global id, auid and validity window. NowSeconds is
// the firmware clock in seconds since the epoch; 0 disables expiry checking.
EFI_STATUS
CephXDecodeAndValidateTicket (
  IN CONST CEPHX_TICKET_BLOB       *Ticket,
  IN CONST UINT8                   *ServiceSecret,
  IN UINTN                         ServiceSecretLength,
  IN UINT64                        ExpectedGlobalId,
  IN UINT64                        NowSeconds,
  OUT CEPHX_SERVICE_TICKET_INFO    *Info
  );

// Verify a CephXAuthorizeReply that was encrypted with the ticket session key.
// The reply must contain nonce_plus_one == ClientNonce + 1. The optional
// connection secret is copied out when present and capacity allows.
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
  );

// Decode an encrypted CephXAuthorizeChallenge and return the server challenge.
EFI_STATUS
CephXDecodeAuthorizeChallengeSecret (
  IN CONST UINT8  *SessionKey,
  IN UINTN        SessionKeyLength,
  IN CONST VOID   *Buffer,
  IN UINTN        BufferLength,
  OUT UINT64      *ServerChallenge
  );

// Decode a GET_PRINCIPAL_SESSION_KEY reply's result_bl and extract the service
// ticket for ExpectedServiceId (e.g. CEPH_ENTITY_TYPE_OSD). Both the encrypted
// CephXServiceTicket (holding the service session key) and the encrypted
// CephXTicketBlob are decrypted with PrincipalSessionKey -- the AUTH ticket
// session key that anchors the principal. Only AES-128 keys are accepted.
EFI_STATUS
CephXDecodeServiceTicket (
  IN  CONST VOID   *Buffer,
  IN  UINTN        BufferLength,
  IN  CONST UINT8  *PrincipalSessionKey,
  IN  UINTN        PrincipalSessionKeyLength,
  IN  UINT32       ExpectedServiceId,
  OUT UINT8        *ServiceSessionKey,
  IN  UINTN        ServiceSessionKeyCapacity,
  OUT UINT64       *TicketSecretId,  OPTIONAL
  OUT VOID         *TicketBlob,  OPTIONAL
  IN  UINTN        TicketBlobCapacity,
  OUT UINTN        *TicketBlobLength  OPTIONAL
  );

// Build a CephXAuthorizer blob for a service. Layout:
//   u8 authorizer_v (=1), u64 global_id, u32 service_id,
//   CephXTicketBlob ticket (u8 v=1, u64 secret_id, bufferlist blob),
//   encode_encrypt(CephXAuthorize v2, SessionKey).
// The encrypted CephXAuthorize carries a nonce and no challenge (the caller
// can append a challenge round-trip later by re-encrypting with a nonce).
EFI_STATUS
CephXEncodeServiceAuthorizer (
  IN  UINT64        GlobalId,
  IN  UINT32        ServiceId,
  IN  UINT64        TicketSecretId,
  IN  CONST VOID    *TicketBlob,
  IN  UINTN         TicketBlobLength,
  IN  CONST UINT8   *SessionKey,
  IN  UINTN         SessionKeyLength,
  IN  UINT64        Nonce,
  OUT VOID          *Buffer,
  IN  UINTN         BufferCapacity,
  OUT UINTN         *EncodedLength
  );

#endif
