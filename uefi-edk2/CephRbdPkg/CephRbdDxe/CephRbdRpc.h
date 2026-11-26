#ifndef CEPH_RBD_RPC_H_
#define CEPH_RBD_RPC_H_

#include <Uefi.h>
#include "CephRbdMessenger.h"
#include "CephRbdMsgrV1Recovery.h"

// Message types from include/msgr.h and ceph_fs.h.
#define CEPH_MSG_MON_MAP              4
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
// PaxosServiceMessage::paxos_encode() emits u64 version, __s16
// deprecated_session_mon and u64 deprecated_session_mon_tid.  MMonGetMap,
// MMonCommand and MMonCommandAck all share this 18-byte prefix; the session
// mon field is a *signed 16-bit* value in Ceph (verified against Ceph 22
// messages/PaxosServiceMessage.h), not a 32-bit field.
#define CEPH_PAXOS_REQUEST_HEADER_SIZE  18
#define CEPH_MON_REQUEST_HEADER_SIZE  CEPH_PAXOS_REQUEST_HEADER_SIZE
#define CEPH_FSID_SIZE                16
#define CEPH_OSD_OP_SIZE              38
#define CEPH_RPC_MAX_COMMAND          4096
#define CEPH_RPC_MAX_CLASS            32
#define CEPH_RPC_MAX_METHOD           32
#define CEPH_RPC_MAX_INDATA           2048
#define CEPH_RPC_MAX_METADATA         4096
#define CEPH_RPC_DEFAULT_OSD_EPOCH    1
// A full OSDMap front payload can exceed the retransmit frame size (a small
// cluster already produces ~4 KiB), so RPC uses its own larger receive buffer.
#define CEPH_RPC_FRAME_MAX            (16 * 1024)

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

// Decode the epoch of a CEPH_MSG_MON_MAP front payload (the reply to
// MON_GET_MAP). The front carries one encoded bufferlist: ENCODE_START(10,6),
// fsid (16 bytes), then the signed epoch (le32).
EFI_STATUS
CephRpcDecodeMonMapEpoch (
  IN CONST VOID  *Buffer,
  IN UINTN       BufferLength,
  OUT UINT64     *Epoch
  );

// OSD RPC codecs.
// Encode a legacy (v1) MOSDOp front payload. For CEPH_OSD_OP_CALL ops the
// class name, method name and in-data are NOT part of the front; Ceph carries
// them in the message data buffer, with each op's payload_len set to the
// number of data bytes it consumes. Data/DataCapacity/DataLength may be NULL
// when no CALL op is present (e.g. plain READ/STAT ops).
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
  OUT UINTN                         *EncodedLength,
  OUT VOID                          *Data  OPTIONAL,
  IN UINTN                          DataCapacity  OPTIONAL,
  OUT UINTN                         *DataLength  OPTIONAL
  );

EFI_STATUS
CephRpcDecodeOsdOpReply (
  IN CONST VOID          *Front,
  IN UINTN               FrontLength,
  IN CONST VOID          *Data,
  IN UINTN               DataLength,
  OUT CEPH_OSD_OP_REPLY  *Reply
  );

// Reconnect/failover support. The generic runner is deliberately independent
// of sockets so host tests can drive its retry policy with a fake attempt.
#define CEPH_RPC_MAX_RECOVERY_ATTEMPTS  4

typedef EFI_STATUS (EFIAPI *CEPH_RPC_ATTEMPT) (
  IN VOID   *Context,
  IN UINTN  EndpointIndex
  );

// Connection loss, timeouts and device errors are safe to retry; protocol or
// credential errors are not.
BOOLEAN
CephRpcStatusIsRecoverable (
  IN EFI_STATUS  Status
  );

// Call Attempt for each endpoint, retrying recoverable failures up to
// MaxAttemptsPerEndpoint before moving to the next endpoint. A successful
// attempt returns immediately; a hard error skips to the next endpoint.
EFI_STATUS
CephRpcRunWithFailover (
  IN UINTN            EndpointCount,
  IN UINTN            MaxAttemptsPerEndpoint,
  IN VOID             *Context,
  IN CEPH_RPC_ATTEMPT  Attempt
  );

typedef struct {
  CEPH_RBD_MSGR_V1_CLIENT               *Client;
  CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG *Config;
  UINT16                                 RequestType;
  UINT16                                 ExpectedReplyType;
  CONST VOID                            *Front;
  UINTN                                  FrontLength;
  CONST VOID                            *Data;
  UINTN                                  DataLength;
  VOID                                  *ReplyFront;
  UINTN                                  ReplyFrontCapacity;
  UINTN                                 *ReplyFrontLength;
  VOID                                  *ReplyData;
  UINTN                                  ReplyDataCapacity;
  UINTN                                 *ReplyDataLength;
  UINTN                                  AttemptIndex;
} CEPH_RPC_RECOVERY_CONTEXT;

// Retry a single RPC after reconnecting on a recoverable failure, replaying
// unacknowledged frames via the messenger reconnect path.
EFI_STATUS
CephRpcCallWithRecovery (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT               *Client,
  IN     CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG *Config,
  IN     UINT16                                 RequestType,
  IN     UINT16                                 ExpectedReplyType,
  IN     CONST VOID                             *Front,
  IN     UINTN                                  FrontLength,
  IN     CONST VOID                             *Data,  OPTIONAL
  IN     UINTN                                  DataLength,
  OUT    VOID                                   *ReplyFront,
  IN     UINTN                                  ReplyFrontCapacity,
  OUT    UINTN                                  *ReplyFrontLength,
  OUT    VOID                                   *ReplyData,  OPTIONAL
  IN     UINTN                                  ReplyDataCapacity,
  OUT    UINTN                                  *ReplyDataLength  OPTIONAL
  );

// Same as CephRpcCall but automatically reconnects and retries when the
// client carries a connect config (set by CephRbdMsgrV1ClientConnect).
EFI_STATUS
CephRpcCallAuto (
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
