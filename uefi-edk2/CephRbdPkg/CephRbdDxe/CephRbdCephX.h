#ifndef CEPH_RBD_CEPHX_H_
#define CEPH_RBD_CEPHX_H_

#include <Uefi.h>

#define CEPHX_REQUEST_GET_AUTH_SESSION_KEY  0x0100
#define CEPHX_REQUEST_GET_PRINCIPAL_KEY     0x0200
#define CEPHX_REQUEST_GET_ROTATING_KEY      0x0400

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

#endif
