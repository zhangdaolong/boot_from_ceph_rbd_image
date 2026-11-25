#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <Library/BaseMemoryLib.h>
#include <Protocol/ServiceBinding.h>
#include <Protocol/Tcp4.h>
#include "CephRbdTcp4.h"

typedef struct {
  UINT32 Type;
  BOOLEAN Signaled;
  EFI_EVENT_NOTIFY NotifyFunction;
  VOID *NotifyContext;
} TEST_EVENT;

static int mController;
static int mChild;
static int mImage;
static EFI_TCP4_ACCESS_POINT mAccessPoint;
static UINTN mCreateChildCount;
static UINTN mDestroyChildCount;
static UINTN mCloseProtocolCount;
static UINTN mSendCount;
static UINTN mReceiveCount;
static UINTN mCloseCount;
static BOOLEAN mConfigured;
static BOOLEAN mTransmitValid;
static BOOLEAN mFailChildProtocolClose;

EFI_GUID gEfiTcp4ServiceBindingProtocolGuid = { 1, 0, 0, { 0 } };
EFI_GUID gEfiTcp4ProtocolGuid = { 2, 0, 0, { 0 } };

static EFI_STATUS EFIAPI FakeCreateChild(
  EFI_SERVICE_BINDING_PROTOCOL *This,
  EFI_HANDLE *ChildHandle
  );
static EFI_STATUS EFIAPI FakeDestroyChild(
  EFI_SERVICE_BINDING_PROTOCOL *This,
  EFI_HANDLE ChildHandle
  );
static EFI_STATUS EFIAPI FakeConfigure(
  EFI_TCP4_PROTOCOL *This,
  EFI_TCP4_CONFIG_DATA *Config
  );
static EFI_STATUS EFIAPI FakeConnect(
  EFI_TCP4_PROTOCOL *This,
  EFI_TCP4_CONNECTION_TOKEN *Token
  );
static EFI_STATUS EFIAPI FakeTransmit(
  EFI_TCP4_PROTOCOL *This,
  EFI_TCP4_IO_TOKEN *Token
  );
static EFI_STATUS EFIAPI FakeReceive(
  EFI_TCP4_PROTOCOL *This,
  EFI_TCP4_IO_TOKEN *Token
  );
static EFI_STATUS EFIAPI FakeClose(
  EFI_TCP4_PROTOCOL *This,
  EFI_TCP4_CLOSE_TOKEN *Token
  );
static EFI_STATUS EFIAPI FakeCancel(
  EFI_TCP4_PROTOCOL *This,
  EFI_TCP4_COMPLETION_TOKEN *Token
  );

static EFI_SERVICE_BINDING_PROTOCOL mServiceBinding = {
  FakeCreateChild,
  FakeDestroyChild
};

static EFI_TCP4_PROTOCOL mTcp4 = {
  FakeConfigure,
  FakeConnect,
  FakeTransmit,
  FakeReceive,
  FakeClose,
  FakeCancel
};

static EFI_STATUS EFIAPI FakeCreateEvent(
  UINT32 Type,
  EFI_TPL NotifyTpl,
  EFI_EVENT_NOTIFY NotifyFunction,
  VOID *NotifyContext,
  EFI_EVENT *Event
  );
static EFI_STATUS EFIAPI FakeSetTimer(
  EFI_EVENT Event,
  EFI_TIMER_DELAY Type,
  UINT64 TriggerTime
  );
static EFI_STATUS EFIAPI FakeWaitForEvent(
  UINTN NumberOfEvents,
  EFI_EVENT *Events,
  UINTN *Index
  );
static EFI_STATUS EFIAPI FakeCheckEvent(EFI_EVENT Event);
static EFI_STATUS EFIAPI FakeCloseEvent(EFI_EVENT Event);
static EFI_STATUS EFIAPI FakeOpenProtocol(
  EFI_HANDLE Handle,
  CONST EFI_GUID *Protocol,
  VOID **Interface,
  EFI_HANDLE AgentHandle,
  EFI_HANDLE ControllerHandle,
  UINT32 Attributes
  );
static EFI_STATUS EFIAPI FakeCloseProtocol(
  EFI_HANDLE Handle,
  CONST EFI_GUID *Protocol,
  EFI_HANDLE AgentHandle,
  EFI_HANDLE ControllerHandle
  );

static EFI_BOOT_SERVICES mBootServices = {
  FakeCreateEvent,
  FakeSetTimer,
  FakeWaitForEvent,
  FakeCheckEvent,
  FakeCloseEvent,
  FakeOpenProtocol,
  FakeCloseProtocol
};

EFI_BOOT_SERVICES *gBS = &mBootServices;

static void
SignalToken(EFI_TCP4_COMPLETION_TOKEN *Token, EFI_STATUS Status)
{
  TEST_EVENT *Event = Token->Event;

  Token->Status = Status;
  Event->Signaled = TRUE;
  if (Event->NotifyFunction != NULL) {
    Event->NotifyFunction(Token->Event, Event->NotifyContext);
  }
}

static EFI_STATUS EFIAPI
FakeCreateChild(EFI_SERVICE_BINDING_PROTOCOL *This, EFI_HANDLE *ChildHandle)
{
  assert(This == &mServiceBinding);
  assert(ChildHandle != NULL);
  *ChildHandle = &mChild;
  mCreateChildCount++;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeDestroyChild(EFI_SERVICE_BINDING_PROTOCOL *This, EFI_HANDLE ChildHandle)
{
  assert(This == &mServiceBinding);
  assert(ChildHandle == &mChild);
  mDestroyChildCount++;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeConfigure(EFI_TCP4_PROTOCOL *This, EFI_TCP4_CONFIG_DATA *Config)
{
  assert(This == &mTcp4);
  if (Config == NULL) {
    mConfigured = FALSE;
    return EFI_SUCCESS;
  }

  assert(Config->AccessPoint.UseDefaultAddress == TRUE);
  assert(Config->AccessPoint.RemotePort == 6789);
  assert(Config->AccessPoint.RemoteAddress.Addr[0] == 10);
  assert(Config->AccessPoint.RemoteAddress.Addr[3] == 7);
  assert(Config->AccessPoint.ActiveFlag == TRUE);
  mAccessPoint = Config->AccessPoint;
  mConfigured = TRUE;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeConnect(EFI_TCP4_PROTOCOL *This, EFI_TCP4_CONNECTION_TOKEN *Token)
{
  assert(This == &mTcp4);
  assert(mConfigured);
  SignalToken(&Token->CompletionToken, EFI_SUCCESS);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeTransmit(EFI_TCP4_PROTOCOL *This, EFI_TCP4_IO_TOKEN *Token)
{
  EFI_TCP4_TRANSMIT_DATA *TxData;

  assert(This == &mTcp4);
  TxData = Token->Packet.TxData;
  assert(TxData != NULL);
  assert(TxData->Push == TRUE);
  assert(TxData->FragmentCount == 1);
  assert(TxData->DataLength == 4);
  assert(TxData->FragmentTable[0].FragmentLength == 4);
  mTransmitValid = (BOOLEAN)(
                     memcmp (
                       TxData->FragmentTable[0].FragmentBuffer,
                       "wire",
                       4
                       ) == 0);
  mSendCount++;
  SignalToken(&Token->CompletionToken, EFI_SUCCESS);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeReceive(EFI_TCP4_PROTOCOL *This, EFI_TCP4_IO_TOKEN *Token)
{
  EFI_TCP4_RECEIVE_DATA *RxData;

  assert(This == &mTcp4);
  RxData = Token->Packet.RxData;
  assert(RxData != NULL);
  assert(RxData->FragmentCount == 1);
  assert(RxData->FragmentTable[0].FragmentLength >= 3);
  CopyMem (RxData->FragmentTable[0].FragmentBuffer, "ack", 3);
  RxData->DataLength = 3;
  RxData->FragmentTable[0].FragmentLength = 3;
  mReceiveCount++;
  SignalToken(&Token->CompletionToken, EFI_SUCCESS);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeClose(EFI_TCP4_PROTOCOL *This, EFI_TCP4_CLOSE_TOKEN *Token)
{
  assert(This == &mTcp4);
  assert(Token->AbortOnClose);
  mCloseCount++;
  SignalToken(&Token->CompletionToken, EFI_SUCCESS);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeCancel(EFI_TCP4_PROTOCOL *This, EFI_TCP4_COMPLETION_TOKEN *Token)
{
  assert(This == &mTcp4);
  (void)Token;
  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI
FakeCreateEvent(
  UINT32 Type,
  EFI_TPL NotifyTpl,
  EFI_EVENT_NOTIFY NotifyFunction,
  VOID *NotifyContext,
  EFI_EVENT *Event
  )
{
  TEST_EVENT *Created;

  (void)NotifyTpl;
  Created = calloc (1, sizeof (*Created));
  if (Created == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Created->Type = Type;
  Created->NotifyFunction = NotifyFunction;
  Created->NotifyContext = NotifyContext;
  *Event = Created;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeSetTimer(EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 TriggerTime)
{
  TEST_EVENT *Timer = Event;

  assert(Timer->Type == EVT_TIMER);
  assert(Type == TimerRelative);
  assert(TriggerTime != 0);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeWaitForEvent(UINTN NumberOfEvents, EFI_EVENT *Events, UINTN *Index)
{
  UINTN EventNumber;

  for (EventNumber = 0; EventNumber < NumberOfEvents; EventNumber++) {
    TEST_EVENT *Event = Events[EventNumber];
    if (Event->Signaled) {
      Event->Signaled = FALSE;
      *Index = EventNumber;
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_READY;
}

static EFI_STATUS EFIAPI
FakeCheckEvent(EFI_EVENT Event)
{
  TEST_EVENT *Checked = Event;

  if (!Checked->Signaled) {
    return EFI_NOT_READY;
  }

  Checked->Signaled = FALSE;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeCloseEvent(EFI_EVENT Event)
{
  free (Event);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeOpenProtocol(
  EFI_HANDLE Handle,
  CONST EFI_GUID *Protocol,
  VOID **Interface,
  EFI_HANDLE AgentHandle,
  EFI_HANDLE ControllerHandle,
  UINT32 Attributes
  )
{
  (void)AgentHandle;
  (void)ControllerHandle;
  assert(Attributes == EFI_OPEN_PROTOCOL_GET_PROTOCOL);

  if ((Handle == &mController) &&
      (memcmp (Protocol, &gEfiTcp4ServiceBindingProtocolGuid,
               sizeof (*Protocol)) == 0))
  {
    *Interface = &mServiceBinding;
    return EFI_SUCCESS;
  }

  if ((Handle == &mChild) &&
      (memcmp (Protocol, &gEfiTcp4ProtocolGuid, sizeof (*Protocol)) == 0))
  {
    *Interface = &mTcp4;
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI
FakeCloseProtocol(
  EFI_HANDLE Handle,
  CONST EFI_GUID *Protocol,
  EFI_HANDLE AgentHandle,
  EFI_HANDLE ControllerHandle
  )
{
  (void)Protocol;
  (void)AgentHandle;
  (void)ControllerHandle;
  assert((Handle == &mController) || (Handle == &mChild));
  mCloseProtocolCount++;
  if ((Handle == &mChild) && mFailChildProtocolClose) {
    mFailChildProtocolClose = FALSE;
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

void
TestTcp4Transport(void)
{
  EFI_IPv4_ADDRESS RemoteAddress = { { 10, 0, 0, 7 } };
  CEPH_RBD_TCP4_CONNECTION Connection;
  UINT8 ReceiveBuffer[16] = { 0 };
  UINTN ReceivedLength = 0;

  ZeroMem (&Connection, sizeof (Connection));
  assert(CephRbdTcp4Connect(
           &mImage, &mController, &RemoteAddress, 6789, 2,
           &Connection) == EFI_SUCCESS);
  assert(Connection.Connected && Connection.Configured);
  assert(mCreateChildCount == 1);
  assert(mAccessPoint.RemotePort == 6789);

  assert(CephRbdTcp4Send(
           &Connection, "wire", 4, 2) == EFI_SUCCESS);
  assert(mSendCount == 1 && mTransmitValid);

  assert(CephRbdTcp4Receive(
           &Connection, ReceiveBuffer, sizeof (ReceiveBuffer),
           2, &ReceivedLength) == EFI_SUCCESS);
  assert(ReceivedLength == 3);
  assert(memcmp (ReceiveBuffer, "ack", 3) == 0);
  assert(mReceiveCount == 1);

  assert(CephRbdTcp4Close(&Connection, 2) == EFI_SUCCESS);
  assert(mCloseCount == 1);
  assert(!mConfigured);
  assert(mDestroyChildCount == 1);
  assert(mCloseProtocolCount == 2);

  ZeroMem (&Connection, sizeof (Connection));
  assert(CephRbdTcp4Connect(
           &mImage, &mController, &RemoteAddress, 6789, 2,
           &Connection) == EFI_SUCCESS);
  mFailChildProtocolClose = TRUE;
  assert(CephRbdTcp4Close(&Connection, 2) == EFI_DEVICE_ERROR);
  assert(Connection.ProtocolOpen);
  assert(Connection.ChildHandle == &mChild);
  assert(CephRbdTcp4Close(&Connection, 2) == EFI_SUCCESS);
  assert(mDestroyChildCount == 2);
  assert(mCloseProtocolCount == 5);
}
