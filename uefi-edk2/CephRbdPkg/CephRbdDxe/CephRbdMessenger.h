#ifndef CEPH_RBD_MESSENGER_H_
#define CEPH_RBD_MESSENGER_H_

#include <Uefi.h>

#define CEPH_MSGR_V1_TAG_MESSAGE       7
#define CEPH_MSGR_V1_HEADER_SIZE       53
#define CEPH_MSGR_V1_FOOTER_SIZE       21
#define CEPH_MSGR_V1_FOOTER_COMPLETE  0x01
#define CEPH_MSGR_V1_FOOTER_NOCRC     0x02
#define CEPH_MSGR_V1_FOOTER_SIGNED    0x04
#define CEPH_MSGR_TAG_READY            1
#define CEPH_MSGR_TAG_RESETSESSION     2
#define CEPH_MSGR_TAG_WAIT             3
#define CEPH_MSGR_TAG_RETRY_SESSION    4
#define CEPH_MSGR_TAG_RETRY_GLOBAL     5
#define CEPH_MSGR_TAG_CLOSE            6
#define CEPH_MSGR_TAG_MESSAGE          7
#define CEPH_MSGR_TAG_ACK              8
#define CEPH_MSGR_TAG_KEEPALIVE        9
#define CEPH_MSGR_TAG_BADPROTOVER     10
#define CEPH_MSGR_TAG_BADAUTHORIZER   11
#define CEPH_MSGR_TAG_FEATURES        12
#define CEPH_MSGR_TAG_SEQ             13
#define CEPH_MSGR_TAG_KEEPALIVE2      14
#define CEPH_MSGR_TAG_KEEPALIVE2_ACK  15
#define CEPH_MSGR_TAG_CHALLENGE_AUTH  16
#define CEPH_MSGR_CONNECT_LOSSY        1
#define CEPH_MSGR_CONNECT_HOST_CLIENT  8
#define CEPH_MSGR_V1_CONNECT_SIZE      33
#define CEPH_MSGR_V1_CONNECT_REPLY_SIZE 26

typedef enum {
  CephMsgrConnectReady,
  CephMsgrConnectRetrySession,
  CephMsgrConnectRetryGlobal,
  CephMsgrConnectResetSession,
  CephMsgrConnectWait,
  CephMsgrConnectAckSequence,
  CephMsgrConnectNeedAuthorizerChallenge,
  CephMsgrConnectRejected
} CEPH_MSGR_CONNECT_ACTION;

typedef struct {
  UINT8   Tag;
  UINT64  Features;
  UINT32  GlobalSequence;
  UINT32  ConnectSequence;
  UINT32  ProtocolVersion;
  UINT32  AuthorizerLength;
  UINT8   Flags;
} CEPH_MSGR_V1_CONNECT_REPLY;

typedef struct {
  UINT64       Sequence;
  UINT64       TransactionId;
  UINT16       Type;
  UINT16       Priority;
  UINT16       Version;
  UINT16       DataOffset;
  UINT32       FrontLength;
  UINT32       MiddleLength;
  UINT32       DataLength;
  CONST UINT8  *Front;
  CONST UINT8  *Middle;
  CONST UINT8  *Data;
  UINT8        FooterFlags;
} CEPH_MSGR_V1_FRAME;

UINT32
CephMsgrCrc32c (
  IN CONST VOID  *Data,
  IN UINTN       Length
  );

EFI_STATUS
CephMsgrDecodeV1Frame (
  IN CONST VOID          *Buffer,
  IN UINTN               BufferLength,
  IN UINTN               MaximumPayload,
  OUT CEPH_MSGR_V1_FRAME *Frame
  );

EFI_STATUS
CephMsgrEncodeV1Connect (
  IN UINT64       SupportedFeatures,
  IN UINT32       HostType,
  IN UINT32       GlobalSequence,
  IN UINT32       ConnectSequence,
  IN UINT32       ProtocolVersion,
  IN UINT32       AuthorizerProtocol,
  IN CONST VOID   *Authorizer,
  IN UINT32       AuthorizerLength,
  IN UINT8        Flags,
  OUT VOID        *Buffer,
  IN UINTN        BufferLength,
  OUT UINTN       *EncodedLength
  );

EFI_STATUS
CephMsgrDecodeV1ConnectReply (
  IN CONST VOID                   *Buffer,
  IN UINTN                        BufferLength,
  OUT CEPH_MSGR_V1_CONNECT_REPLY  *Reply
  );

EFI_STATUS
CephMsgrClassifyV1ConnectReply (
  IN CONST CEPH_MSGR_V1_CONNECT_REPLY  *Reply,
  IN UINT64                            RequiredFeatures,
  IN UINT32                            ExpectedProtocolVersion,
  IN UINT32                            CurrentConnectSequence,
  OUT CEPH_MSGR_CONNECT_ACTION         *Action
  );

#endif
