#ifndef CEPH_RBD_MSGR_V1_SESSION_H_
#define CEPH_RBD_MSGR_V1_SESSION_H_

#include <Uefi.h>
#include "CephRbdMessenger.h"
#include "CephRbdMsgrV1Handshake.h"
#include "CephRbdTcp4.h"

typedef struct {
  CEPH_RBD_TCP4_CONNECTION  *Connection;
  UINT64                    NextOutgoingSequence;
  UINT64                    LastIncomingSequence;
  UINT64                    PeerAcknowledgedSequence;
  UINTN                     MaximumPayload;
  // Ceph uses a 13-byte legacy footer unless CEPH_FEATURE_MSG_AUTH is
  // negotiated (21 bytes with a signature). Derived from the negotiated
  // feature set at connect time and used for both send and receive.
  UINTN                     FooterSize;
  UINT32                    TimeoutSeconds;
  // A lossy peer (CEPH_MSG_CONNECT_LOSSY in the connect reply) never sends
  // CEPH_MSGR_TAG_ACK. Tracking such messages in the retransmit queue would
  // fill it permanently, so callers skip tracking on lossy sessions.
  BOOLEAN                   Lossy;
  BOOLEAN                   Ready;
} CEPH_MSGR_V1_SESSION;

EFI_STATUS
CephMsgrV1SessionInit (
  OUT CEPH_MSGR_V1_SESSION      *Session,
  IN CEPH_RBD_TCP4_CONNECTION   *Connection,
  IN CONST CEPH_MSGR_V1_NEGOTIATION *Negotiation,
  IN UINTN                      MaximumPayload,
  IN UINT32                     TimeoutSeconds
  );

EFI_STATUS
CephMsgrV1SessionSendMessage (
  IN OUT CEPH_MSGR_V1_SESSION  *Session,
  IN CONST CEPH_MSGR_V1_FRAME  *Frame,
  OUT VOID                     *FrameBuffer,
  IN UINTN                     FrameBufferCapacity,
  OUT UINTN                    *FrameLength
  );

EFI_STATUS
CephMsgrV1SessionReceiveMessage (
  IN OUT CEPH_MSGR_V1_SESSION  *Session,
  OUT VOID                     *FrameBuffer,
  IN UINTN                     FrameBufferCapacity,
  OUT CEPH_MSGR_V1_FRAME       *Frame,
  OUT UINTN                    *FrameLength
  );

#endif
