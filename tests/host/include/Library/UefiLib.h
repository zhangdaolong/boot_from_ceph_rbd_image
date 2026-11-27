#ifndef _UEFI_LIB_H_
#define _UEFI_LIB_H_

// Host unit-test stub for <Library/UefiLib.h>. The firmware diagnostics use
// Print() with UEFI format specifiers (%r/%a/%p); host tests do not assert on
// console output, so compile it to a no-op.
#define Print(...) ((VOID)0)

#endif
