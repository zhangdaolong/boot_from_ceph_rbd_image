#ifndef TEST_BASE_LIB_H_
#define TEST_BASE_LIB_H_

// The host Uefi.h stub provides the small BaseLib helpers (LShiftU64, etc.)
// that the firmware sources use, so this header only needs to exist to
// satisfy the EDK II include path during host compiles.
#include <Uefi.h>

#endif
