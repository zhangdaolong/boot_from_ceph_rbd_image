#include <Uefi.h>
#include <Protocol/ServiceBinding.h>
#include <Protocol/Tcp4.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include "CephRbdTcp4.h"

#define CEPH_RBD_TCP4_TIMEOUT_TICKS_PER_SECOND  10000000ULL

STATIC
VOID
EFIAPI
CephRbdTcp4EventNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  (VOID)Event;
  (VOID)Context;
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
  OUT EFI_EVENT  *Event
  )
{
  if (Event == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return gBS->CreateEvent (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                CephRbdTcp4EventNotify,
                NULL,
                Event
                );
}

STATIC
EFI_STATUS
CephRbdTcp4WaitForCompletion (
  IN EFI_TCP4_PROTOCOL          *Tcp4,
  IN EFI_TCP4_COMPLETION_TOKEN  *Token,
  IN UINT32                     TimeoutSeconds
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  CancelStatus;
  EFI_EVENT   TimeoutEvent;
  EFI_EVENT   Events[2];
  UINTN       EventIndex;
  EFI_STATUS  TimeoutStatus;

  if ((Tcp4 == NULL) || (Token == NULL) || (Token->Event == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CephRbdTcp4CreateTimeoutEvent (TimeoutSeconds, &TimeoutEvent);
  if (EFI_ERROR (Status)) {
    TimeoutStatus = Status;
    CancelStatus = Tcp4->Cancel (Tcp4, Token);
    Events[0] = Token->Event;
    Status = gBS->WaitForEvent (1, Events, &EventIndex);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (CancelStatus == EFI_NOT_FOUND) {
      return Token->Status;
    }

    return EFI_ERROR (CancelStatus) ? CancelStatus : TimeoutStatus;
  }

  Events[0] = Token->Event;
  Events[1] = TimeoutEvent;
  Status = gBS->WaitForEvent (2, Events, &EventIndex);
  if (EFI_ERROR (Status)) {
    Tcp4->Cancel (Tcp4, Token);
    Events[0] = Token->Event;
    gBS->WaitForEvent (1, Events, &EventIndex);
    gBS->CloseEvent (TimeoutEvent);
    return Status;
  }

  if (EventIndex == 1) {
    CancelStatus = Tcp4->Cancel (Tcp4, Token);
    if (CancelStatus == EFI_NOT_FOUND) {
      Status = gBS->CheckEvent (Token->Event);
      if (!EFI_ERROR (Status)) {
        Status = Token->Status;
        gBS->CloseEvent (TimeoutEvent);
        return Status;
      }
    }

    Events[0] = Token->Event;
    Status = gBS->WaitForEvent (1, Events, &EventIndex);
    if (!EFI_ERROR (Status)) {
      if (CancelStatus == EFI_NOT_FOUND) {
        Status = Token->Status;
      } else {
        Status = EFI_ERROR (CancelStatus) ?
                 CancelStatus : EFI_TIMEOUT;
      }
    }
  } else {
    Status = Token->Status;
  }

  gBS->CloseEvent (TimeoutEvent);
  return Status;
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
CephRbdTcp4Connect (
  IN EFI_HANDLE                 ImageHandle,
  IN EFI_HANDLE                 ControllerHandle,
  IN CONST EFI_IPv4_ADDRESS     *RemoteAddress,
  IN UINT16                     RemotePort,
  IN UINT32                     TimeoutSeconds,
  OUT CEPH_RBD_TCP4_CONNECTION *Connection
  )
{
  EFI_STATUS                 Status;
  EFI_STATUS                 CleanupStatus;
  EFI_TCP4_CONFIG_DATA       Config;
  EFI_TCP4_CONNECTION_TOKEN  Token;

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
  Config.AccessPoint.UseDefaultAddress = TRUE;
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
  Status = CephRbdTcp4CreateCompletionEvent (&Token.CompletionToken.Event);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Status = Connection->Tcp4->Connect (Connection->Tcp4, &Token);
  if (!EFI_ERROR (Status)) {
    Connection->Connected = TRUE;
    Status = CephRbdTcp4WaitForCompletion (
               Connection->Tcp4,
               &Token.CompletionToken,
               TimeoutSeconds
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
CephRbdTcp4Send (
  IN CEPH_RBD_TCP4_CONNECTION  *Connection,
  IN CONST VOID                *Buffer,
  IN UINTN                     BufferLength,
  IN UINT32                    TimeoutSeconds
  )
{
  EFI_STATUS              Status;
  EFI_TCP4_IO_TOKEN       Token;
  EFI_TCP4_TRANSMIT_DATA  TxData;

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
  Status = CephRbdTcp4CreateCompletionEvent (&Token.CompletionToken.Event);
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
               TimeoutSeconds
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
  EFI_STATUS            Status;
  EFI_TCP4_IO_TOKEN     Token;
  EFI_TCP4_RECEIVE_DATA RxData;

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
  Status = CephRbdTcp4CreateCompletionEvent (&Token.CompletionToken.Event);
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
               TimeoutSeconds
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
  EFI_STATUS            Status;
  EFI_STATUS            FirstError;
  EFI_TCP4_CLOSE_TOKEN  Token;

  if ((Connection == NULL) || (TimeoutSeconds == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  FirstError = EFI_SUCCESS;
  if ((Connection->Tcp4 != NULL) && Connection->Connected) {
    ZeroMem (&Token, sizeof (Token));
    Token.AbortOnClose = TRUE;
    Status = CephRbdTcp4CreateCompletionEvent (&Token.CompletionToken.Event);
    if (!EFI_ERROR (Status)) {
      Status = Connection->Tcp4->Close (Connection->Tcp4, &Token);
      if (!EFI_ERROR (Status)) {
        Status = CephRbdTcp4WaitForCompletion (
                   Connection->Tcp4,
                   &Token.CompletionToken,
                   TimeoutSeconds
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
