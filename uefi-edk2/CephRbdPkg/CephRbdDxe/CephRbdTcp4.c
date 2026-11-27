#include <Uefi.h>
#include <Protocol/ServiceBinding.h>
#include <Protocol/Tcp4.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include "CephRbdTcp4.h"

#define CEPH_RBD_TCP4_TIMEOUT_TICKS_PER_SECOND  10000000ULL

// EFI_TCP4 completion tokens require an EVT_NOTIFY_SIGNAL event. Such an
// event cannot be passed to gBS->WaitForEvent() (CoreCheckEvent rejects
// EVT_NOTIFY_SIGNAL with EFI_INVALID_PARAMETER). Instead the notification
// function flips a per-operation flag and the caller drives the TCP state
// machine with Tcp4->Poll() until the flag is set or a timer expires. This is
// the same pattern used by EDK II's own NetworkPkg (e.g. HttpDxe).
typedef struct {
  BOOLEAN  IsDone;
} CEPH_RBD_TCP4_COMPLETION_CONTEXT;

STATIC
VOID
EFIAPI
CephRbdTcp4EventNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  CEPH_RBD_TCP4_COMPLETION_CONTEXT  *Completion;

  (VOID)Event;
  if (Context != NULL) {
    Completion = (CEPH_RBD_TCP4_COMPLETION_CONTEXT *)Context;
    Completion->IsDone = TRUE;
  }
}

STATIC
EFI_STATUS
CephRbdTcp4CreateTimeoutEvent (
  IN UINT32     TimeoutSeconds,
  OUT EFI_EVENT *TimeoutEvent
  )
{
  EFI_STATUS  Status;

  if ((TimeoutSeconds == 0) || (TimeoutEvent == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *TimeoutEvent = NULL;
  Status = gBS->CreateEvent (
                  EVT_TIMER,
                  TPL_APPLICATION,
                  NULL,
                  NULL,
                  TimeoutEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->SetTimer (
                  *TimeoutEvent,
                  TimerRelative,
                  (UINT64)TimeoutSeconds *
                  CEPH_RBD_TCP4_TIMEOUT_TICKS_PER_SECOND
                  );
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (*TimeoutEvent);
    *TimeoutEvent = NULL;
  }

  return Status;
}

STATIC
EFI_STATUS
CephRbdTcp4CreateCompletionEvent (
  OUT EFI_EVENT  *Event,
  IN  VOID       *Context
  )
{
  if (Event == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return gBS->CreateEvent (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                CephRbdTcp4EventNotify,
                Context,
                Event
                );
}

STATIC
EFI_STATUS
CephRbdTcp4WaitForCompletion (
  IN EFI_TCP4_PROTOCOL               *Tcp4,
  IN EFI_TCP4_COMPLETION_TOKEN       *Token,
  IN UINT32                          TimeoutSeconds,
  IN CEPH_RBD_TCP4_COMPLETION_CONTEXT *Context
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   TimeoutEvent;

  if ((Tcp4 == NULL) || (Token == NULL) || (Token->Event == NULL) ||
      (Context == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephRbdTcp4CreateTimeoutEvent (TimeoutSeconds, &TimeoutEvent);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Drive the TCP state machine until the completion notification fires or
  // the timer expires. gBS->WaitForEvent() cannot be used here because the
  // completion token event is EVT_NOTIFY_SIGNAL (see comment above).
  while (!Context->IsDone) {
    if (!EFI_ERROR (gBS->CheckEvent (TimeoutEvent))) {
      Tcp4->Cancel (Tcp4, Token);
      gBS->CloseEvent (TimeoutEvent);
      return EFI_TIMEOUT;
    }

    Tcp4->Poll (Tcp4);
  }

  gBS->CloseEvent (TimeoutEvent);
  return Token->Status;
}

STATIC
VOID
CephRbdTcp4RecordCleanupError (
  IN OUT EFI_STATUS  *FirstError,
  IN EFI_STATUS      Status
  )
{
  if (!EFI_ERROR (*FirstError) && EFI_ERROR (Status)) {
    *FirstError = Status;
  }
}

EFI_STATUS
CephRbdTcp4ConnectEx (
  IN EFI_HANDLE                 ImageHandle,
  IN EFI_HANDLE                 ControllerHandle,
  IN CONST EFI_IPv4_ADDRESS     *RemoteAddress,
  IN UINT16                     RemotePort,
  IN UINT32                     TimeoutSeconds,
  IN CONST CEPH_RBD_LOCAL_IP_CONFIG  *LocalIp OPTIONAL,
  OUT CEPH_RBD_TCP4_CONNECTION *Connection
  )
{
  EFI_STATUS                          Status;
  EFI_STATUS                          CleanupStatus;
  EFI_TCP4_CONFIG_DATA                Config;
  EFI_TCP4_CONNECTION_TOKEN           Token;
  CEPH_RBD_TCP4_COMPLETION_CONTEXT     CompletionCtx;

  if ((ImageHandle == NULL) || (ControllerHandle == NULL) ||
      (RemoteAddress == NULL) || (RemotePort == 0) ||
      (TimeoutSeconds == 0) || (Connection == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Connection, sizeof (*Connection));
  Connection->ImageHandle      = ImageHandle;
  Connection->ControllerHandle = ControllerHandle;
  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiTcp4ServiceBindingProtocolGuid,
                  (VOID **)&Connection->ServiceBinding,
                  ImageHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Connection->ServiceBindingOpen = TRUE;
  if (Connection->ServiceBinding == NULL) {
    Status = EFI_DEVICE_ERROR;
    goto Error;
  }

  Status = Connection->ServiceBinding->CreateChild (
                                         Connection->ServiceBinding,
                                         &Connection->ChildHandle
                                         );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Status = gBS->OpenProtocol (
                  Connection->ChildHandle,
                  &gEfiTcp4ProtocolGuid,
                  (VOID **)&Connection->Tcp4,
                  ImageHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Connection->ProtocolOpen = TRUE;
  if (Connection->Tcp4 == NULL) {
    Status = EFI_DEVICE_ERROR;
    goto Error;
  }

  ZeroMem (&Config, sizeof (Config));
  if (LocalIp == NULL) {
    Config.AccessPoint.UseDefaultAddress = TRUE;
  } else {
    Config.AccessPoint.UseDefaultAddress = LocalIp->UseDefaultAddress;
    if (!LocalIp->UseDefaultAddress) {
      CopyMem (
        &Config.AccessPoint.StationAddress,
        &LocalIp->StationAddress,
        sizeof (EFI_IPv4_ADDRESS)
        );
      CopyMem (
        &Config.AccessPoint.SubnetMask,
        &LocalIp->SubnetMask,
        sizeof (EFI_IPv4_ADDRESS)
        );
    }
  }
  CopyMem (
    &Config.AccessPoint.RemoteAddress,
    RemoteAddress,
    sizeof (*RemoteAddress)
    );
  Config.AccessPoint.RemotePort = RemotePort;
  Config.AccessPoint.ActiveFlag = TRUE;
  Config.TimeToLive = 64;

  Status = Connection->Tcp4->Configure (Connection->Tcp4, &Config);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Connection->Configured = TRUE;
  ZeroMem (&Token, sizeof (Token));
  CompletionCtx.IsDone = FALSE;
  Status = CephRbdTcp4CreateCompletionEvent (
             &Token.CompletionToken.Event,
             &CompletionCtx
             );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Status = Connection->Tcp4->Connect (Connection->Tcp4, &Token);
  if (!EFI_ERROR (Status)) {
    Connection->Connected = TRUE;
    Status = CephRbdTcp4WaitForCompletion (
               Connection->Tcp4,
               &Token.CompletionToken,
               TimeoutSeconds,
               &CompletionCtx
               );
  }

  gBS->CloseEvent (Token.CompletionToken.Event);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  return EFI_SUCCESS;

Error:
  CleanupStatus = CephRbdTcp4Close (Connection, TimeoutSeconds);
  if (EFI_ERROR (CleanupStatus)) {
    DEBUG ((
      DEBUG_ERROR,
      "CephRbdTcp4: cleanup after connect failure failed: %r\n",
      CleanupStatus
      ));
  }

  return Status;
}

EFI_STATUS
CephRbdTcp4Connect (
  IN EFI_HANDLE                 ImageHandle,
  IN EFI_HANDLE                 ControllerHandle,
  IN CONST EFI_IPv4_ADDRESS     *RemoteAddress,
  IN UINT16                     RemotePort,
  IN UINT32                     TimeoutSeconds,
  OUT CEPH_RBD_TCP4_CONNECTION *Connection
  )
{
  return CephRbdTcp4ConnectEx (
           ImageHandle,
           ControllerHandle,
           RemoteAddress,
           RemotePort,
           TimeoutSeconds,
           NULL,
           Connection
           );
}

EFI_STATUS
CephRbdTcp4Send (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN CONST VOID                *Buffer,
  IN UINTN                     BufferLength,
  IN UINT32                    TimeoutSeconds
  )
{
  EFI_STATUS                          Status;
  EFI_TCP4_IO_TOKEN                   Token;
  EFI_TCP4_TRANSMIT_DATA              TxData;
  CEPH_RBD_TCP4_COMPLETION_CONTEXT    CompletionCtx;

  if ((Connection == NULL) || (Buffer == NULL) ||
      (BufferLength == 0) || (BufferLength > MAX_UINT32) ||
      (TimeoutSeconds == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (!Connection->Connected || (Connection->Tcp4 == NULL)) {
    return EFI_NOT_STARTED;
  }

  ZeroMem (&Token, sizeof (Token));
  ZeroMem (&TxData, sizeof (TxData));
  CompletionCtx.IsDone = FALSE;
  Status = CephRbdTcp4CreateCompletionEvent (
             &Token.CompletionToken.Event,
             &CompletionCtx
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  TxData.Push                            = TRUE;
  TxData.DataLength                      = (UINT32)BufferLength;
  TxData.FragmentCount                   = 1;
  TxData.FragmentTable[0].FragmentLength = (UINT32)BufferLength;
  TxData.FragmentTable[0].FragmentBuffer = (VOID *)Buffer;
  Token.Packet.TxData = &TxData;

  Status = Connection->Tcp4->Transmit (Connection->Tcp4, &Token);
  if (!EFI_ERROR (Status)) {
    Status = CephRbdTcp4WaitForCompletion (
               Connection->Tcp4,
               &Token.CompletionToken,
               TimeoutSeconds,
               &CompletionCtx
               );
  }

  gBS->CloseEvent (Token.CompletionToken.Event);
  return Status;
}

EFI_STATUS
CephRbdTcp4Receive (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  OUT VOID                    *Buffer,
  IN UINTN                     BufferCapacity,
  IN UINT32                    TimeoutSeconds,
  OUT UINTN                    *ReceivedLength
  )
{
  EFI_STATUS                          Status;
  EFI_TCP4_IO_TOKEN                   Token;
  EFI_TCP4_RECEIVE_DATA               RxData;
  CEPH_RBD_TCP4_COMPLETION_CONTEXT    CompletionCtx;

  if ((Connection == NULL) || (Buffer == NULL) ||
      (BufferCapacity == 0) || (BufferCapacity > MAX_UINT32) ||
      (TimeoutSeconds == 0) || (ReceivedLength == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  *ReceivedLength = 0;
  if (!Connection->Connected || (Connection->Tcp4 == NULL)) {
    return EFI_NOT_STARTED;
  }

  ZeroMem (&Token, sizeof (Token));
  ZeroMem (&RxData, sizeof (RxData));
  CompletionCtx.IsDone = FALSE;
  Status = CephRbdTcp4CreateCompletionEvent (
             &Token.CompletionToken.Event,
             &CompletionCtx
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  RxData.DataLength                      = (UINT32)BufferCapacity;
  RxData.FragmentCount                   = 1;
  RxData.FragmentTable[0].FragmentLength = (UINT32)BufferCapacity;
  RxData.FragmentTable[0].FragmentBuffer = Buffer;
  Token.Packet.RxData = &RxData;

  Status = Connection->Tcp4->Receive (Connection->Tcp4, &Token);
  if (!EFI_ERROR (Status)) {
    Status = CephRbdTcp4WaitForCompletion (
               Connection->Tcp4,
               &Token.CompletionToken,
               TimeoutSeconds,
               &CompletionCtx
               );
  }

  if (!EFI_ERROR (Status)) {
    if (RxData.DataLength > BufferCapacity) {
      Status = EFI_COMPROMISED_DATA;
    } else {
      *ReceivedLength = RxData.DataLength;
    }
  }

  gBS->CloseEvent (Token.CompletionToken.Event);
  return Status;
}

EFI_STATUS
CephRbdTcp4ReceiveExact (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  OUT VOID                     *Buffer,
  IN UINTN                     BufferLength,
  IN UINT32                    TimeoutSeconds
  )
{
  EFI_STATUS  Status;
  UINTN       ReceivedLength;
  UINTN       ReceiveCapacity;
  UINTN       Offset;

  if ((Connection == NULL) || (Buffer == NULL) ||
      (BufferLength == 0) || (TimeoutSeconds == 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Offset = 0;
  while (Offset < BufferLength) {
    ReceiveCapacity = BufferLength - Offset;
    if (ReceiveCapacity > MAX_UINT32) {
      ReceiveCapacity = MAX_UINT32;
    }

    ReceivedLength = 0;
    Status = CephRbdTcp4Receive (
               Connection,
               (UINT8 *)Buffer + Offset,
               ReceiveCapacity,
               TimeoutSeconds,
               &ReceivedLength
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (ReceivedLength == 0) {
      return EFI_CONNECTION_FIN;
    }

    if (ReceivedLength > ReceiveCapacity) {
      return EFI_COMPROMISED_DATA;
    }

    Offset += ReceivedLength;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdTcp4Close (
  IN OUT CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN UINT32                        TimeoutSeconds
  )
{
  EFI_STATUS                          Status;
  EFI_STATUS                          FirstError;
  EFI_TCP4_CLOSE_TOKEN                Token;
  CEPH_RBD_TCP4_COMPLETION_CONTEXT    CompletionCtx;

  if ((Connection == NULL) || (TimeoutSeconds == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  FirstError = EFI_SUCCESS;
  if ((Connection->Tcp4 != NULL) && Connection->Connected) {
    ZeroMem (&Token, sizeof (Token));
    Token.AbortOnClose = TRUE;
    CompletionCtx.IsDone = FALSE;
    Status = CephRbdTcp4CreateCompletionEvent (
               &Token.CompletionToken.Event,
               &CompletionCtx
               );
    if (!EFI_ERROR (Status)) {
      Status = Connection->Tcp4->Close (Connection->Tcp4, &Token);
      if (!EFI_ERROR (Status)) {
        Status = CephRbdTcp4WaitForCompletion (
                   Connection->Tcp4,
                   &Token.CompletionToken,
                   TimeoutSeconds,
                   &CompletionCtx
                   );
      }

      gBS->CloseEvent (Token.CompletionToken.Event);
    }

    CephRbdTcp4RecordCleanupError (&FirstError, Status);
    if (EFI_ERROR (Status)) {
      return FirstError;
    }

    Connection->Connected = FALSE;
  }

  if ((Connection->Tcp4 != NULL) && Connection->Configured) {
    Status = Connection->Tcp4->Configure (Connection->Tcp4, NULL);
    CephRbdTcp4RecordCleanupError (&FirstError, Status);
    if (EFI_ERROR (Status)) {
      return FirstError;
    }

    Connection->Configured = FALSE;
    Connection->Connected  = FALSE;
  }

  if (Connection->ProtocolOpen) {
    Status = gBS->CloseProtocol (
                    Connection->ChildHandle,
                    &gEfiTcp4ProtocolGuid,
                    Connection->ImageHandle,
                    Connection->ControllerHandle
                    );
    CephRbdTcp4RecordCleanupError (&FirstError, Status);
    if (EFI_ERROR (Status)) {
      return FirstError;
    }

    Connection->ProtocolOpen = FALSE;
  }

  if ((Connection->ChildHandle != NULL) &&
      (Connection->ServiceBinding == NULL))
  {
    return EFI_COMPROMISED_DATA;
  }

  if (Connection->ChildHandle != NULL) {
    Status = Connection->ServiceBinding->DestroyChild (
                                           Connection->ServiceBinding,
                                           Connection->ChildHandle
                                           );
    CephRbdTcp4RecordCleanupError (&FirstError, Status);
    if (EFI_ERROR (Status)) {
      return FirstError;
    }

    Connection->ChildHandle = NULL;
    Connection->Tcp4 = NULL;
    Connection->Configured = FALSE;
    Connection->Connected = FALSE;
    Connection->ProtocolOpen = FALSE;
  }

  if (Connection->ServiceBindingOpen) {
    Status = gBS->CloseProtocol (
                    Connection->ControllerHandle,
                    &gEfiTcp4ServiceBindingProtocolGuid,
                    Connection->ImageHandle,
                    Connection->ControllerHandle
                    );
    CephRbdTcp4RecordCleanupError (&FirstError, Status);
    if (EFI_ERROR (Status)) {
      return FirstError;
    }

    Connection->ServiceBindingOpen = FALSE;
  }

  ZeroMem (Connection, sizeof (*Connection));
  return FirstError;
}
