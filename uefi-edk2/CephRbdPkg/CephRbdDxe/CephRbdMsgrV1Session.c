#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "CephRbdMsgrV1Session.h"
#include "CephRbdMsgrV1Transport.h"
#include "CephRbdTcp4.h"

#define CEPH_MSGR_V1_KEEPALIVE2_PAYLOAD_SIZE  8
#define CEPH_MSGR_V1_ACK_SIZE                 9

STATIC
UINT64
CephMsgrV1SessionReadLe64 (
  IN CONST UINT8  *Data
  )
{
  UINT64  Value;
  UINTN   Index;

  Value = 0;
  for (Index = 0; Index < sizeof (Value); Index++) {
    Value |= (UINT64)Data[Index] << (Index * 8);
  }

  return Value;
}

STATIC
VOID
CephMsgrV1SessionWriteLe64 (
  OUT UINT8  *Data,
  IN UINT64  Value
  )
{
  UINTN  Index;

  for (Index = 0; Index < sizeof (Value); Index++) {
    Data[Index] = (UINT8)(Value >> (Index * 8));
  }
}

STATIC
EFI_STATUS
CephMsgrV1SessionSendAck (
  IN CEPH_MSGR_V1_SESSION  *Session,
  IN UINT64                 Sequence
  )
{
  UINT8  Ack[CEPH_MSGR_V1_ACK_SIZE];

  Ack[0] = CEPH_MSGR_TAG_ACK;
  CephMsgrV1SessionWriteLe64 (Ack + 1, Sequence);
  return CephRbdTcp4Send (
           Session->Connection,
           Ack,
           sizeof (Ack),
           Session->TimeoutSeconds
           );
}

STATIC
EFI_STATUS
CephMsgrV1SessionHandleControlTag (
  IN OUT CEPH_MSGR_V1_SESSION  *Session,
  IN UINT8                     Tag
  )
{
  EFI_STATUS  Status;
  UINT8       Payload[CEPH_MSGR_V1_KEEPALIVE2_PAYLOAD_SIZE];
  UINT64      AcknowledgedSequence;
  UINT8       KeepaliveReply[1 + CEPH_MSGR_V1_KEEPALIVE2_PAYLOAD_SIZE];

  switch (Tag) {
    case CEPH_MSGR_TAG_ACK:
      Status = CephRbdTcp4ReceiveExact (
                 Session->Connection,
                 Payload,
                 sizeof (Payload),
                 Session->TimeoutSeconds
                 );
      if (EFI_ERROR (Status)) {
        return Status;
      }

      AcknowledgedSequence = CephMsgrV1SessionReadLe64 (Payload);
      if (AcknowledgedSequence > Session->NextOutgoingSequence - 1) {
        Session->Ready = FALSE;
        return EFI_COMPROMISED_DATA;
      }

      if (AcknowledgedSequence > Session->PeerAcknowledgedSequence) {
        Session->PeerAcknowledgedSequence = AcknowledgedSequence;
      }

      return EFI_SUCCESS;

    case CEPH_MSGR_TAG_KEEPALIVE:
      return EFI_SUCCESS;

    case CEPH_MSGR_TAG_KEEPALIVE2:
      Status = CephRbdTcp4ReceiveExact (
                 Session->Connection,
                 Payload,
                 sizeof (Payload),
                 Session->TimeoutSeconds
                 );
      if (EFI_ERROR (Status)) {
        return Status;
      }

      KeepaliveReply[0] = CEPH_MSGR_TAG_KEEPALIVE2_ACK;
      CopyMem (KeepaliveReply + 1, Payload, sizeof (Payload));
      return CephRbdTcp4Send (
               Session->Connection,
               KeepaliveReply,
               sizeof (KeepaliveReply),
               Session->TimeoutSeconds
               );

    case CEPH_MSGR_TAG_KEEPALIVE2_ACK:
      return CephRbdTcp4ReceiveExact (
               Session->Connection,
               Payload,
               sizeof (Payload),
               Session->TimeoutSeconds
               );

    case CEPH_MSGR_TAG_CLOSE:
      Session->Ready = FALSE;
      return EFI_CONNECTION_FIN;

    default:
      Session->Ready = FALSE;
      return EFI_UNSUPPORTED;
  }
}

EFI_STATUS
CephMsgrV1SessionInit (
  OUT CEPH_MSGR_V1_SESSION          *Session,
  IN CEPH_RBD_TCP4_CONNECTION       *Connection,
  IN CONST CEPH_MSGR_V1_NEGOTIATION *Negotiation,
  IN UINTN                          MaximumPayload,
  IN UINT32                         TimeoutSeconds
  )
{
  if ((Session == NULL) || (Connection == NULL) ||
      (Negotiation == NULL) || !Connection->Connected ||
      (Connection->Tcp4 == NULL) || !Negotiation->Ready ||
      (Negotiation->Action != CephMsgrConnectReady) ||
      (MaximumPayload == 0) || (TimeoutSeconds == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Session, sizeof (*Session));
  Session->Connection      = Connection;
  Session->NextOutgoingSequence = 1;
  Session->MaximumPayload  = MaximumPayload;
  // connect_reply.features can include server-required bits even when we did
  // not advertise them. The effective negotiated set is the intersection with
  // what we sent in the connect frame. Without MSG_AUTH, Ceph uses the
  // 13-byte ceph_msg_footer_old; otherwise the 21-byte signed form.
  Session->FooterSize =
    (((Negotiation->ConnectReply.Features &
       Negotiation->SupportedFeatures) & CEPH_FEATURE_MSG_AUTH) != 0) ?
    CEPH_MSGR_V1_FOOTER_SIZE : CEPH_MSGR_V1_FOOTER_LEGACY_SIZE;
  Session->TimeoutSeconds = TimeoutSeconds;
  // OSD endpoints reply with CEPH_MSG_CONNECT_LOSSY and never send ACK tags.
  Session->Lossy =
    ((Negotiation->ConnectReply.Flags & CEPH_MSGR_CONNECT_LOSSY) != 0);
  Session->Ready           = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
CephMsgrV1SessionSendMessage (
  IN OUT CEPH_MSGR_V1_SESSION  *Session,
  IN CONST CEPH_MSGR_V1_FRAME  *Frame,
  OUT VOID                     *FrameBuffer,
  IN UINTN                     FrameBufferCapacity,
  OUT UINTN                    *FrameLength
  )
{
  EFI_STATUS        Status;
  CEPH_MSGR_V1_FRAME Outgoing;

  if ((Session == NULL) || (Frame == NULL) ||
      (FrameBuffer == NULL) || (FrameLength == NULL) ||
      !Session->Ready || (Session->Connection == NULL) ||
      (Session->NextOutgoingSequence == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (Session->NextOutgoingSequence == MAX_UINT64) {
    *FrameLength = 0;
    return EFI_BAD_BUFFER_SIZE;
  }

  CopyMem (&Outgoing, Frame, sizeof (Outgoing));
  Outgoing.Sequence = Session->NextOutgoingSequence;
  if (Session->FooterSize == CEPH_MSGR_V1_FOOTER_LEGACY_SIZE) {
    Outgoing.FooterFlags |= CEPH_MSGR_V1_FOOTER_LEGACY;
  }
  *FrameLength = 0;
  Status = CephMsgrEncodeV1Frame (
             &Outgoing,
             Session->MaximumPayload,
             FrameBuffer,
             FrameBufferCapacity,
             FrameLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdTcp4Send (
             Session->Connection,
             FrameBuffer,
             *FrameLength,
             Session->TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    Session->Ready = FALSE;
    *FrameLength = 0;
    return Status;
  }

  Session->NextOutgoingSequence++;
  return EFI_SUCCESS;
}

EFI_STATUS
CephMsgrV1SessionReceiveMessage (
  IN OUT CEPH_MSGR_V1_SESSION  *Session,
  OUT VOID                     *FrameBuffer,
  IN UINTN                     FrameBufferCapacity,
  OUT CEPH_MSGR_V1_FRAME       *Frame,
  OUT UINTN                    *FrameLength
  )
{
  EFI_STATUS  Status;
  UINT8       Tag;
  UINT64      ExpectedSequence;

  if ((Session == NULL) || (FrameBuffer == NULL) ||
      (Frame == NULL) || (FrameLength == NULL) ||
      !Session->Ready || (Session->Connection == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *FrameLength = 0;
  while (Session->Ready) {
    Status = CephRbdTcp4ReceiveExact (
               Session->Connection,
               &Tag,
               sizeof (Tag),
               Session->TimeoutSeconds
               );
    if (EFI_ERROR (Status)) {
      Session->Ready = FALSE;
      return Status;
    }

    if (Tag != CEPH_MSGR_TAG_MESSAGE) {
      Status = CephMsgrV1SessionHandleControlTag (Session, Tag);
      if (EFI_ERROR (Status)) {
        Session->Ready = FALSE;
        return Status;
      }

      continue;
    }

    Status = CephMsgrV1ReceiveFrameAfterTag (
               Session->Connection,
               Tag,
               Session->MaximumPayload,
               Session->FooterSize,
               Session->TimeoutSeconds,
               FrameBuffer,
               FrameBufferCapacity,
               Frame,
               FrameLength
               );
    if (EFI_ERROR (Status)) {
      Session->Ready = FALSE;
      return Status;
    }

    if ((Frame->Sequence == 0) ||
        (Session->LastIncomingSequence == MAX_UINT64))
    {
      Session->Ready = FALSE;
      return Frame->Sequence == 0 ?
             EFI_COMPROMISED_DATA : EFI_BAD_BUFFER_SIZE;
    }

    ExpectedSequence = Session->LastIncomingSequence + 1;
    if (Frame->Sequence <= Session->LastIncomingSequence) {
      Status = CephMsgrV1SessionSendAck (
                 Session,
                 Session->LastIncomingSequence
                 );
      if (EFI_ERROR (Status)) {
        Session->Ready = FALSE;
        return Status;
      }

      continue;
    }

    if (Frame->Sequence != ExpectedSequence) {
      Session->Ready = FALSE;
      return EFI_COMPROMISED_DATA;
    }

    Session->LastIncomingSequence = Frame->Sequence;
    Status = CephMsgrV1SessionSendAck (
               Session,
               Session->LastIncomingSequence
               );
    if (EFI_ERROR (Status)) {
      Session->Ready = FALSE;
      return Status;
    }

    return EFI_SUCCESS;
  }

  return EFI_CONNECTION_FIN;
}
