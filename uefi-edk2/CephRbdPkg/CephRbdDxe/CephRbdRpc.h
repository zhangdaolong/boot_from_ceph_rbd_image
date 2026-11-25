#ifndef CEPH_RBD_RPC_H_
#define CEPH_RBD_RPC_H_

#include <Uefi.h>
#include "CephRbdMessenger.h"
#include "CephRbdMsgrV1Recovery.h"

// Message types from include/msgr.h and ceph_fs.h.
#define CEPH_MSG_MON_GET_MAP          5
#define CEPH_MSG_MON_COMMAND          50
#define CEPH_MSG_MON_COMMAND_ACK      51
#define CEPH_MSG_OSD_MAP              41
#define CEPH_MSG_OSD_OP               42
#define CEPH_MSG_OSD_OPREPLY          43

// Entity types from include/msgr.h.
#define CEPH_ENTITY_TYPE_MON          0x01
#define CEPH_ENTITY_TYPE_OSD          0x04
#define CEPH_ENTITY_TYPE_CLIENT       0x08

// OSD op opcodes from include/rados.h.
#define CEPH_OSD_OP_READ              0x1201
#define CEPH_OSD_OP_STAT              0x1202
#define CEPH_OSD_OP_CALL              0x1401
#define CEPH_OSD_FLAG_READ            0x0010
#define CEPH_OSD_OP_FLAG_FADVISE_READAHEAD 0x0001

// Wire sizes.
#define CEPH_MON_REQUEST_HEADER_SIZE  18   // u64 have_version, u16 session_mon, u64 tid
#define CEPH_FSID_SIZE                16
#define CEPH_OSD_OP_SIZE              38
#define CEPH_RPC_MAX_COMMAND          4096
#define CEPH_RPC_MAX_CLASS            32
#define CEPH_RPC_MAX_METHOD           32
#define CEPH_RPC_MAX_INDATA           2048
#define CEPH_RPC_MAX_METADATA         4096
#define CEPH_RPC_DEFAULT_OSD_EPOCH    1

typedef struct {
  UINT64  HaveVersion;
  UINT16  SessionMon;
  UINT64  SessionMonTid;
} CEPH_MON_REQUEST_HEADER;

typedef struct {
  UINT32  PoolId;
  UINT64  PoolEpoch;
  UINT64  OsdEpoch;
} CEPH_OSD_MAP_SUMMARY;

typedef struct {
  UINT16  Op;
  UINT32  Flags;
  UINT64  Offset;
  UINT64  Length;
  UINT8   ClassName[CEPH_RPC_MAX_CLASS];
  UINTN   ClassNameLength;
  UINT8   MethodName[CEPH_RPC_MAX_METHOD];
  UINTN   MethodNameLength;
  CONST UINT8  *Indata;
  UINTN        IndataLength;
} CEPH_OSD_OP_REQUEST;

typedef struct {
  INT32        Result;
  UINT32       NumOps;
  CONST UINT8  *Data;
  UINTN        DataLength;
} CEPH_OSD_OP_REPLY;

// Monitor RPC codecs.
EFI_STATUS
CephRpcEncodeMonGetMap (
  IN CONST CEPH_MON_REQUEST_HEADER  *Header,
  IN CONST UINT8                    *Fsid,
  IN UINT32                         StartEpoch,
  OUT VOID                          *Buffer,
  IN UINTN                          BufferLength,
  OUT UINTN                         *EncodedLength
  );

EFI_STATUS
CephRpcEncodeMonCommand (
  IN CONST CEPH_MON_REQUEST_HEADER  *Header,
  IN CONST UINT8                    *Fsid,
  IN CONST CHAR8                    *Command,
  IN UINTN                          CommandLength,
  OUT VOID                          *Buffer,
  IN UINTN                          BufferLength,
  OUT UINTN                         *EncodedLength
  );

EFI_STATUS
CephRpcDecodeMonCommandAck (
  IN CONST VOID  *Buffer,
  IN UINTN       BufferLength,
  OUT INT32      *Result,
  OUT CONST UINT8  **OutData,  OPTIONAL
  OUT UINTN      *OutDataLength  OPTIONAL
  );

// Decode the leading epoch field of a CEPH_MSG_OSD_MAP front payload.
EFI_STATUS
CephRpcDecodeOsdMapEpoch (
  IN CONST VOID  *Buffer,
  IN UINTN       BufferLength,
  OUT UINT64     *Epoch
  );

// OSD RPC codecs.
EFI_STATUS
CephRpcEncodeOsdOp (
  IN UINT32                         PoolId,
  IN UINT32                         ClientIncarnation,
  IN UINT32                         OsdmapEpoch,
  IN UINT64                         RequestId,
  IN CONST CHAR8                    *ObjectName,
  IN UINTN                          ObjectNameLength,
  IN CONST CEPH_OSD_OP_REQUEST      *Ops,
  IN UINTN                          OpCount,
  OUT VOID                          *Buffer,
  IN UINTN                          BufferLength,
  OUT UINTN                         *EncodedLength
  );

EFI_STATUS
CephRpcDecodeOsdOpReply (
  IN CONST VOID          *Front,
  IN UINTN               FrontLength,
  IN CONST VOID          *Data,
  IN UINTN               DataLength,
  OUT CEPH_OSD_OP_REPLY  *Reply
  );

// Generic transaction: send a request frame and wait for a reply whose
// transaction id and message type match. Processed control tags are handled
// by the session layer. Final reply payload is copied into the caller buffer.
EFI_STATUS
CephRpcCall (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN UINT16                       RequestType,
  IN UINT16                       ExpectedReplyType,
  IN CONST VOID                   *Front,
  IN UINTN                        FrontLength,
  IN CONST VOID                   *Data,  OPTIONAL
  IN UINTN                        DataLength,
  OUT VOID                        *ReplyFront,
  IN UINTN                        ReplyFrontCapacity,
  OUT UINTN                       *ReplyFrontLength,
  OUT VOID                        *ReplyData,  OPTIONAL
  IN UINTN                        ReplyDataCapacity,
  OUT UINTN                       *ReplyDataLength  OPTIONAL
  );

#endif
