#include "pxe_undi.h"

#include <string.h>

/*
 * The byte checksum of the !PXE structure must be zero over StructLength
 * bytes. The scan window never hands us a length larger than our struct.
 */
int
PxeUndiValidateChecksum(const PXE_UNDI_PXE_STRUCT *Pxe)
{
  const uint8_t *bytes;
  unsigned length;
  unsigned sum = 0;
  unsigned index;

  if (Pxe == NULL || Pxe->StructLength < PXE_UNDI_PXE_MIN_LENGTH) {
    return 0;
  }

  length = Pxe->StructLength;
  if (length > sizeof(*Pxe)) {
    length = sizeof(*Pxe);
  }

  bytes = (const uint8_t *)Pxe;
  for (index = 0; index < length; index++) {
    sum += bytes[index];
  }
  return (sum & 0xffu) == 0;
}

int
PxeUndiDiscover(const PXE_UNDI_PLATFORM *Platform, PXE_UNDI_CONTEXT *Context)
{
  uint8_t signature[4];
  uint8_t raw[PXE_UNDI_PXE_MAX_LENGTH];
  uint32_t address;
  unsigned length;
  unsigned sum;
  unsigned index;
  int saw_bad_checksum = 0;

  if (Platform == NULL || Context == NULL || Platform->ReadBiosMemory == NULL) {
    return PXE_UNDI_INVALID_ARGUMENT;
  }

  memset(Context, 0, sizeof(*Context));
  Context->Platform = *Platform;

  for (address = PXE_UNDI_BIOS_ROM_START;
       address < PXE_UNDI_BIOS_ROM_END;
       address++) {
    if (Platform->ReadBiosMemory(Platform->Context, address,
                                 signature, sizeof(signature)) != 0) {
      continue;
    }
    if (signature[0] != PXE_UNDI_PXE_SIGNATURE_0 ||
        signature[1] != PXE_UNDI_PXE_SIGNATURE_1 ||
        signature[2] != PXE_UNDI_PXE_SIGNATURE_2 ||
        signature[3] != PXE_UNDI_PXE_SIGNATURE_3) {
      continue;
    }

    if (Platform->ReadBiosMemory(Platform->Context, address + 4,
                                 raw, 1) != 0) {
      continue;
    }
    length = raw[0];
    if (length < PXE_UNDI_PXE_MIN_LENGTH || length > sizeof(Context->Pxe)) {
      continue;
    }
    if (Platform->ReadBiosMemory(Platform->Context, address,
                                 raw, length) != 0) {
      continue;
    }

    sum = 0;
    for (index = 0; index < length; index++) {
      sum += raw[index];
    }
    if ((sum & 0xffu) != 0) {
      saw_bad_checksum = 1;
      continue;
    }

    memcpy(&Context->Pxe, raw, length);
    if (Context->Pxe.Revision != 0) {
      continue;
    }
    if (Context->Pxe.PXEnvCodeSeg == 0 && Context->Pxe.PXEnvCodeOffset == 0 &&
        Context->Pxe.UndiCodeSeg == 0 && Context->Pxe.UndiCodeOffset == 0) {
      continue;
    }

    Context->PhysicalAddress = address;
    Context->EntryPoint.Segment = Context->Pxe.PXEnvCodeSeg;
    Context->EntryPoint.Offset = Context->Pxe.PXEnvCodeOffset;
    Context->Ready = 1;
    return PXE_UNDI_SUCCESS;
  }

  return saw_bad_checksum ? PXE_UNDI_BAD_CHECKSUM : PXE_UNDI_NOT_FOUND;
}

/* All UNDI parameter blocks begin with a 16-bit status word. */
static int
PxeUndiCall(PXE_UNDI_CONTEXT *Context, uint16_t Opcode,
            void *ParamBlock, uint16_t ParamBlockSize)
{
  uint16_t api_exit = 0;
  uint16_t call_status;

  if (Context == NULL || !Context->Ready ||
      Context->Platform.CallApi == NULL || ParamBlock == NULL) {
    return PXE_UNDI_NOT_READY;
  }

  *(uint16_t *)ParamBlock = 0;
  call_status = Context->Platform.CallApi(
    Context->Platform.Context, Opcode, ParamBlock, ParamBlockSize, &api_exit);
  if (call_status != 0 || *(uint16_t *)ParamBlock != 0) {
    return PXE_UNDI_CALL_FAILED;
  }
  return PXE_UNDI_SUCCESS;
}

static PXE_UNDI_FAR_PTR
PxeUndiMakeFarPointer(PXE_UNDI_CONTEXT *Context, const void *HostAddress)
{
  PXE_UNDI_FAR_PTR zero = { 0, 0 };

  if (Context == NULL || HostAddress == NULL ||
      Context->Platform.MakeFarPointer == NULL) {
    return zero;
  }
  return Context->Platform.MakeFarPointer(Context->Platform.Context,
                                          HostAddress);
}

static void *
PxeUndiFarPointerToHost(PXE_UNDI_CONTEXT *Context, PXE_UNDI_FAR_PTR FarPointer)
{
  if (Context == NULL || Context->Platform.FarPointerToHost == NULL) {
    return NULL;
  }
  return Context->Platform.FarPointerToHost(Context->Platform.Context,
                                            FarPointer);
}

int
PxeUndiStartup(PXE_UNDI_CONTEXT *Context)
{
  PXENV_UNDI_SIMPLE_PARAMS params;
  return PxeUndiCall(Context, PXENV_UNDI_STARTUP, &params, sizeof(params));
}

int
PxeUndiInitialize(PXE_UNDI_CONTEXT *Context, const void *ProtocolIni)
{
  PXENV_UNDI_INITIALIZE_PARAMS params;

  memset(&params, 0, sizeof(params));
  params.ProtocolIni = PxeUndiMakeFarPointer(Context, ProtocolIni);
  return PxeUndiCall(Context, PXENV_UNDI_INITIALIZE, &params, sizeof(params));
}

int
PxeUndiGetNicType(PXE_UNDI_CONTEXT *Context,
                  PXENV_UNDI_GET_NIC_TYPE_PARAMS *Out)
{
  int result;

  if (Out == NULL) {
    return PXE_UNDI_INVALID_ARGUMENT;
  }
  memset(Out, 0, sizeof(*Out));
  result = PxeUndiCall(Context, PXENV_UNDI_GET_NIC_TYPE, Out, sizeof(*Out));
  return result;
}

int
PxeUndiGetInformation(PXE_UNDI_CONTEXT *Context,
                      PXENV_UNDI_GET_INFORMATION_PARAMS *Out)
{
  if (Out == NULL) {
    return PXE_UNDI_INVALID_ARGUMENT;
  }
  memset(Out, 0, sizeof(*Out));
  return PxeUndiCall(Context, PXENV_UNDI_GET_INFORMATION, Out, sizeof(*Out));
}

int
PxeUndiSetStationAddress(PXE_UNDI_CONTEXT *Context, const uint8_t Mac[6])
{
  PXENV_UNDI_SET_STATION_ADDRESS_PARAMS params;

  if (Mac == NULL) {
    return PXE_UNDI_INVALID_ARGUMENT;
  }
  memset(&params, 0, sizeof(params));
  memcpy(params.StationAddress, Mac, PXE_UNDI_MAC_SIZE);
  return PxeUndiCall(Context, PXENV_UNDI_SET_STATION_ADDRESS,
                     &params, sizeof(params));
}

int
PxeUndiOpen(PXE_UNDI_CONTEXT *Context, uint16_t Filter)
{
  PXENV_UNDI_OPEN_PARAMS params;

  memset(&params, 0, sizeof(params));
  params.OpenFlag = 0;
  params.PktFilter = Filter;
  return PxeUndiCall(Context, PXENV_UNDI_OPEN, &params, sizeof(params));
}

int
PxeUndiClose(PXE_UNDI_CONTEXT *Context)
{
  PXENV_UNDI_SIMPLE_PARAMS params;
  return PxeUndiCall(Context, PXENV_UNDI_CLOSE, &params, sizeof(params));
}

int
PxeUndiShutdown(PXE_UNDI_CONTEXT *Context)
{
  PXENV_UNDI_SIMPLE_PARAMS params;
  return PxeUndiCall(Context, PXENV_UNDI_SHUTDOWN, &params, sizeof(params));
}

int
PxeUndiSetPacketFilter(PXE_UNDI_CONTEXT *Context, uint16_t Filter)
{
  struct {
    uint16_t Status;
    uint8_t Filter;
  } PXE_UNDI_PACKED params;

  memset(&params, 0, sizeof(params));
  params.Filter = (uint8_t)Filter;
  return PxeUndiCall(Context, PXENV_UNDI_SET_PACKET_FILTER,
                     &params, sizeof(params));
}

int
PxeUndiSetMulticast(PXE_UNDI_CONTEXT *Context,
                    const uint8_t (*MacList)[6], unsigned Count)
{
  struct {
    uint16_t Status;
    PXENV_UNDI_MCAST_ADDRESS Multicast;
  } PXE_UNDI_PACKED params;
  unsigned index;

  if (MacList == NULL && Count != 0) {
    return PXE_UNDI_INVALID_ARGUMENT;
  }

  memset(&params, 0, sizeof(params));
  if (Count > PXE_UNDI_MAX_MCAST_ADDR) {
    Count = PXE_UNDI_MAX_MCAST_ADDR;
  }
  params.Multicast.MCastAddrCount = (uint16_t)Count;
  for (index = 0; index < Count; index++) {
    memcpy(params.Multicast.McastAddr[index], MacList[index],
           PXE_UNDI_MAC_SIZE);
  }
  return PxeUndiCall(Context, PXENV_UNDI_SET_MCAST_ADDRESS,
                     &params, sizeof(params));
}

int
PxeUndiSendFrame(void *Opaque, const uint8_t *Frame, size_t Length)
{
  PXE_UNDI_CONTEXT *Context = (PXE_UNDI_CONTEXT *)Opaque;
  PXENV_UNDI_TRANSMIT_PARAMS params;
  PXENV_UNDI_TBD tbd;
  unsigned index;
  int broadcast = 1;

  if (Context == NULL || Frame == NULL || Length == 0 ||
      Length > sizeof(((PXE_UNDI_FRAME_BUFFER *)0)->Bytes)) {
    return PXE_UNDI_INVALID_ARGUMENT;
  }

  for (index = 0; index < PXE_UNDI_MAC_SIZE; index++) {
    if (Frame[index] != 0xffu) {
      broadcast = 0;
      break;
    }
  }

  memset(&params, 0, sizeof(params));
  memset(&tbd, 0, sizeof(tbd));
  tbd.ImmedLength = (uint16_t)Length;
  tbd.Xmit = PxeUndiMakeFarPointer(Context, Frame);
  tbd.DataBlkCount = 0;

  params.Protocol = PXE_UNDI_PROTO_UNKNOWN;
  params.XmitFlag = broadcast ? PXE_UNDI_XMT_BROADCAST : PXE_UNDI_XMT_DESTADDR;
  params.DestAddr = PxeUndiMakeFarPointer(Context, Frame);
  params.TBD = PxeUndiMakeFarPointer(Context, &tbd);
  params.Reserved = 0;

  return PxeUndiCall(Context, PXENV_UNDI_TRANSMIT, &params, sizeof(params));
}

int
PxeUndiReceiveFrame(void *Opaque, uint8_t *Frame, size_t *Length)
{
  PXE_UNDI_CONTEXT *Context = (PXE_UNDI_CONTEXT *)Opaque;
  PXENV_UNDI_ISR_PARAMS params;
  size_t capacity;
  uint16_t received;
  void *host_buffer;
  int result;

  if (Context == NULL || Frame == NULL || Length == NULL) {
    return PXE_UNDI_INVALID_ARGUMENT;
  }
  capacity = *Length;
  *Length = 0;
  if (capacity == 0 || capacity > 0xffffu) {
    return PXE_UNDI_BUFFER_TOO_SMALL;
  }

  memset(&params, 0, sizeof(params));
  params.FuncFlag = PXE_UNDI_ISR_START;
  params.Frame = PxeUndiMakeFarPointer(Context, Frame);
  params.FrameLength = (uint16_t)capacity;

  result = PxeUndiCall(Context, PXENV_UNDI_RECEIVE, &params, sizeof(params));
  if (result != PXE_UNDI_SUCCESS) {
    return result;
  }

  received = params.FrameLength;
  if (received == 0) {
    return PXE_UNDI_NO_PACKET;
  }
  if (received > capacity) {
    return PXE_UNDI_BUFFER_TOO_SMALL;
  }

  host_buffer = PxeUndiFarPointerToHost(Context, params.Frame);
  if (host_buffer == NULL || host_buffer != (void *)Frame) {
    return PXE_UNDI_INVALID_ARGUMENT;
  }

  *Length = received;
  return PXE_UNDI_SUCCESS;
}

void
PxeUndiInstallNetOps(void)
{
  gCephBiosPxeUndiOps.Discover = PxeUndiDiscover;
  gCephBiosPxeUndiOps.Startup = PxeUndiStartup;
  gCephBiosPxeUndiOps.Initialize = PxeUndiInitialize;
  gCephBiosPxeUndiOps.GetNicType = PxeUndiGetNicType;
  gCephBiosPxeUndiOps.GetInformation = PxeUndiGetInformation;
  gCephBiosPxeUndiOps.SetStationAddress = PxeUndiSetStationAddress;
  gCephBiosPxeUndiOps.Open = PxeUndiOpen;
  gCephBiosPxeUndiOps.Close = PxeUndiClose;
  gCephBiosPxeUndiOps.Shutdown = PxeUndiShutdown;
  gCephBiosPxeUndiOps.SetPacketFilter = PxeUndiSetPacketFilter;
  gCephBiosPxeUndiOps.SetMulticast = PxeUndiSetMulticast;
  gCephBiosPxeUndiOps.SendFrame = PxeUndiSendFrame;
  gCephBiosPxeUndiOps.ReceiveFrame = PxeUndiReceiveFrame;
}
