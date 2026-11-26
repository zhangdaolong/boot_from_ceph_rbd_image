#ifndef TEST_DEVICE_PATH_H_
#define TEST_DEVICE_PATH_H_

#include <Uefi.h>

// Host-side stand-in for MdePkg/Include/Protocol/DevicePath.h. The Boot
// Manager handoff layer needs the header fields, not just a forward
// declaration, to build HD(GPT)+FilePath device paths byte-for-byte.
typedef struct _EFI_DEVICE_PATH_PROTOCOL {
  UINT8  Type;
  UINT8  SubType;
  UINT8  Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

#endif
