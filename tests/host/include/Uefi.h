#ifndef TEST_UEFI_H_
#define TEST_UEFI_H_

#include <stddef.h>
#include <stdint.h>

#define IN
#define OUT
#define OPTIONAL
#define CONST const
#define STATIC static

typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef size_t UINTN;
typedef int32_t INT32;
typedef char CHAR8;
typedef void VOID;
typedef int EFI_STATUS;

#define MAX_UINT32 UINT32_MAX
#define MAX_UINT64 UINT64_MAX
#define MAX_UINTN SIZE_MAX

#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER (-1)
#define EFI_BAD_BUFFER_SIZE (-2)
#define EFI_BUFFER_TOO_SMALL (-3)
#define EFI_UNSUPPORTED (-4)
#define EFI_COMPROMISED_DATA (-5)
#define EFI_CRC_ERROR (-6)
#define EFI_ABORTED (-7)
#define EFI_ACCESS_DENIED (-8)
#define EFI_ERROR(Status) ((Status) != EFI_SUCCESS)

static inline UINT64
LShiftU64(UINT64 Value, UINTN Shift)
{
  return Value << Shift;
}

#endif
