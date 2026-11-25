#ifndef TEST_UEFI_H_
#define TEST_UEFI_H_

#include <stddef.h>
#include <stdint.h>

#define IN
#define OUT
#define OPTIONAL
#define CONST const
#define STATIC static
#define EFIAPI

typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef int32_t INT32;
typedef char CHAR8;
typedef uint8_t BOOLEAN;
typedef void VOID;
typedef int EFI_STATUS;
typedef VOID *EFI_HANDLE;
typedef VOID *EFI_EVENT;
typedef UINTN EFI_TPL;

typedef struct {
  UINT32  Data1;
  UINT16  Data2;
  UINT16  Data3;
  UINT8   Data4[8];
} EFI_GUID;

typedef struct {
  UINT8  Addr[4];
} EFI_IPv4_ADDRESS;

typedef VOID (EFIAPI *EFI_EVENT_NOTIFY)(EFI_EVENT Event, VOID *Context);

#define MAX_UINT32 UINT32_MAX
#define MAX_UINT64 UINT64_MAX
#define MAX_UINTN SIZE_MAX
#define TRUE 1
#define FALSE 0

#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER (-1)
#define EFI_BAD_BUFFER_SIZE (-2)
#define EFI_BUFFER_TOO_SMALL (-3)
#define EFI_UNSUPPORTED (-4)
#define EFI_COMPROMISED_DATA (-5)
#define EFI_CRC_ERROR (-6)
#define EFI_ABORTED (-7)
#define EFI_ACCESS_DENIED (-8)
#define EFI_OUT_OF_RESOURCES (-9)
#define EFI_NOT_STARTED (-10)
#define EFI_TIMEOUT (-11)
#define EFI_NOT_FOUND (-12)
#define EFI_DEVICE_ERROR (-13)
#define EFI_NOT_READY (-14)
#define EFI_CONNECTION_FIN (-15)
#define EFI_ERROR(Status) ((Status) != EFI_SUCCESS)

#define EFI_OPEN_PROTOCOL_GET_PROTOCOL  0x00000002U
#define EVT_TIMER                       0x80000000U
#define EVT_NOTIFY_SIGNAL               0x00000200U
#define TPL_APPLICATION                 4
#define TPL_CALLBACK                    8

typedef enum {
  TimerCancel,
  TimerPeriodic,
  TimerRelative
} EFI_TIMER_DELAY;

typedef struct _EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
struct _EFI_BOOT_SERVICES {
  EFI_STATUS (*CreateEvent)(
    UINT32 Type,
    EFI_TPL NotifyTpl,
    EFI_EVENT_NOTIFY NotifyFunction,
    VOID *NotifyContext,
    EFI_EVENT *Event
    );
  EFI_STATUS (*SetTimer)(EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 TriggerTime);
  EFI_STATUS (*WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
  EFI_STATUS (*CheckEvent)(EFI_EVENT Event);
  EFI_STATUS (*CloseEvent)(EFI_EVENT Event);
  EFI_STATUS (*OpenProtocol)(
    EFI_HANDLE Handle,
    CONST EFI_GUID *Protocol,
    VOID **Interface,
    EFI_HANDLE AgentHandle,
    EFI_HANDLE ControllerHandle,
    UINT32 Attributes
    );
  EFI_STATUS (*CloseProtocol)(
    EFI_HANDLE Handle,
    CONST EFI_GUID *Protocol,
    EFI_HANDLE AgentHandle,
    EFI_HANDLE ControllerHandle
    );
};

extern EFI_BOOT_SERVICES *gBS;

static inline UINT64
LShiftU64(UINT64 Value, UINTN Shift)
{
  return Value << Shift;
}

#endif
