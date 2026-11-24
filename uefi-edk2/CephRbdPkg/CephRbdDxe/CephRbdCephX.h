#ifndef CEPH_RBD_CEPHX_H_
#define CEPH_RBD_CEPHX_H_

#include <Uefi.h>

#define CEPHX_REQUEST_GET_AUTH_SESSION_KEY  0x0100

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
