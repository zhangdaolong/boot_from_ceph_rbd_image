#ifndef TEST_DRIVER_BINDING_H_
#define TEST_DRIVER_BINDING_H_

#include <Uefi.h>

typedef struct _EFI_DEVICE_PATH_PROTOCOL EFI_DEVICE_PATH_PROTOCOL;

extern EFI_GUID gEfiDriverBindingProtocolGuid;

typedef struct _EFI_DRIVER_BINDING_PROTOCOL EFI_DRIVER_BINDING_PROTOCOL;

struct _EFI_DRIVER_BINDING_PROTOCOL {
  EFI_STATUS (EFIAPI *Supported)(
    EFI_DRIVER_BINDING_PROTOCOL *This,
    EFI_HANDLE ControllerHandle,
    EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath
    );
  EFI_STATUS (EFIAPI *Start)(
    EFI_DRIVER_BINDING_PROTOCOL *This,
    EFI_HANDLE ControllerHandle,
    EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath
    );
  EFI_STATUS (EFIAPI *Stop)(
    EFI_DRIVER_BINDING_PROTOCOL *This,
    EFI_HANDLE ControllerHandle,
    UINTN NumberOfChildren,
    EFI_HANDLE *ChildHandleBuffer
    );
  UINT32 Version;
  EFI_HANDLE ImageHandle;
  EFI_HANDLE DriverBindingHandle;
};

#endif
