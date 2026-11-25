#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <Library/BaseMemoryLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/ServiceBinding.h>
#include <Protocol/Tcp4.h>
#include "CephRbdMsgrV1Handshake.h"
#include "CephRbdMsgrV1Session.h"
#include "CephRbdMsgrV1Transport.h"
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
static UINT8 mSentBuffer[8192];
static UINTN mSentLength;
static CONST UINT8 *mReceiveScript;
static UINTN mReceiveScriptLength;
static UINTN mReceiveScriptOffset;
static UINTN mReceiveMaxChunk;

EFI_GUID gEfiTcp4ServiceBindingProtocolGuid = { 1, 0, 0, { 0 } };
EFI_GUID gEfiTcp4ProtocolGuid = { 2, 0, 0, { 0 } };
EFI_GUID gEfiBlockIoProtocolGuid = { 3, 0, 0, { 0 } };

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
static EFI_STATUS EFIAPI FakeInstallProtocolInterface(
  IN OUT EFI_HANDLE *Handle,
  IN CONST EFI_GUID *Protocol,
  IN UINTN InterfaceType,
  IN VOID *Interface
  );

static EFI_HANDLE mBlockIoHandle;

static EFI_BOOT_SERVICES   mBootServices = {
  FakeCreateEvent,
  FakeSetTimer,
  FakeWaitForEvent,
  FakeCheckEvent,
  FakeCloseEvent,
  FakeOpenProtocol,
  FakeCloseProtocol,
  FakeInstallProtocolInterface
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
  UINTN                   Length;

  assert(This == &mTcp4);
  TxData = Token->Packet.TxData;
  assert(TxData != NULL);
  assert(TxData->Push == TRUE);
  assert(TxData->FragmentCount == 1);
  assert(TxData->DataLength == TxData->FragmentTable[0].FragmentLength);
  Length = TxData->DataLength;
  assert(Length <= sizeof (mSentBuffer) - mSentLength);
  CopyMem (
    mSentBuffer + mSentLength,
    TxData->FragmentTable[0].FragmentBuffer,
    Length
    );
  mSentLength += Length;
  if ((Length == 4) &&
      (memcmp (TxData->FragmentTable[0].FragmentBuffer, "wire", 4) == 0))
  {
    mTransmitValid = TRUE;
  }

  mSendCount++;
  SignalToken(&Token->CompletionToken, EFI_SUCCESS);
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
FakeReceive(EFI_TCP4_PROTOCOL *This, EFI_TCP4_IO_TOKEN *Token)
{
  EFI_TCP4_RECEIVE_DATA *RxData;
  UINTN                 Length;

  assert(This == &mTcp4);
  RxData = Token->Packet.RxData;
  assert(RxData != NULL);
  assert(RxData->FragmentCount == 1);
  assert(RxData->FragmentTable[0].FragmentLength > 0);
  if (mReceiveScript == NULL) {
    Length = 3;
    CopyMem (RxData->FragmentTable[0].FragmentBuffer, "ack", Length);
  } else {
    Length = mReceiveScriptLength - mReceiveScriptOffset;
    if (Length > RxData->FragmentTable[0].FragmentLength) {
      Length = RxData->FragmentTable[0].FragmentLength;
    }

    if ((mReceiveMaxChunk != 0) && (Length > mReceiveMaxChunk)) {
      Length = mReceiveMaxChunk;
    }

    CopyMem (
      RxData->FragmentTable[0].FragmentBuffer,
      mReceiveScript + mReceiveScriptOffset,
      Length
      );
    mReceiveScriptOffset += Length;
  }

  RxData->DataLength = (UINT32)Length;
  RxData->FragmentTable[0].FragmentLength = (UINT32)Length;
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

static EFI_STATUS EFIAPI
FakeInstallProtocolInterface(
  IN OUT EFI_HANDLE *Handle,
  IN CONST EFI_GUID *Protocol,
  IN UINTN InterfaceType,
  IN VOID *Interface
  )
{
  (void)Protocol;
  (void)InterfaceType;
  (void)Interface;
  assert(Handle != NULL);
  mBlockIoHandle = (Handle != NULL) ? *Handle : NULL;
  return EFI_SUCCESS;
}

static EFI_STATUS
RunTestHandshake(
  CEPH_RBD_TCP4_CONNECTION *Connection,
  EFI_IPv4_ADDRESS *RemoteAddress,
  CEPH_MSGR_V1_NEGOTIATION *Negotiation
  )
{
  return CephMsgrV1ClientHandshake(
           Connection,
           RemoteAddress,
           6789,
           0x0F,
           CEPH_MSGR_CONNECT_HOST_CLIENT,
           1,
           6,
           1,
           0,
           NULL,
           0,
           0,
           0x05,
           2,
           Negotiation
           );
}

static void
WriteTestLe64(UINT8 *Data, UINT64 Value)
{
  UINTN Index;

  for (Index = 0; Index < 8; Index++) {
    Data[Index] = (UINT8)(Value >> (Index * 8));
  }
}

void
TestTcp4AndMessengerHandshake(void)
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

  {
    UINT8                         HandshakeReply[
                                    CEPH_MSGR_V1_BANNER_LENGTH +
                                    2 * CEPH_MSGR_V1_ENTITY_ADDR_LENGTH +
                                    CEPH_MSGR_V1_CONNECT_REPLY_SIZE + 3
                                    ];
    CEPH_MSGR_V1_NEGOTIATION      Negotiation;
    UINTN                         ServerAddressOffset;
    UINTN                         ReplyOffset;
    UINTN                         HandshakeLength;

    ZeroMem (HandshakeReply, sizeof (HandshakeReply));
    CopyMem (HandshakeReply, "ceph v027", CEPH_MSGR_V1_BANNER_LENGTH);
    ServerAddressOffset = CEPH_MSGR_V1_BANNER_LENGTH;
    HandshakeReply[ServerAddressOffset + 8] = 0;
    HandshakeReply[ServerAddressOffset + 9] = 2;
    HandshakeReply[ServerAddressOffset + 10] = 0x1A;
    HandshakeReply[ServerAddressOffset + 11] = 0x85;
    CopyMem (
      HandshakeReply + ServerAddressOffset + 12,
      RemoteAddress.Addr,
      sizeof (RemoteAddress.Addr)
      );

    ServerAddressOffset += CEPH_MSGR_V1_ENTITY_ADDR_LENGTH;
    HandshakeReply[ServerAddressOffset + 8] = 0;
    HandshakeReply[ServerAddressOffset + 9] = 2;
    HandshakeReply[ServerAddressOffset + 10] = 0xC0;
    HandshakeReply[ServerAddressOffset + 11] = 0x01;
    HandshakeReply[ServerAddressOffset + 12] = 192;
    HandshakeReply[ServerAddressOffset + 13] = 0;
    HandshakeReply[ServerAddressOffset + 14] = 2;
    HandshakeReply[ServerAddressOffset + 15] = 1;

    ReplyOffset = CEPH_MSGR_V1_BANNER_LENGTH +
                  2 * CEPH_MSGR_V1_ENTITY_ADDR_LENGTH;
    HandshakeLength = ReplyOffset + CEPH_MSGR_V1_CONNECT_REPLY_SIZE;
    HandshakeReply[ReplyOffset] = CEPH_MSGR_TAG_READY;
    HandshakeReply[ReplyOffset + 1] = 0x05;
    HandshakeReply[ReplyOffset + 9] = 3;
    HandshakeReply[ReplyOffset + 13] = 7;
    HandshakeReply[ReplyOffset + 17] = 1;

    mReceiveScript = HandshakeReply;
    mReceiveScriptLength = HandshakeLength;
    mReceiveScriptOffset = 0;
    mReceiveMaxChunk = 7;
    mSentLength = 0;

    ZeroMem (&Connection, sizeof (Connection));
    assert(CephRbdTcp4Connect(
             &mImage, &mController, &RemoteAddress, 6789, 2,
             &Connection) == EFI_SUCCESS);
    assert(RunTestHandshake(
             &Connection, &RemoteAddress, &Negotiation) == EFI_SUCCESS);
    assert(Negotiation.Ready);
    assert(Negotiation.Action == CephMsgrConnectReady);
    assert(Negotiation.AuthorizerReplyLength == 0);
    assert(mReceiveScriptOffset == HandshakeLength);
    assert(mSentLength ==
           CEPH_MSGR_V1_BANNER_LENGTH +
           CEPH_MSGR_V1_ENTITY_ADDR_LENGTH +
           CEPH_MSGR_V1_CONNECT_SIZE);
    assert(memcmp (mSentBuffer, "ceph v027", CEPH_MSGR_V1_BANNER_LENGTH) == 0);
    assert(memcmp (
             mSentBuffer + CEPH_MSGR_V1_BANNER_LENGTH,
             HandshakeReply + CEPH_MSGR_V1_BANNER_LENGTH +
               CEPH_MSGR_V1_ENTITY_ADDR_LENGTH,
             CEPH_MSGR_V1_ENTITY_ADDR_LENGTH
             ) == 0);
    assert(mSentBuffer[CEPH_MSGR_V1_BANNER_LENGTH +
                       CEPH_MSGR_V1_ENTITY_ADDR_LENGTH + 8] ==
           CEPH_MSGR_CONNECT_HOST_CLIENT);

    mReceiveScript = NULL;
    mReceiveMaxChunk = 0;
    assert(CephRbdTcp4Close(&Connection, 2) == EFI_SUCCESS);

    ZeroMem (HandshakeReply, sizeof (HandshakeReply));
    CopyMem (HandshakeReply, "ceph v027", CEPH_MSGR_V1_BANNER_LENGTH);
    ServerAddressOffset = CEPH_MSGR_V1_BANNER_LENGTH;
    HandshakeReply[ServerAddressOffset + 8] = 0;
    HandshakeReply[ServerAddressOffset + 9] = 2;
    HandshakeReply[ServerAddressOffset + 10] = 0x1A;
    HandshakeReply[ServerAddressOffset + 11] = 0x85;
    CopyMem (
      HandshakeReply + ServerAddressOffset + 12,
      RemoteAddress.Addr,
      sizeof (RemoteAddress.Addr)
      );
    ServerAddressOffset += CEPH_MSGR_V1_ENTITY_ADDR_LENGTH;
    HandshakeReply[ServerAddressOffset + 8] = 0;
    HandshakeReply[ServerAddressOffset + 9] = 2;
    HandshakeReply[ServerAddressOffset + 10] = 0xC0;
    HandshakeReply[ServerAddressOffset + 11] = 0x01;
    HandshakeReply[ServerAddressOffset + 12] = 192;
    HandshakeReply[ServerAddressOffset + 14] = 2;
    HandshakeReply[ServerAddressOffset + 15] = 1;
    HandshakeReply[ReplyOffset] = CEPH_MSGR_TAG_CHALLENGE_AUTH;
    HandshakeReply[ReplyOffset + 1] = 0x05;
    HandshakeReply[ReplyOffset + 9] = 3;
    HandshakeReply[ReplyOffset + 13] = 7;
    HandshakeReply[ReplyOffset + 17] = 1;
    HandshakeReply[ReplyOffset + 21] = 3;
    CopyMem (
      HandshakeReply + HandshakeLength,
      "xyz",
      3
      );

    mReceiveScript = HandshakeReply;
    mReceiveScriptLength = HandshakeLength + 3;
    mReceiveScriptOffset = 0;
    mReceiveMaxChunk = 5;
    mSentLength = 0;
    ZeroMem (&Connection, sizeof (Connection));
    assert(CephRbdTcp4Connect(
             &mImage, &mController, &RemoteAddress, 6789, 2,
             &Connection) == EFI_SUCCESS);
    assert(RunTestHandshake(
             &Connection, &RemoteAddress, &Negotiation) == EFI_SUCCESS);
    assert(!Negotiation.Ready);
    assert(Negotiation.Action == CephMsgrConnectNeedAuthorizerChallenge);
    assert(Negotiation.AuthorizerReplyLength == 3);
    assert(memcmp (Negotiation.AuthorizerReply, "xyz", 3) == 0);
    assert(CephRbdTcp4Close(&Connection, 2) == EFI_SUCCESS);

    HandshakeReply[CEPH_MSGR_V1_BANNER_LENGTH + 10] = 0x1A;
    HandshakeReply[CEPH_MSGR_V1_BANNER_LENGTH + 11] = 0x86;
    mReceiveScriptLength = CEPH_MSGR_V1_BANNER_LENGTH +
                           2 * CEPH_MSGR_V1_ENTITY_ADDR_LENGTH;
    mReceiveScriptOffset = 0;
    mSentLength = 0;
    ZeroMem (&Connection, sizeof (Connection));
    assert(CephRbdTcp4Connect(
             &mImage, &mController, &RemoteAddress, 6789, 2,
             &Connection) == EFI_SUCCESS);
    assert(RunTestHandshake(
             &Connection, &RemoteAddress, &Negotiation) ==
           EFI_ACCESS_DENIED);
    assert(mSentLength == CEPH_MSGR_V1_BANNER_LENGTH);
    assert(CephRbdTcp4Close(&Connection, 2) == EFI_SUCCESS);

    mReceiveScript = NULL;
    mReceiveMaxChunk = 0;
  }

  ZeroMem (&Connection, sizeof (Connection));
  assert(CephRbdTcp4Connect(
           &mImage, &mController, &RemoteAddress, 6789, 2,
           &Connection) == EFI_SUCCESS);
  mFailChildProtocolClose = TRUE;
  assert(CephRbdTcp4Close(&Connection, 2) == EFI_DEVICE_ERROR);
  assert(Connection.ProtocolOpen);
  assert(Connection.ChildHandle == &mChild);
  assert(CephRbdTcp4Close(&Connection, 2) == EFI_SUCCESS);
  assert(mDestroyChildCount == 5);
  assert(mCloseProtocolCount == 11);
}

void
TestMessengerSession(void)
{
  EFI_IPv4_ADDRESS RemoteAddress = { { 10, 0, 0, 7 } };
  CEPH_RBD_TCP4_CONNECTION Connection;
  CEPH_MSGR_V1_NEGOTIATION Negotiation;
  CEPH_MSGR_V1_SESSION Session;
  CEPH_MSGR_V1_FRAME Outgoing;
  CEPH_MSGR_V1_FRAME Incoming;
  CEPH_MSGR_V1_FRAME SentFrame;
  UINT8 OutgoingBuffer[256];
  UINT8 IncomingFrame[256];
  UINT8 SessionScript[1024];
  UINT8 ReceiveBuffer[256];
  UINT8 KeepaliveTime[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  UINTN OutgoingLength;
  UINTN IncomingLength;
  UINTN SessionScriptLength;
  UINTN ScriptOffset;
  UINTN FrameLength;
  UINTN ControlOffset;

  ZeroMem (&Connection, sizeof (Connection));
  assert(CephRbdTcp4Connect(
           &mImage, &mController, &RemoteAddress, 6789, 2,
           &Connection) == EFI_SUCCESS);
  ZeroMem (&Negotiation, sizeof (Negotiation));
  Negotiation.Ready = TRUE;
  Negotiation.Action = CephMsgrConnectReady;
  assert(CephMsgrV1SessionInit(
           &Session, &Connection, &Negotiation, 128, 2) == EFI_SUCCESS);

  ZeroMem (&Outgoing, sizeof (Outgoing));
  Outgoing.Type = 1;
  Outgoing.Priority = 64;
  Outgoing.Version = 1;
  Outgoing.SourceType = CEPH_MSGR_CONNECT_HOST_CLIENT;
  Outgoing.CompatVersion = 1;
  Outgoing.FrontLength = 7;
  Outgoing.Front = (CONST UINT8 *)"request";
  mSentLength = 0;
  assert(CephMsgrV1SessionSendMessage(
           &Session,
           &Outgoing,
           OutgoingBuffer,
           sizeof (OutgoingBuffer),
           &OutgoingLength
           ) == EFI_SUCCESS);
  assert(Session.NextOutgoingSequence == 2);
  assert(CephMsgrDecodeV1Frame(
           mSentBuffer,
           OutgoingLength,
           128,
           &SentFrame
           ) == EFI_SUCCESS);
  assert(SentFrame.Sequence == 1);

  ZeroMem (&Incoming, sizeof (Incoming));
  Incoming.Sequence = 1;
  Incoming.Type = 2;
  Incoming.Priority = 64;
  Incoming.Version = 1;
  Incoming.SourceType = 1;
  Incoming.CompatVersion = 1;
  Incoming.FrontLength = 8;
  Incoming.Front = (CONST UINT8 *)"response";
  assert(CephMsgrEncodeV1Frame(
           &Incoming,
           128,
           IncomingFrame,
           sizeof (IncomingFrame),
           &IncomingLength
           ) == EFI_SUCCESS);

  ScriptOffset = 0;
  SessionScript[ScriptOffset++] = CEPH_MSGR_TAG_ACK;
  WriteTestLe64(SessionScript + ScriptOffset, 1);
  ScriptOffset += 8;
  SessionScript[ScriptOffset++] = CEPH_MSGR_TAG_KEEPALIVE2;
  CopyMem (SessionScript + ScriptOffset, KeepaliveTime, sizeof (KeepaliveTime));
  ScriptOffset += sizeof (KeepaliveTime);
  CopyMem (SessionScript + ScriptOffset, IncomingFrame, IncomingLength);
  ScriptOffset += IncomingLength;
  CopyMem (SessionScript + ScriptOffset, IncomingFrame, IncomingLength);
  ScriptOffset += IncomingLength;
  Incoming.Sequence = 2;
  assert(CephMsgrEncodeV1Frame(
           &Incoming,
           128,
           IncomingFrame,
           sizeof (IncomingFrame),
           &IncomingLength
           ) == EFI_SUCCESS);
  CopyMem (SessionScript + ScriptOffset, IncomingFrame, IncomingLength);
  ScriptOffset += IncomingLength;
  SessionScriptLength = ScriptOffset;
  mReceiveScript = SessionScript;
  mReceiveScriptLength = SessionScriptLength;
  mReceiveScriptOffset = 0;
  mReceiveMaxChunk = 9;

  assert(CephMsgrV1SessionReceiveMessage(
           &Session,
           ReceiveBuffer,
           sizeof (ReceiveBuffer),
           &Incoming,
           &FrameLength
           ) == EFI_SUCCESS);
  assert(Incoming.Sequence == 1);
  assert(Incoming.FrontLength == 8);
  assert(memcmp (Incoming.Front, "response", 8) == 0);
  assert(Session.PeerAcknowledgedSequence == 1);
  assert(Session.LastIncomingSequence == 1);

  assert(CephMsgrV1SessionReceiveMessage(
           &Session,
           ReceiveBuffer,
           sizeof (ReceiveBuffer),
           &Incoming,
           &FrameLength
           ) == EFI_SUCCESS);
  assert(Incoming.Sequence == 2);
  assert(Session.LastIncomingSequence == 2);
  assert(mReceiveScriptOffset == SessionScriptLength);

  ControlOffset = OutgoingLength;
  assert(mSentBuffer[ControlOffset] == CEPH_MSGR_TAG_KEEPALIVE2_ACK);
  assert(memcmp (
           mSentBuffer + ControlOffset + 1,
           KeepaliveTime,
           sizeof (KeepaliveTime)
           ) == 0);
  ControlOffset += 9;
  assert(mSentBuffer[ControlOffset] == CEPH_MSGR_TAG_ACK);
  assert(mSentBuffer[ControlOffset + 1] == 1);
  ControlOffset += 9;
  assert(mSentBuffer[ControlOffset] == CEPH_MSGR_TAG_ACK);
  assert(mSentBuffer[ControlOffset + 1] == 1);
  ControlOffset += 9;
  assert(mSentBuffer[ControlOffset] == CEPH_MSGR_TAG_ACK);
  assert(mSentBuffer[ControlOffset + 1] == 2);

  mReceiveScript = NULL;
  mReceiveScriptLength = 0;
  mReceiveScriptOffset = 0;
  mReceiveMaxChunk = 0;
  assert(CephRbdTcp4Close(&Connection, 2) == EFI_SUCCESS);

  ZeroMem (&Connection, sizeof (Connection));
  assert(CephRbdTcp4Connect(
           &mImage, &mController, &RemoteAddress, 6789, 2,
           &Connection) == EFI_SUCCESS);
  assert(CephMsgrV1SessionInit(
           &Session, &Connection, &Negotiation, 128, 2) == EFI_SUCCESS);
  {
    UINT8 InvalidAck[9] = { CEPH_MSGR_TAG_ACK, 1, 0, 0, 0, 0, 0, 0, 0 };

    mReceiveScript = InvalidAck;
    mReceiveScriptLength = sizeof (InvalidAck);
    mReceiveScriptOffset = 0;
    assert(CephMsgrV1SessionReceiveMessage(
             &Session,
             ReceiveBuffer,
             sizeof (ReceiveBuffer),
             &Incoming,
             &FrameLength
             ) == EFI_COMPROMISED_DATA);
    assert(!Session.Ready);
  }

  mReceiveScript = NULL;
  mReceiveScriptLength = 0;
  mReceiveScriptOffset = 0;
  assert(CephRbdTcp4Close(&Connection, 2) == EFI_SUCCESS);
}

void
TestMessengerFrameTransport(void)
{
  EFI_IPv4_ADDRESS RemoteAddress = { { 10, 0, 0, 7 } };
  CEPH_RBD_TCP4_CONNECTION Connection;
  CEPH_MSGR_V1_FRAME Outgoing;
  CEPH_MSGR_V1_FRAME Incoming;
  UINT8 FrameBuffer[256];
  UINT8 WireBuffer[256];
  UINTN FrameLength;
  UINTN WireLength;

  ZeroMem (&Connection, sizeof (Connection));
  assert(CephRbdTcp4Connect(
           &mImage, &mController, &RemoteAddress, 6789, 2,
           &Connection) == EFI_SUCCESS);

  ZeroMem (&Outgoing, sizeof (Outgoing));
  Outgoing.Sequence = 42;
  Outgoing.TransactionId = 0x1234;
  Outgoing.Type = 1;
  Outgoing.Priority = 64;
  Outgoing.Version = 1;
  Outgoing.SourceType = CEPH_MSGR_CONNECT_HOST_CLIENT;
  Outgoing.SourceNumber = 9;
  Outgoing.CompatVersion = 1;
  Outgoing.FrontLength = 5;
  Outgoing.Front = (CONST UINT8 *)"front";

  mSentLength = 0;
  assert(CephMsgrV1SendFrame(
           &Connection,
           &Outgoing,
           128,
           2,
           FrameBuffer,
           sizeof (FrameBuffer),
           &FrameLength
           ) == EFI_SUCCESS);
  assert(FrameLength == mSentLength);
  assert(FrameLength > CEPH_MSGR_V1_HEADER_SIZE);
  assert(CephMsgrDecodeV1Frame(
           mSentBuffer,
           mSentLength,
           128,
           &Incoming
           ) == EFI_SUCCESS);
  assert(Incoming.Sequence == 42);
  assert(Incoming.TransactionId == 0x1234);
  assert(Incoming.FrontLength == 5);
  assert(memcmp (Incoming.Front, "front", 5) == 0);

  ZeroMem (&Outgoing, sizeof (Outgoing));
  Outgoing.Sequence = 43;
  Outgoing.Type = 2;
  Outgoing.Priority = 64;
  Outgoing.Version = 1;
  Outgoing.SourceType = CEPH_MSGR_CONNECT_HOST_CLIENT;
  Outgoing.SourceNumber = 10;
  Outgoing.CompatVersion = 1;
  Outgoing.DataLength = 7;
  Outgoing.Data = (CONST UINT8 *)"payload";
  assert(CephMsgrEncodeV1Frame(
           &Outgoing,
           128,
           WireBuffer,
           sizeof (WireBuffer),
           &WireLength
           ) == EFI_SUCCESS);

  mReceiveScript = WireBuffer;
  mReceiveScriptLength = WireLength;
  mReceiveScriptOffset = 0;
  mReceiveMaxChunk = 13;
  assert(CephMsgrV1ReceiveFrame(
           &Connection,
           128,
           2,
           FrameBuffer,
           sizeof (FrameBuffer),
           &Incoming,
           &FrameLength
           ) == EFI_SUCCESS);
  assert(FrameLength == WireLength);
  assert(Incoming.Sequence == 43);
  assert(Incoming.DataLength == 7);
  assert(memcmp (Incoming.Data, "payload", 7) == 0);
  assert(mReceiveScriptOffset == WireLength);

  {
    UINT8 ControlTag = CEPH_MSGR_TAG_ACK;

    mReceiveScript = &ControlTag;
    mReceiveScriptLength = sizeof (ControlTag);
    mReceiveScriptOffset = 0;
    assert(CephMsgrV1ReceiveFrame(
             &Connection,
             128,
             2,
             FrameBuffer,
             sizeof (FrameBuffer),
             &Incoming,
             &FrameLength
             ) == EFI_UNSUPPORTED);
    assert(mReceiveScriptOffset == 1);
  }

  mReceiveScript = WireBuffer;
  mReceiveScriptLength = WireLength;
  WireBuffer[10] ^= 0x01;
  mReceiveScriptOffset = 0;
  assert(CephMsgrV1ReceiveFrame(
           &Connection,
           128,
           2,
           FrameBuffer,
           sizeof (FrameBuffer),
           &Incoming,
           &FrameLength
           ) == EFI_CRC_ERROR);
  assert(mReceiveScriptOffset == 1 + CEPH_MSGR_V1_HEADER_SIZE);

  WireBuffer[10] ^= 0x01;
  mReceiveScriptOffset = 0;
  assert(CephMsgrV1ReceiveFrame(
           &Connection,
           6,
           2,
           FrameBuffer,
           sizeof (FrameBuffer),
           &Incoming,
           &FrameLength
           ) == EFI_BAD_BUFFER_SIZE);
  assert(mReceiveScriptOffset == 1 + CEPH_MSGR_V1_HEADER_SIZE);

  mReceiveScript = NULL;
  mReceiveScriptLength = 0;
  mReceiveScriptOffset = 0;
  mReceiveMaxChunk = 0;
  assert(CephRbdTcp4Close(&Connection, 2) == EFI_SUCCESS);
}
