#include "ceph_stage2.h"
#include "ceph_stage2_network.h"

/*
 * PXE/UNDI operations table. It starts empty so that the disk-only core has
 * no link-time dependency on the PXE module; PxeUndiInstallNetOps() fills it
 * when the PXE driver is present.
 */
PXE_UNDI_NET_OPS gCephBiosPxeUndiOps = { 0 };

static PXE_UNDI_CONTEXT mPxeUndiContext;

static uint32_t
ReadLe32(const uint8_t *Data)
{
  return (uint32_t)Data[0]
         | ((uint32_t)Data[1] << 8)
         | ((uint32_t)Data[2] << 16)
         | ((uint32_t)Data[3] << 24);
}

int
CephBiosStage2LoadPartitionSector(
  CEPH_BIOS_READ_AT ReadAt,
  void *Context,
  uint64_t ImageSizeBytes,
  uint8_t BootSector[CEPH_BIOS_SECTOR_SIZE],
  uint32_t *PartitionStartLba
  )
{
  uint8_t mbr[CEPH_BIOS_SECTOR_SIZE];
  const uint8_t *active = NULL;
  uint32_t start_lba;
  uint32_t sectors;
  uint64_t offset;
  unsigned int index;

  if (ReadAt == NULL || BootSector == NULL || PartitionStartLba == NULL ||
      ImageSizeBytes < CEPH_BIOS_SECTOR_SIZE) {
    return CEPH_BIOS_INVALID_ARGUMENT;
  }

  if (ReadAt(Context, 0, mbr, sizeof(mbr)) != 0) {
    return CEPH_BIOS_IO_ERROR;
  }

  if (mbr[510] != 0x55 || mbr[511] != 0xaa) {
    return CEPH_BIOS_INVALID_MBR;
  }

  for (index = 0; index < 4; index++) {
    const uint8_t *entry = mbr + 446 + index * 16;
    if (entry[0] != 0 && entry[0] != 0x80) {
      return CEPH_BIOS_INVALID_MBR;
    }

    if (entry[4] == 0 &&
        (entry[0] != 0 || ReadLe32(entry + 8) != 0 || ReadLe32(entry + 12) != 0)) {
      return CEPH_BIOS_INVALID_MBR;
    }

    if (entry[0] == 0x80) {
      if (entry[4] == 0) {
        return CEPH_BIOS_INVALID_MBR;
      }

      if (active != NULL) {
        return CEPH_BIOS_INVALID_MBR;
      }
      active = entry;
    }
  }

  if (active == NULL) {
    return CEPH_BIOS_NO_ACTIVE_PARTITION;
  }

  start_lba = ReadLe32(active + 8);
  sectors = ReadLe32(active + 12);
  if (start_lba == 0 || sectors == 0 ||
      (uint64_t)start_lba > ImageSizeBytes / CEPH_BIOS_SECTOR_SIZE ||
      (uint64_t)sectors > ImageSizeBytes / CEPH_BIOS_SECTOR_SIZE - start_lba) {
    return CEPH_BIOS_OUT_OF_RANGE;
  }

  offset = (uint64_t)start_lba * CEPH_BIOS_SECTOR_SIZE;
  if (ReadAt(Context, offset, BootSector, CEPH_BIOS_SECTOR_SIZE) != 0) {
    return CEPH_BIOS_IO_ERROR;
  }

  if (BootSector[510] != 0x55 || BootSector[511] != 0xaa) {
    return CEPH_BIOS_INVALID_BOOT_SECTOR;
  }

  *PartitionStartLba = start_lba;
  return CEPH_BIOS_SUCCESS;
}

int
CephBiosStage2InitNetwork(
  const PXE_UNDI_PLATFORM *Platform,
  CEPH_BIOS_NETWORK *Network
  )
{
  PXE_UNDI_NET_OPS *ops = &gCephBiosPxeUndiOps;
  PXENV_UNDI_GET_NIC_TYPE_PARAMS nic;
  PXENV_UNDI_GET_INFORMATION_PARAMS info;
  uint16_t filter;
  int result;

  if (Network == NULL) {
    return CEPH_BIOS_INVALID_ARGUMENT;
  }
  Network->Send = NULL;
  Network->Receive = NULL;
  Network->Context = NULL;

  /* No registered PXE/UNDI stack: keep the original disk-only behaviour. */
  if (Platform == NULL || ops->Discover == NULL || ops->Startup == NULL ||
      ops->Initialize == NULL || ops->GetNicType == NULL ||
      ops->GetInformation == NULL || ops->SetStationAddress == NULL ||
      ops->Open == NULL || ops->SendFrame == NULL ||
      ops->ReceiveFrame == NULL) {
    return CEPH_BIOS_NETWORK_NOT_AVAILABLE;
  }

  result = ops->Discover(Platform, &mPxeUndiContext);
  if (result != PXE_UNDI_SUCCESS) {
    return CEPH_BIOS_NETWORK_NOT_AVAILABLE;
  }

  result = ops->Startup(&mPxeUndiContext);
  if (result != PXE_UNDI_SUCCESS) {
    goto fail;
  }
  result = ops->Initialize(&mPxeUndiContext, NULL);
  if (result != PXE_UNDI_SUCCESS) {
    goto fail;
  }
  result = ops->GetNicType(&mPxeUndiContext, &nic);
  if (result != PXE_UNDI_SUCCESS) {
    goto fail;
  }
  result = ops->GetInformation(&mPxeUndiContext, &info);
  if (result != PXE_UNDI_SUCCESS) {
    goto fail;
  }
  result = ops->SetStationAddress(&mPxeUndiContext, info.PermNodeAddress);
  if (result != PXE_UNDI_SUCCESS) {
    goto fail;
  }

  filter = PXE_UNDI_FLTR_DIRECTED | PXE_UNDI_FLTR_BROADCAST;
  result = ops->Open(&mPxeUndiContext, filter);
  if (result != PXE_UNDI_SUCCESS) {
    goto fail;
  }

  Network->Send = ops->SendFrame;
  Network->Receive = ops->ReceiveFrame;
  Network->Context = &mPxeUndiContext;
  return CEPH_BIOS_SUCCESS;

fail:
  if (ops->Shutdown != NULL) {
    ops->Shutdown(&mPxeUndiContext);
  }
  return CEPH_BIOS_IO_ERROR;
}

int
CephBiosStage2ShutdownNetwork(
  CEPH_BIOS_NETWORK *Network
  )
{
  PXE_UNDI_NET_OPS *ops = &gCephBiosPxeUndiOps;
  int status = CEPH_BIOS_SUCCESS;
  int result;

  if (Network == NULL || Network->Context == NULL) {
    return CEPH_BIOS_INVALID_ARGUMENT;
  }

  if (ops->Close != NULL) {
    result = ops->Close(&mPxeUndiContext);
    if (result != PXE_UNDI_SUCCESS) {
      status = CEPH_BIOS_IO_ERROR;
    }
  }
  if (ops->Shutdown != NULL) {
    result = ops->Shutdown(&mPxeUndiContext);
    if (result != PXE_UNDI_SUCCESS) {
      status = CEPH_BIOS_IO_ERROR;
    }
  }

  Network->Send = NULL;
  Network->Receive = NULL;
  Network->Context = NULL;
  return status;
}

int
CephBiosStage2SendFrame(
  CEPH_BIOS_NETWORK *Network,
  const uint8_t *Frame,
  size_t Length
  )
{
  if (Network == NULL || Network->Send == NULL) {
    return CEPH_BIOS_NETWORK_NOT_AVAILABLE;
  }
  if (Network->Send(Network->Context, Frame, Length) != PXE_UNDI_SUCCESS) {
    return CEPH_BIOS_IO_ERROR;
  }
  return CEPH_BIOS_SUCCESS;
}

int
CephBiosStage2ReceiveFrame(
  CEPH_BIOS_NETWORK *Network,
  uint8_t *Frame,
  size_t *Length
  )
{
  int result;

  if (Network == NULL || Network->Receive == NULL) {
    return CEPH_BIOS_NETWORK_NOT_AVAILABLE;
  }
  result = Network->Receive(Network->Context, Frame, Length);
  if (result == PXE_UNDI_NO_PACKET) {
    return CEPH_BIOS_NETWORK_NOT_AVAILABLE;
  }
  if (result != PXE_UNDI_SUCCESS) {
    return CEPH_BIOS_IO_ERROR;
  }
  return CEPH_BIOS_SUCCESS;
}
