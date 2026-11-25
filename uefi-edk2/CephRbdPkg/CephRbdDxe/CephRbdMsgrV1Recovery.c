#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include "CephRbdMsgrV1Recovery.h"
#include "CephRbdMsgrV1Transport.h"

STATIC
VOID
CephMsgrV1RecoveryWriteLe64 (
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
UINT64
CephMsgrV1RecoveryReadLe64 (
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

VOID
CephMsgrV1RetransmitQueueInit (
  OUT CEPH_MSGR_V1_RETRANSMIT_QUEUE  *Queue,
  IN UINT32                         MaxRetries  OPTIONAL
  )
{
  if (Queue == NULL) {
    return;
  }

  ZeroMem (Queue, sizeof (*Queue));
  Queue->MaxRetries = (MaxRetries != 0) ?
                      MaxRetries : CEPH_MSGR_V1_MAX_RPC_RETRIES;
}

EFI_STATUS
CephMsgrV1RetransmitQueueTrack (
  IN OUT CEPH_MSGR_V1_RETRANSMIT_QUEUE  *Queue,
  IN UINT64                             Sequence,
  IN CONST VOID                         *Frame,
  IN UINTN                              FrameLength
  )
{
  UINTN  Index;
  UINTN  Slot;

  if ((Queue == NULL) || (Frame == NULL) ||
      (FrameLength == 0) ||
      (FrameLength > CEPH_MSGR_V1_RETRANSMIT_FRAME_MAX) ||
      (Sequence == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Slot = Queue->Count;
  for (Index = 0; Index < Queue->Count; Index++) {
    if (!Queue->Entry[Index].Active) {
      Slot = Index;
      break;
    }
  }

  if (Slot >= CEPH_MSGR_V1_RETRANSMIT_MAX) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (&Queue->Entry[Slot], sizeof (Queue->Entry[Slot]));
  Queue->Entry[Slot].Sequence = Sequence;
  Queue->Entry[Slot].Length = FrameLength;
  Queue->Entry[Slot].Retries = 0;
  Queue->Entry[Slot].Active = TRUE;
  CopyMem (Queue->Entry[Slot].Buffer, Frame, FrameLength);
  if (Slot == Queue->Count) {
    Queue->Count++;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephMsgrV1RetransmitQueueAcknowledge (
  IN OUT CEPH_MSGR_V1_RETRANSMIT_QUEUE  *Queue,
  IN UINT64                             AcknowledgedSequence,
  OUT UINTN                             *PendingCount  OPTIONAL
  )
{
  UINTN  Index;

  if (Queue == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < Queue->Count; Index++) {
    if (Queue->Entry[Index].Active &&
        (Queue->Entry[Index].Sequence <= AcknowledgedSequence))
    {
      Queue->Entry[Index].Active = FALSE;
      ZeroMem (Queue->Entry[Index].Buffer, Queue->Entry[Index].Length);
      Queue->Entry[Index].Length = 0;
    }
  }

  while ((Queue->Count > 0) && !Queue->Entry[Queue->Count - 1].Active) {
    Queue->Count--;
  }

  if (PendingCount != NULL) {
    *PendingCount = CephMsgrV1RetransmitQueuePendingCount (
                      Queue,
                      AcknowledgedSequence
                      );
  }

  return EFI_SUCCESS;
}

UINTN
CephMsgrV1RetransmitQueuePendingCount (
  IN CONST CEPH_MSGR_V1_RETRANSMIT_QUEUE  *Queue,
  IN UINT64                               MinimumSequence
  )
{
  UINTN  Index;
  UINTN  Count;

  if (Queue == NULL) {
    return 0;
  }

  Count = 0;
  for (Index = 0; Index < Queue->Count; Index++) {
    if (Queue->Entry[Index].Active &&
        (Queue->Entry[Index].Sequence > MinimumSequence))
    {
      Count++;
    }
  }

  return Count;
}

BOOLEAN
CephMsgrV1RetransmitQueueExhausted (
  IN CONST CEPH_MSGR_V1_RETRANSMIT_QUEUE  *Queue,
  IN UINT64                               MinimumSequence
  )
{
  UINTN  Index;

  if (Queue == NULL) {
    return TRUE;
  }

  for (Index = 0; Index < Queue->Count; Index++) {
    if (Queue->Entry[Index].Active &&
        (Queue->Entry[Index].Sequence > MinimumSequence) &&
        (Queue->Entry[Index].Retries < Queue->MaxRetries))
    {
      return FALSE;
    }
  }

  return TRUE;
}

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
  )
{
  UINTN       Index;
  EFI_STATUS  Status;

  if ((Queue == NULL) || (Callback == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < Queue->Count; Index++) {
    if (!Queue->Entry[Index].Active ||
        (Queue->Entry[Index].Sequence <= MinimumSequence))
    {
      continue;
    }

    if (Queue->Entry[Index].Retries >= Queue->MaxRetries) {
      return EFI_TIMEOUT;
    }

    Status = Callback (
               Context,
               Queue->Entry[Index].Buffer,
               Queue->Entry[Index].Length,
               Queue->Entry[Index].Sequence
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Queue->Entry[Index].Retries++;
  }

  return EFI_SUCCESS;
}

typedef struct {
  CEPH_RBD_TCP4_CONNECTION  *Connection;
  UINT32                    TimeoutSeconds;
} CEPH_MSGR_V1_REPLAY_CONTEXT;

STATIC
EFI_STATUS
EFIAPI
CephMsgrV1ReplayFrame (
  IN VOID         *Context,
  IN CONST VOID   *Frame,
  IN UINTN        FrameLength,
  IN UINT64       Sequence
  )
{
  CEPH_MSGR_V1_REPLAY_CONTEXT  *Replay;

  (VOID)Sequence;
  if ((Context == NULL) || (Frame == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Replay = Context;
  return CephRbdTcp4Send (
           Replay->Connection,
           Frame,
           FrameLength,
           Replay->TimeoutSeconds
           );
}

typedef enum {
  CephRecoveryDone,
  CephRecoveryRetry,
  CephRecoverySeq,
  CephRecoveryNeedAuth,
  CephRecoveryFailed
} CEPH_RECOVERY_DISPOSITION;

STATIC
CEPH_RECOVERY_DISPOSITION
CephRbdMsgrV1ApplySequences (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT           *Client,
  IN CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG *Config,
  IN UINT32                                Attempt,
  OUT EFI_STATUS                           *Status
  )
{
  CEPH_RBD_TCP4_CONNECTION   *Connection;
  CEPH_MSGR_V1_NEGOTIATION   *Negotiation;

  (VOID)Attempt;
  Connection = &Client->Connection;
  Negotiation = &Client->Negotiation;
  *Status = EFI_SUCCESS;

  *Status = CephMsgrV1ClientHandshake (
             Connection,
             &Config->RemoteAddress,
             Config->RemotePort,
             Config->SupportedFeatures,
             Config->HostType,
             Client->GlobalSequence,
             Client->ConnectSequence,
             Config->ProtocolVersion,
             Config->AuthorizerProtocol,
             Config->Authorizer,
             Config->AuthorizerLength,
             Config->Flags,
             Config->RequiredFeatures,
             Config->TimeoutSeconds,
             Negotiation
             );
  if (EFI_ERROR (*Status)) {
    return CephRecoveryFailed;
  }

  switch (Negotiation->Action) {
    case CephMsgrConnectReady:
      *Status = CephMsgrV1SessionInit (
                  &Client->Session,
                  Connection,
                  Negotiation,
                  CEPH_MSGR_V1_RETRANSMIT_FRAME_MAX,
                  Config->TimeoutSeconds
                  );
      return EFI_ERROR (*Status) ? CephRecoveryFailed : CephRecoveryDone;

    case CephMsgrConnectRetrySession:
      if (Negotiation->ConnectReply.ConnectSequence <= Client->ConnectSequence) {
        *Status = EFI_COMPROMISED_DATA;
        return CephRecoveryFailed;
      }

      Client->ConnectSequence = Negotiation->ConnectReply.ConnectSequence;
      return CephRecoveryRetry;

    case CephMsgrConnectRetryGlobal:
      if (Negotiation->ConnectReply.GlobalSequence <= Client->GlobalSequence) {
        *Status = EFI_COMPROMISED_DATA;
        return CephRecoveryFailed;
      }

      Client->GlobalSequence = Negotiation->ConnectReply.GlobalSequence;
      Client->ConnectSequence = 0;
      return CephRecoveryRetry;

    case CephMsgrConnectResetSession:
      Client->ConnectSequence = 0;
      return CephRecoveryRetry;

    case CephMsgrConnectWait:
      // We cannot sleep in firmware without a timer service; treat WAIT as a
      // bounded retry so callers can observe progress in logs.
      DEBUG ((DEBUG_INFO,
              "CephRbdMsgrV1: monitor asked to wait (attempt %u)\n",
              Attempt));
      return CephRecoveryRetry;

    case CephMsgrConnectAckSequence:
      // Connect-time SEQ recovery is completed in CephRbdMsgrV1TryConnect
      // after the handshake reply has been consumed.
      return CephRecoverySeq;

    case CephMsgrConnectNeedAuthorizerChallenge:
      // The caller must complete CephX and retry; the challenge bytes are in
      // Negotiation->AuthorizerReply.
      return CephRecoveryNeedAuth;

    case CephMsgrConnectRejected:
    default:
      *Status = EFI_ACCESS_DENIED;
      return CephRecoveryFailed;
  }
}

STATIC
EFI_STATUS
CephRbdMsgrV1FinishSeqRecovery (
  IN CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG  *Config,
  IN OUT CEPH_RBD_MSGR_V1_CLIENT            *Client
  )
{
  EFI_STATUS                Status;
  UINT8                     AckWire[8];
  UINT64                    NewlyAckedSequence;
  UINT8                     InSeqWire[8];
  CEPH_MSGR_V1_REPLAY_CONTEXT  Replay;
  CEPH_MSGR_V1_SESSION      *Session;

  Status = CephRbdTcp4ReceiveExact (
             &Client->Connection,
             AckWire,
             sizeof (AckWire),
             Config->TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  NewlyAckedSequence = CephMsgrV1RecoveryReadLe64 (AckWire);
  if (NewlyAckedSequence > MAX_UINT64 - CEPH_MSGR_V1_MAX_IN_SEQ_GAP) {
    return EFI_COMPROMISED_DATA;
  }

  // Discard frames the peer has already received and prime our outgoing
  // sequence exactly as messenger v1 discard_requeued_up_to() does.
  Status = CephMsgrV1RetransmitQueueAcknowledge (
             &Client->Retransmit,
             NewlyAckedSequence,
             NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CephMsgrV1RecoveryWriteLe64 (
    InSeqWire,
    Client->Session.LastIncomingSequence
    );
  Status = CephRbdTcp4Send (
             &Client->Connection,
             InSeqWire,
             sizeof (InSeqWire),
             Config->TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // After the SEQ exchange the peer sends the final READY connect reply.
  Status = CephRbdTcp4ReceiveExact (
             &Client->Connection,
             AckWire,
             sizeof (AckWire),
             Config->TimeoutSeconds
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&Client->Negotiation, sizeof (Client->Negotiation));
  Client->Negotiation.ConnectReply.Tag = AckWire[0];
  Client->Negotiation.Action = CephMsgrConnectRejected;
  if (AckWire[0] != CEPH_MSGR_TAG_READY) {
    return EFI_ACCESS_DENIED;
  }

  Session = &Client->Session;
  ZeroMem (Session, sizeof (*Session));
  Session->Connection = &Client->Connection;
  Session->NextOutgoingSequence = NewlyAckedSequence + 1;
  Session->MaximumPayload = CEPH_MSGR_V1_RETRANSMIT_FRAME_MAX;
  Session->TimeoutSeconds = Config->TimeoutSeconds;
  Session->Ready = TRUE;
  Client->Negotiation.Ready = TRUE;
  Client->Negotiation.Action = CephMsgrConnectReady;

  Replay.Connection = &Client->Connection;
  Replay.TimeoutSeconds = Config->TimeoutSeconds;
  return CephMsgrV1RetransmitQueueForEachPending (
           &Client->Retransmit,
           NewlyAckedSequence,
           &Replay,
           CephMsgrV1ReplayFrame
           );
}

STATIC
CEPH_RECOVERY_DISPOSITION
CephRbdMsgrV1TryConnect (
  IN CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG  *Config,
  IN OUT CEPH_RBD_MSGR_V1_CLIENT            *Client,
  IN UINT32                                 Attempt,
  OUT EFI_STATUS                            *Status
  )
{
  EFI_STATUS                ConnectStatus;
  CEPH_RECOVERY_DISPOSITION  Disposition;

  *Status = EFI_SUCCESS;
  ConnectStatus = CephRbdTcp4Connect (
                    Config->ImageHandle,
                    Config->ControllerHandle,
                    &Config->RemoteAddress,
                    Config->RemotePort,
                    Config->TimeoutSeconds,
                    &Client->Connection
                    );
  if (EFI_ERROR (ConnectStatus)) {
    *Status = ConnectStatus;
    return CephRecoveryFailed;
  }

  Client->Connected = TRUE;
  Disposition = CephRbdMsgrV1ApplySequences (
                  Client,
                  Config,
                  Attempt,
                  &ConnectStatus
                  );
  if (Disposition == CephRecoverySeq) {
    ConnectStatus = CephRbdMsgrV1FinishSeqRecovery (Config, Client);
    Disposition = EFI_ERROR (ConnectStatus) ?
                  CephRecoveryFailed : CephRecoveryDone;
  } else if (Disposition == CephRecoveryNeedAuth) {
    ConnectStatus = EFI_ACCESS_DENIED;
    Disposition = CephRecoveryFailed;
  }

  if (EFI_ERROR (ConnectStatus)) {
    CephRbdTcp4Close (&Client->Connection, Config->TimeoutSeconds);
    Client->Connected = FALSE;
    Client->Session.Ready = FALSE;
    *Status = ConnectStatus;
    return CephRecoveryFailed;
  }

  *Status = ConnectStatus;
  return Disposition;
}

EFI_STATUS
CephRbdMsgrV1ClientConnect (
  IN CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG  *Config,
  OUT CEPH_RBD_MSGR_V1_CLIENT               *Client
  )
{
  EFI_STATUS                Status;
  CEPH_RECOVERY_DISPOSITION  Disposition;
  UINT32                    Attempt;
  UINT32                    MaxAttempts;

  if ((Config == NULL) || (Client == NULL) ||
      (Config->RemotePort == 0) || (Config->TimeoutSeconds == 0) ||
      (Config->ImageHandle == NULL) || (Config->ControllerHandle == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Client, sizeof (*Client));
  CephMsgrV1RetransmitQueueInit (&Client->Retransmit, 0);
  Client->GlobalSequence = 1;
  Client->ConnectSequence = 1;
  Client->ClientNonce = Config->ClientNonce;

  MaxAttempts = (Config->MaxConnectAttempts != 0) ?
                Config->MaxConnectAttempts : CEPH_MSGR_V1_MAX_CONNECT_ATTEMPTS;
  if (MaxAttempts > CEPH_MSGR_V1_MAX_CONNECT_ATTEMPTS) {
    MaxAttempts = CEPH_MSGR_V1_MAX_CONNECT_ATTEMPTS;
  }

  for (Attempt = 0; Attempt < MaxAttempts; Attempt++) {
    Disposition = CephRbdMsgrV1TryConnect (Config, Client, Attempt, &Status);
    if (Disposition == CephRecoveryDone) {
      return EFI_SUCCESS;
    }

    if (Disposition != CephRecoveryRetry) {
      return EFI_ERROR (Status) ? Status : EFI_ACCESS_DENIED;
    }

    CephRbdTcp4Close (&Client->Connection, Config->TimeoutSeconds);
    Client->Connected = FALSE;
    ZeroMem (&Client->Negotiation, sizeof (Client->Negotiation));
    DEBUG ((DEBUG_INFO,
            "CephRbdMsgrV1: reconnecting (attempt %u of %u)\n",
            Attempt + 1,
            MaxAttempts
            ));
  }

  return EFI_TIMEOUT;
}

EFI_STATUS
CephRbdMsgrV1ClientReconnect (
  IN CONST CEPH_RBD_MSGR_V1_CONNECT_CONFIG  *Config,
  IN OUT CEPH_RBD_MSGR_V1_CLIENT            *Client
  )
{
  EFI_STATUS                    Status;
  CEPH_RECOVERY_DISPOSITION     Disposition;
  CEPH_MSGR_V1_REPLAY_CONTEXT   Replay;
  UINTN                         RetryCount;
  UINT32                        Attempt;
  UINT32                        MaxAttempts;

  if ((Config == NULL) || (Client == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Client->Connected) {
    CephRbdTcp4Close (&Client->Connection, Config->TimeoutSeconds);
    Client->Connected = FALSE;
    Client->Session.Ready = FALSE;
  }

  Client->ConnectSequence++;
  MaxAttempts = (Config->MaxConnectAttempts != 0) ?
                Config->MaxConnectAttempts : CEPH_MSGR_V1_MAX_CONNECT_ATTEMPTS;
  if (MaxAttempts > CEPH_MSGR_V1_MAX_CONNECT_ATTEMPTS) {
    MaxAttempts = CEPH_MSGR_V1_MAX_CONNECT_ATTEMPTS;
  }

  for (Attempt = 0; Attempt < MaxAttempts; Attempt++) {
    Disposition = CephRbdMsgrV1TryConnect (Config, Client, Attempt, &Status);
    if (Disposition == CephRecoveryDone) {
      break;
    }

    if (Disposition != CephRecoveryRetry) {
      return EFI_ERROR (Status) ? Status : EFI_ACCESS_DENIED;
    }

    CephRbdTcp4Close (&Client->Connection, Config->TimeoutSeconds);
    Client->Connected = FALSE;
    ZeroMem (&Client->Negotiation, sizeof (Client->Negotiation));
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Replay everything the peer has not acknowledged yet.
  Replay.Connection = &Client->Connection;
  Replay.TimeoutSeconds = Config->TimeoutSeconds;
  Status = CephMsgrV1RetransmitQueueForEachPending (
             &Client->Retransmit,
             Client->Session.PeerAcknowledgedSequence,
             &Replay,
             CephMsgrV1ReplayFrame
             );
  if (EFI_ERROR (Status) && (Status != EFI_TIMEOUT)) {
    return Status;
  }

  RetryCount = 0;
  return CephMsgrV1RetransmitQueueAcknowledge (
           &Client->Retransmit,
           Client->Session.PeerAcknowledgedSequence,
           &RetryCount
           );
}

EFI_STATUS
CephRbdMsgrV1ClientSendTracked (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN CONST CEPH_MSGR_V1_FRAME     *Frame,
  OUT VOID                        *FrameBuffer,
  IN UINTN                        FrameBufferCapacity,
  OUT UINTN                       *FrameLength
  )
{
  EFI_STATUS  Status;

  if ((Client == NULL) || (Frame == NULL) ||
      (FrameBuffer == NULL) || (FrameLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephMsgrV1SessionSendMessage (
             &Client->Session,
             Frame,
             FrameBuffer,
             FrameBufferCapacity,
             FrameLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephMsgrV1RetransmitQueueTrack (
             &Client->Retransmit,
             Client->Session.NextOutgoingSequence - 1,
             FrameBuffer,
             *FrameLength
             );
  if (EFI_ERROR (Status)) {
    // Do not leave an untracked message "in flight": force reconnection so
    // connect-time SEQ recovery can reconcile the sequence gap.
    Client->Session.Ready = FALSE;
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdMsgrV1ClientReceiveMessage (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  OUT VOID                        *FrameBuffer,
  IN UINTN                        FrameBufferCapacity,
  OUT CEPH_MSGR_V1_FRAME          *Frame,
  OUT UINTN                       *FrameLength
  )
{
  EFI_STATUS  Status;

  if ((Client == NULL) || (FrameBuffer == NULL) ||
      (Frame == NULL) || (FrameLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephMsgrV1SessionReceiveMessage (
             &Client->Session,
             FrameBuffer,
             FrameBufferCapacity,
             Frame,
             FrameLength
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return CephMsgrV1RetransmitQueueAcknowledge (
           &Client->Retransmit,
           Client->Session.PeerAcknowledgedSequence,
           NULL
           );
}

EFI_STATUS
CephRbdMsgrV1ClientClose (
  IN OUT CEPH_RBD_MSGR_V1_CLIENT  *Client,
  IN UINT32                       TimeoutSeconds
  )
{
  EFI_STATUS  Status;

  if ((Client == NULL) || (TimeoutSeconds == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Client->Connected) {
    return EFI_SUCCESS;
  }

  Status = CephRbdTcp4Close (&Client->Connection, TimeoutSeconds);
  Client->Connected = FALSE;
  Client->Session.Ready = FALSE;
  ZeroMem (&Client->ConnectionSecret, sizeof (Client->ConnectionSecret));
  Client->ConnectionSecretLength = 0;
  return Status;
}
