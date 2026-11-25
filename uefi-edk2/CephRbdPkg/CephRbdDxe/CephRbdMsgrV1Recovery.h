#ifndef CEPH_RBD_MSGR_V1_RECOVERY_H_
#define CEPH_RBD_MSGR_V1_RECOVERY_H_

#include <Uefi.h>
#include <Protocol/Tcp4.h>
#include "CephRbdMessenger.h"
#include "CephRbdMsgrV1Handshake.h"
#include "CephRbdMsgrV1Session.h"
#include "CephRbdTcp4.h"

// Bounded limits. Firmware must cap memory, so retransmission and retry are
// deliberately bounded instead of heap-allocated.
#define CEPH_MSGR_V1_RETRANSMIT_MAX          16
#define CEPH_MSGR_V1_RETRANSMIT_FRAME_MAX    4096
// Incoming frames (notably a full OSDMap) can be larger than the retransmit
// buffer; the session accepts up to this size and RPC owns the backing buffer.
#define CEPH_MSGR_V1_SESSION_FRAME_MAX       (16 * 1024)
#define CEPH_MSGR_V1_MAX_CONNECT_ATTEMPTS    8
#define CEPH_MSGR_V1_MAX_RPC_RETRIES         4
#define CEPH_MSGR_V1_MAX_IN_SEQ_GAP          1024

typedef struct {
  UINT64  Sequence;
  UINTN   Length;
  UINT32  Retries;
  BOOLEAN Active;
  UINT8   Buffer[CEPH_MSGR_V1_RETRANSMIT_FRAME_MAX];
} CEPH_MSGR_V1_RETRANSMIT_ENTRY;

typedef struct {
  CEPH_MSGR_V1_RETRANSMIT_ENTRY  Entry[CEPH_MSGR_V1_RETRANSMIT_MAX];
  UINTN                          Count;
  UINT32                         MaxRetries;
} CEPH_MSGR_V1_RETRANSMIT_QUEUE;

typedef struct {
  EFI_HANDLE              ImageHandle;
  EFI_HANDLE              ControllerHandle;
  EFI_IPv4_ADDRESS        RemoteAddress;
  UINT16                  RemotePort;
  UINT64                  SupportedFeatures;
  UINT64                  RequiredFeatures;
  UINT32                  HostType;
  UINT32                  ProtocolVersion;
  UINT32                  AuthorizerProtocol;
  CONST VOID              *Authorizer;
  UINT32                  AuthorizerLength;
  UINT8                   Flags;
  UINT32                  TimeoutSeconds;
  UINT32                  MaxConnectAttempts;
  UINT64                  ClientNonce;
} CEPH_RBD_MSGR_V1_CONNECT_CONFIG;

typedef struct {
  CEPH_RBD_TCP4_CONNECTION  Connection;
  CEPH_MSGR_V1_SESSION      Session;
  CEPH_MSGR_V1_NEGOTIATION  Negotiation;
  CEPH_MSGR_V1_RETRANSMIT_QUEUE  Retransmit;
  UINT32                    GlobalSequence;
  UINT32                    ConnectSequence;
  UINT64                    ClientNonce;
  UINT8                     ConnectionSecret[64];
  UINTN                     ConnectionSecretLength;
  BOOLEAN                   Connected;
  // Connect parameters are remembered so RPCs can reconnect and replay after
  // a transport failure without the caller having to pass them again.
  CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG  *RecoveryConfig;
} CEPH_RBD_MSGR_V1_CLIENT;

VOID
CephMsgrV1RetransmitQueueInit (
  OUT CEPH_MSGR_V1_RETRANSMIT_QUEUE  *Queue,
  IN UINT32                         MaxRetries  OPTIONAL
  );

EFI_STATUS
CephMsgrV1RetransmitQueueTrack (
  IN OUT CEPH_MSGR_V1_RETRANSMIT_QUEUE  *Queue,
  IN UINT64                             Sequence,
  IN CONST VOID                         *Frame,
  IN UINTN                              FrameLength
  );

EFI_STATUS
CephMsgrV1RetransmitQueueAcknowledge (
  IN OUT CEPH_MSGR_V1_RETRANSMIT_QUEUE  *Queue,
  IN UINT64                             AcknowledgedSequence,
  OUT UINTN                             *PendingCount  OPTIONAL
  );

EFI_STATUS
CephMsgrV1RetransmitQueueForEachPending (
  IN OUT CEPH_MSGR_V1_RETRANSMIT_QUEUE  *Queue,
  IN UINT64                             MinimumSequence,
  IN VOID                               *Context,
  IN EFI_STATUS (EFIAPI *Callback) (
       IN VOID         *Context,
       IN CONST VOID   *Frame,
       IN UINTN        FrameLength,
       IN UINT64       Sequence
       )
  );

UINTN
CephMsgrV1RetransmitQueuePendingCount (
  IN CONST CEPH_MSGR_V1_RETRANSMIT_QUEUE  *Queue,
  IN UINT64                               MinimumSequence
  );

BOOLEAN
CephMsgrV1RetransmitQueueExhausted (
  IN CONST CEPH_MSGR_V1_RETRANSMIT_QUEUE  *Queue,
  IN UINT64                               MinimumSequence
  );

// Connect with bounded retry. Handles RETRY_SESSION, RETRY_GLOBAL, WAIT,
// RESETSESSION, CHALLENGE_AUTH and SEQ recovery actions using the negotiated
// connect reply. On success the client owns a live TCP connection and session.
EFI_STATUS
CephRbdMsgrV1ClientConnect (
  IN CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG  *Config,
  OUT CEPH_RBD_MSGR_V1_CLIENT               *Client
  );

// Re-establish a dropped connection and replay unacknowledged frames.
EFI_STATUS
CephRbdMsgrV1ClientReconnect (
  IN CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG  *Config,
  IN OUT CEPH_RBD_MSGR_V1_CLIENT            *Client
  );

// Send a frame, tracking it for retransmission until the peer acknowledges it.
EFI_STATUS
CephRbdMsgrV1ClientSendTracked (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN CONST CEPH_MSGR_V1_FRAME     *Frame,
  OUT VOID                        *FrameBuffer,
  IN UINTN                        FrameBufferCapacity,
  OUT UINTN                       *FrameLength
  );

// Drive receive until a message frame arrives, processing ACK tags so the
// retransmit queue is pruned as the peer makes progress.
EFI_STATUS
CephRbdMsgrV1ClientReceiveMessage (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  OUT VOID                        *FrameBuffer,
  IN UINTN                        FrameBufferCapacity,
  OUT CEPH_MSGR_V1_FRAME          *Frame,
  OUT UINTN                       *FrameLength
  );

EFI_STATUS
CephRbdMsgrV1ClientClose (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN UINT32                       TimeoutSeconds
  );

#endif
