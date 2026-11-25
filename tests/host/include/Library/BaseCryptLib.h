#ifndef TEST_BASE_CRYPT_LIB_H_
#define TEST_BASE_CRYPT_LIB_H_

#include <Uefi.h>

UINTN
AesGetContextSize (
  VOID
  );

BOOLEAN
AesInit (
  OUT VOID        *AesContext,
  IN CONST UINT8  *Key,
  IN UINTN        KeyLength
  );

BOOLEAN
AesCbcEncrypt (
  IN VOID         *AesContext,
  IN CONST UINT8  *Input,
  IN UINTN        InputSize,
  IN CONST UINT8  *Ivec,
  OUT UINT8       *Output
  );

#endif
