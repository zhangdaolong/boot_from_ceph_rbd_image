#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ceph_stage2.h"
#include "ceph_stage2_network.h"
#include "pxe_undi.h"

#define MOCK_ROM_START 0x000c0000u
#define MOCK_ROM_SIZE  0x00040000u
#define MOCK_ROM_PXE   0x000f0000u
#define MOCK_MAX_CALLS 64u
#define MOCK_MAX_FAR   32u
#define MOCK_PARAM_MAX 64u

typedef struct {
  uint16_t Opcode;
  uint16_t ParamBlockSize;
  uint8_t ParamBlock[MOCK_PARAM_MAX];
} MOCK_CALL;

typedef struct {
  uint8_t Rom[MOCK_ROM_SIZE];
  MOCK_CALL Calls[MOCK_MAX_CALLS];
  unsigned CallCount;
  PXE_UNDI_FAR_PTR FarPtrs[MOCK_MAX_FAR];
  void *HostPtrs[MOCK_MAX_FAR];
  unsigned FarPtrCount;
  uint8_t TxFrame[1600];
  size_t TxLength;
  uint8_t RxFrame[1600];
  size_t RxLength;
  uint8_t StationAddress[6];
  uint16_t Filter;
  uint16_t MulticastCount;
  uint16_t FailOpcode;
} MOCK;

static const uint8_t kStation[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

static void
WriteRomPxe(MOCK *Mock, uint8_t Revision, int Corrupt)
{
  PXE_UNDI_PXE_STRUCT pxe;
  unsigned offset = MOCK_ROM_PXE - MOCK_ROM_START;
  unsigned length;
  unsigned sum = 0;
  unsigned index;

  memset(&pxe, 0, sizeof(pxe));
  memcpy(pxe.Signature, "!PXE", 4);
  pxe.StructLength = (uint8_t)sizeof(pxe);
  pxe.Revision = Revision;
  pxe.Reserved1 = 0;
  pxe.UndiCIDataSelector = 0x0028;
  pxe.PXEnvSelector = 0x0030;
  pxe.StackSize = 0x0800;
  pxe.StackSeg = 0x9000;
  pxe.UndiCodeSeg = 0xa000;
  pxe.UndiCodeOffset = 0x0100;
  pxe.UndiDataSeg = 0xb000;
  pxe.PXEnvCodeSeg = 0xc000;
  pxe.PXEnvCodeOffset = 0x0200;

  length = pxe.StructLength;
  for (index = 0; index < length; index++) {
    sum += ((const uint8_t *)&pxe)[index];
  }
  pxe.Checksum = (uint8_t)(0u - (sum & 0xffu));

  memcpy(Mock->Rom + offset, &pxe, sizeof(pxe));
  if (Corrupt) {
    Mock->Rom[offset + 7] ^= 0x01u;
  }
}

static int
MockReadBiosMemory(void *Context, uint32_t Address, void *Buffer, size_t Length)
{
  MOCK *mock = (MOCK *)Context;
  uint32_t offset;

  if (Address < MOCK_ROM_START || Address >= MOCK_ROM_START + MOCK_ROM_SIZE) {
    return -1;
  }
  offset = Address - MOCK_ROM_START;
  if (Length > MOCK_ROM_SIZE - offset) {
    return -1;
  }
  memcpy(Buffer, mock->Rom + offset, Length);
  return 0;
}

static PXE_UNDI_FAR_PTR
MockMakeFarPointer(void *Context, const void *HostAddress)
{
  MOCK *mock = (MOCK *)Context;
  PXE_UNDI_FAR_PTR far = { 0, 0 };
  unsigned index;

  for (index = 0; index < mock->FarPtrCount; index++) {
    if (mock->HostPtrs[index] == HostAddress) {
      return mock->FarPtrs[index];
    }
  }
  if (mock->FarPtrCount >= MOCK_MAX_FAR) {
    return far;
  }
  index = mock->FarPtrCount++;
  mock->HostPtrs[index] = (void *)HostAddress;
  mock->FarPtrs[index].Segment = 0x9000;
  mock->FarPtrs[index].Offset = (uint16_t)(0x1000 + index);
  return mock->FarPtrs[index];
}

static void *
MockFarToHost(void *Context, PXE_UNDI_FAR_PTR FarPointer)
{
  MOCK *mock = (MOCK *)Context;
  unsigned index;

  if (FarPointer.Segment != 0x9000 || FarPointer.Offset < 0x1000) {
    return NULL;
  }
  index = FarPointer.Offset - 0x1000;
  if (index >= mock->FarPtrCount) {
    return NULL;
  }
  return mock->HostPtrs[index];
}

static uint16_t
MockCallApi(void *Context, uint16_t Opcode, void *ParamBlock,
            uint16_t ParamBlockSize, uint16_t *ApiExit)
{
  MOCK *mock = (MOCK *)Context;
  PXENV_UNDI_TRANSMIT_PARAMS *tx;
  PXENV_UNDI_TBD *tbd;
  PXENV_UNDI_ISR_PARAMS *isr;
  PXENV_UNDI_GET_INFORMATION_PARAMS *info;
  PXENV_UNDI_GET_NIC_TYPE_PARAMS *nic;
  PXENV_UNDI_SET_STATION_ADDRESS_PARAMS *station;
  PXENV_UNDI_OPEN_PARAMS *open;
  uint8_t *raw = (uint8_t *)ParamBlock;
  const uint8_t *source;
  size_t copy_length;
  MOCK_CALL *call;

  if (mock->CallCount < MOCK_MAX_CALLS) {
    call = &mock->Calls[mock->CallCount++];
    call->Opcode = Opcode;
    call->ParamBlockSize = ParamBlockSize;
    copy_length = ParamBlockSize < MOCK_PARAM_MAX ?
                  ParamBlockSize : MOCK_PARAM_MAX;
    memcpy(call->ParamBlock, ParamBlock, copy_length);
  }
  if (ApiExit != NULL) {
    *ApiExit = 0;
  }
  if (Opcode == mock->FailOpcode) {
    return 1;
  }

  switch (Opcode) {
  case PXENV_UNDI_GET_NIC_TYPE:
    nic = (PXENV_UNDI_GET_NIC_TYPE_PARAMS *)ParamBlock;
    nic->NicType = PXE_UNDI_NIC_PCI;
    nic->Pci.VendorId = 0x10ec;
    nic->Pci.DeviceId = 0x8168;
    nic->Status = 0;
    break;
  case PXENV_UNDI_GET_INFORMATION:
    info = (PXENV_UNDI_GET_INFORMATION_PARAMS *)ParamBlock;
    info->HwType = 1;
    info->HwAddrLen = 6;
    info->MaxTranUnit = PXE_UNDI_ETHERNET_MTU;
    info->RxBufCount = 8;
    info->TxBufCount = 8;
    memcpy(info->PermNodeAddress, kStation, sizeof(kStation));
    memcpy(info->CurrentNodeAddress, kStation, sizeof(kStation));
    info->Status = 0;
    break;
  case PXENV_UNDI_SET_STATION_ADDRESS:
    station = (PXENV_UNDI_SET_STATION_ADDRESS_PARAMS *)ParamBlock;
    memcpy(mock->StationAddress, station->StationAddress, 6);
    station->Status = 0;
    break;
  case PXENV_UNDI_OPEN:
    open = (PXENV_UNDI_OPEN_PARAMS *)ParamBlock;
    mock->Filter = open->PktFilter;
    mock->MulticastCount = open->Multicast.MCastAddrCount;
    open->Status = 0;
    break;
  case PXENV_UNDI_SET_PACKET_FILTER:
    mock->Filter = raw[2];
    raw[0] = 0;
    raw[1] = 0;
    break;
  case PXENV_UNDI_SET_MCAST_ADDRESS:
    mock->MulticastCount = (uint16_t)(raw[2] | ((uint16_t)raw[3] << 8));
    raw[0] = 0;
    raw[1] = 0;
    break;
  case PXENV_UNDI_TRANSMIT:
    tx = (PXENV_UNDI_TRANSMIT_PARAMS *)ParamBlock;
    tbd = (PXENV_UNDI_TBD *)MockFarToHost(Context, tx->TBD);
    source = (const uint8_t *)MockFarToHost(Context, tbd->Xmit);
    memcpy(mock->TxFrame, source, tbd->ImmedLength);
    mock->TxLength = tbd->ImmedLength;
    tx->Status = 0;
    break;
  case PXENV_UNDI_RECEIVE:
    isr = (PXENV_UNDI_ISR_PARAMS *)ParamBlock;
    if (mock->RxLength != 0) {
      source = mock->RxFrame;
      copy_length = mock->RxLength < isr->FrameLength ?
                    mock->RxLength : isr->FrameLength;
      memcpy(MockFarToHost(Context, isr->Frame), source, copy_length);
      isr->FrameLength = (uint16_t)mock->RxLength;
      isr->PacketType = 0;
      mock->RxLength = 0;
    } else {
      isr->FrameLength = 0;
    }
    isr->Status = 0;
    break;
  default:
    raw[0] = 0;
    raw[1] = 0;
    break;
  }
  return 0;
}

static PXE_UNDI_PLATFORM
MakePlatform(MOCK *Mock)
{
  PXE_UNDI_PLATFORM platform;

  memset(&platform, 0, sizeof(platform));
  platform.Context = Mock;
  platform.ReadBiosMemory = MockReadBiosMemory;
  platform.CallApi = MockCallApi;
  platform.MakeFarPointer = MockMakeFarPointer;
  platform.FarPointerToHost = MockFarToHost;
  return platform;
}

static const MOCK_CALL *
FindCall(const MOCK *Mock, uint16_t Opcode)
{
  unsigned index;

  for (index = 0; index < Mock->CallCount; index++) {
    if (Mock->Calls[index].Opcode == Opcode) {
      return &Mock->Calls[index];
    }
  }
  return NULL;
}

static void
BuildFrame(uint8_t *Frame, size_t *Length, int Broadcast)
{
  memset(Frame, 0, 18);
  if (Broadcast) {
    memset(Frame, 0xff, 6);
  } else {
    memcpy(Frame, "\x02\x00\x00\x00\x00\x02", 6);
  }
  memcpy(Frame + 6, kStation, 6);
  Frame[12] = 0x08;
  Frame[13] = 0x00;
  Frame[14] = 0xde;
  Frame[15] = 0xad;
  Frame[16] = 0xbe;
  Frame[17] = 0xef;
  *Length = 18;
}

static void
TestDiscovery(void)
{
  MOCK mock;
  PXE_UNDI_PLATFORM platform;
  PXE_UNDI_CONTEXT context;
  const PXE_UNDI_PXE_STRUCT *pxe;

  memset(&mock, 0, sizeof(mock));
  platform = MakePlatform(&mock);

  assert(PxeUndiDiscover(&platform, &context) == PXE_UNDI_NOT_FOUND);

  WriteRomPxe(&mock, 0, 0);
  assert(PxeUndiDiscover(&platform, &context) == PXE_UNDI_SUCCESS);
  assert(context.Ready == 1);
  assert(context.PhysicalAddress == MOCK_ROM_PXE);
  pxe = &context.Pxe;
  assert(memcmp(pxe->Signature, "!PXE", 4) == 0);
  assert(pxe->StructLength == sizeof(PXE_UNDI_PXE_STRUCT));
  assert(PxeUndiValidateChecksum(pxe) == 1);
  assert(pxe->Revision == 0);
  assert(pxe->StackSize == 0x0800 && pxe->StackSeg == 0x9000);
  assert(pxe->UndiCodeSeg == 0xa000 && pxe->UndiCodeOffset == 0x0100);
  assert(context.EntryPoint.Segment == 0xc000);
  assert(context.EntryPoint.Offset == 0x0200);

  WriteRomPxe(&mock, 0, 1);
  assert(PxeUndiDiscover(&platform, &context) == PXE_UNDI_BAD_CHECKSUM);

  WriteRomPxe(&mock, 1, 0);
  assert(PxeUndiDiscover(&platform, &context) == PXE_UNDI_NOT_FOUND);

  assert(PxeUndiDiscover(NULL, &context) == PXE_UNDI_INVALID_ARGUMENT);
  assert(PxeUndiValidateChecksum(NULL) == 0);
}

static void
TestParameterBlocks(void)
{
  MOCK mock;
  PXE_UNDI_PLATFORM platform;
  PXE_UNDI_CONTEXT context;
  PXENV_UNDI_GET_NIC_TYPE_PARAMS nic;
  PXENV_UNDI_GET_INFORMATION_PARAMS info;
  const MOCK_CALL *call;
  uint8_t frame[1600];
  uint8_t received[1600];
  size_t length;
  size_t capacity;
  const uint8_t mcast[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };

  memset(&mock, 0, sizeof(mock));
  platform = MakePlatform(&mock);
  WriteRomPxe(&mock, 0, 0);
  assert(PxeUndiDiscover(&platform, &context) == PXE_UNDI_SUCCESS);

  assert(PxeUndiStartup(&context) == PXE_UNDI_SUCCESS);
  assert(PxeUndiInitialize(&context, NULL) == PXE_UNDI_SUCCESS);
  assert(PxeUndiGetNicType(&context, &nic) == PXE_UNDI_SUCCESS);
  assert(PxeUndiGetInformation(&context, &info) == PXE_UNDI_SUCCESS);
  assert(PxeUndiSetStationAddress(&context, kStation) == PXE_UNDI_SUCCESS);
  assert(PxeUndiOpen(&context, PXE_UNDI_FLTR_DIRECTED |
                              PXE_UNDI_FLTR_BROADCAST) == PXE_UNDI_SUCCESS);
  assert(PxeUndiSetPacketFilter(&context, PXE_UNDI_FLTR_PROMISCUOUS) ==
         PXE_UNDI_SUCCESS);
  assert(PxeUndiSetMulticast(&context, &mcast, 1) == PXE_UNDI_SUCCESS);
  assert(PxeUndiClose(&context) == PXE_UNDI_SUCCESS);
  assert(PxeUndiShutdown(&context) == PXE_UNDI_SUCCESS);

  call = FindCall(&mock, PXENV_UNDI_STARTUP);
  assert(call != NULL && call->ParamBlockSize == sizeof(PXENV_UNDI_SIMPLE_PARAMS));
  call = FindCall(&mock, PXENV_UNDI_INITIALIZE);
  assert(call != NULL && call->ParamBlockSize == sizeof(PXENV_UNDI_INITIALIZE_PARAMS));
  assert(((const PXENV_UNDI_INITIALIZE_PARAMS *)call->ParamBlock)->ProtocolIni.Segment == 0);
  call = FindCall(&mock, PXENV_UNDI_GET_NIC_TYPE);
  assert(call != NULL && call->ParamBlockSize == sizeof(PXENV_UNDI_GET_NIC_TYPE_PARAMS));
  call = FindCall(&mock, PXENV_UNDI_GET_INFORMATION);
  assert(call != NULL && call->ParamBlockSize == sizeof(PXENV_UNDI_GET_INFORMATION_PARAMS));
  call = FindCall(&mock, PXENV_UNDI_SET_STATION_ADDRESS);
  assert(call != NULL && call->ParamBlockSize == sizeof(PXENV_UNDI_SET_STATION_ADDRESS_PARAMS));
  assert(memcmp(((const PXENV_UNDI_SET_STATION_ADDRESS_PARAMS *)
                 call->ParamBlock)->StationAddress, kStation, 6) == 0);
  call = FindCall(&mock, PXENV_UNDI_OPEN);
  assert(call != NULL && call->ParamBlockSize == sizeof(PXENV_UNDI_OPEN_PARAMS));
  assert(((const PXENV_UNDI_OPEN_PARAMS *)call->ParamBlock)->OpenFlag == 0);
  assert(((const PXENV_UNDI_OPEN_PARAMS *)call->ParamBlock)->PktFilter ==
         (PXE_UNDI_FLTR_DIRECTED | PXE_UNDI_FLTR_BROADCAST));
  call = FindCall(&mock, PXENV_UNDI_SET_PACKET_FILTER);
  assert(call != NULL && call->ParamBlock[2] == PXE_UNDI_FLTR_PROMISCUOUS);
  call = FindCall(&mock, PXENV_UNDI_SET_MCAST_ADDRESS);
  assert(call != NULL && call->ParamBlock[2] == 1);
  assert(memcmp(call->ParamBlock + 4, mcast, 6) == 0);
  call = FindCall(&mock, PXENV_UNDI_CLOSE);
  assert(call != NULL && call->ParamBlockSize == sizeof(PXENV_UNDI_SIMPLE_PARAMS));
  call = FindCall(&mock, PXENV_UNDI_SHUTDOWN);
  assert(call != NULL && call->ParamBlockSize == sizeof(PXENV_UNDI_SIMPLE_PARAMS));

  BuildFrame(frame, &length, 1);
  assert(PxeUndiSendFrame(&context, frame, length) == PXE_UNDI_SUCCESS);
  call = FindCall(&mock, PXENV_UNDI_TRANSMIT);
  assert(call != NULL && call->ParamBlockSize == sizeof(PXENV_UNDI_TRANSMIT_PARAMS));
  assert(((const PXENV_UNDI_TRANSMIT_PARAMS *)call->ParamBlock)->Protocol ==
         PXE_UNDI_PROTO_UNKNOWN);
  assert(((const PXENV_UNDI_TRANSMIT_PARAMS *)call->ParamBlock)->XmitFlag ==
         PXE_UNDI_XMT_BROADCAST);
  assert(mock.TxLength == length && memcmp(mock.TxFrame, frame, length) == 0);

  BuildFrame(frame, &length, 0);
  assert(PxeUndiSendFrame(&context, frame, length) == PXE_UNDI_SUCCESS);
  assert(((const PXENV_UNDI_TRANSMIT_PARAMS *)
          mock.Calls[mock.CallCount - 1].ParamBlock)->XmitFlag ==
         PXE_UNDI_XMT_DESTADDR);

  memcpy(mock.RxFrame, frame, length);
  mock.RxLength = length;
  capacity = sizeof(received);
  assert(PxeUndiReceiveFrame(&context, received, &capacity) == PXE_UNDI_SUCCESS);
  assert(capacity == length && memcmp(received, frame, length) == 0);
  call = FindCall(&mock, PXENV_UNDI_RECEIVE);
  assert(call != NULL && call->ParamBlockSize == sizeof(PXENV_UNDI_ISR_PARAMS));
  assert(((const PXENV_UNDI_ISR_PARAMS *)call->ParamBlock)->FuncFlag ==
         PXE_UNDI_ISR_START);
}

static void
TestFrameRoundTrip(void)
{
  MOCK mock;
  PXE_UNDI_PLATFORM platform;
  PXE_UNDI_CONTEXT context;
  uint8_t frame[1600];
  uint8_t received[1600];
  size_t length;
  size_t capacity;

  memset(&mock, 0, sizeof(mock));
  platform = MakePlatform(&mock);
  WriteRomPxe(&mock, 0, 0);
  assert(PxeUndiDiscover(&platform, &context) == PXE_UNDI_SUCCESS);
  assert(PxeUndiStartup(&context) == PXE_UNDI_SUCCESS);
  assert(PxeUndiInitialize(&context, NULL) == PXE_UNDI_SUCCESS);
  assert(PxeUndiOpen(&context, PXE_UNDI_FLTR_DIRECTED) == PXE_UNDI_SUCCESS);

  BuildFrame(frame, &length, 0);
  assert(PxeUndiSendFrame(&context, frame, length) == PXE_UNDI_SUCCESS);
  assert(mock.TxLength == length && memcmp(mock.TxFrame, frame, length) == 0);

  memcpy(mock.RxFrame, frame, length);
  mock.RxLength = length;
  capacity = sizeof(received);
  assert(PxeUndiReceiveFrame(&context, received, &capacity) == PXE_UNDI_SUCCESS);
  assert(capacity == length && memcmp(received, frame, length) == 0);

  capacity = sizeof(received);
  assert(PxeUndiReceiveFrame(&context, received, &capacity) == PXE_UNDI_NO_PACKET);

  memcpy(mock.RxFrame, frame, length);
  mock.RxLength = length;
  capacity = 8;
  assert(PxeUndiReceiveFrame(&context, received, &capacity) ==
         PXE_UNDI_BUFFER_TOO_SMALL);

  assert(PxeUndiSendFrame(&context, NULL, 1) == PXE_UNDI_INVALID_ARGUMENT);
}

static void
TestStage2NetworkPath(void)
{
  MOCK mock;
  PXE_UNDI_PLATFORM platform;
  CEPH_BIOS_NETWORK network;
  uint8_t frame[1600];
  uint8_t received[1600];
  size_t length;
  size_t capacity;
  unsigned sequence = 0;

  memset(&mock, 0, sizeof(mock));
  platform = MakePlatform(&mock);
  WriteRomPxe(&mock, 0, 0);

  /* Without the registered operations table the disk-only path is intact. */
  gCephBiosPxeUndiOps.Discover = NULL;
  assert(CephBiosStage2InitNetwork(&platform, &network) ==
         CEPH_BIOS_NETWORK_NOT_AVAILABLE);
  assert(CephBiosStage2InitNetwork(NULL, &network) ==
         CEPH_BIOS_NETWORK_NOT_AVAILABLE);

  PxeUndiInstallNetOps();
  assert(CephBiosStage2InitNetwork(&platform, &network) == CEPH_BIOS_SUCCESS);
  assert(network.Send != NULL && network.Receive != NULL &&
         network.Context != NULL);

  assert(mock.Calls[sequence++].Opcode == PXENV_UNDI_STARTUP);
  assert(mock.Calls[sequence++].Opcode == PXENV_UNDI_INITIALIZE);
  assert(mock.Calls[sequence++].Opcode == PXENV_UNDI_GET_NIC_TYPE);
  assert(mock.Calls[sequence++].Opcode == PXENV_UNDI_GET_INFORMATION);
  assert(mock.Calls[sequence++].Opcode == PXENV_UNDI_SET_STATION_ADDRESS);
  assert(mock.Calls[sequence++].Opcode == PXENV_UNDI_OPEN);
  assert(memcmp(mock.StationAddress, kStation, 6) == 0);
  assert(mock.Filter == (PXE_UNDI_FLTR_DIRECTED | PXE_UNDI_FLTR_BROADCAST));

  BuildFrame(frame, &length, 0);
  assert(CephBiosStage2SendFrame(&network, frame, length) == CEPH_BIOS_SUCCESS);
  assert(mock.TxLength == length && memcmp(mock.TxFrame, frame, length) == 0);

  memcpy(mock.RxFrame, frame, length);
  mock.RxLength = length;
  capacity = sizeof(received);
  assert(CephBiosStage2ReceiveFrame(&network, received, &capacity) ==
         CEPH_BIOS_SUCCESS);
  assert(capacity == length && memcmp(received, frame, length) == 0);

  assert(CephBiosStage2ShutdownNetwork(&network) == CEPH_BIOS_SUCCESS);
  assert(FindCall(&mock, PXENV_UNDI_CLOSE) != NULL);
  assert(FindCall(&mock, PXENV_UNDI_SHUTDOWN) != NULL);
  assert(network.Send == NULL && network.Receive == NULL &&
         network.Context == NULL);
}

int
main(void)
{
  TestDiscovery();
  TestParameterBlocks();
  TestFrameRoundTrip();
  TestStage2NetworkPath();
  puts("bios pxe/undi tests: passed");
  return 0;
}
