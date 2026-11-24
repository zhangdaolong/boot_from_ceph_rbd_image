#ifndef CEPH_RBD_MESSENGER_H_
#define CEPH_RBD_MESSENGER_H_

#include <Uefi.h>

#define CEPH_MSGR_V1_TAG_MESSAGE       7
#define CEPH_MSGR_V1_HEADER_SIZE       53
#define CEPH_MSGR_V1_FOOTER_SIZE       21
#define CEPH_MSGR_V1_FOOTER_COMPLETE  0x01
#define CEPH_MSGR_V1_FOOTER_NOCRC     0x02
#define CEPH_MSGR_V1_FOOTER_SIGNED    0x04

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

#endif
