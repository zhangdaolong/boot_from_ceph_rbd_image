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

BOOLEAN
AesCbcDecrypt (
  IN VOID         *AesContext,
  IN CONST UINT8  *Input,
  IN UINTN        InputSize,
  IN CONST UINT8  *Ivec,
  OUT UINT8       *Output
  );

BOOLEAN
AesEncrypt (
  IN VOID         *AesContext,
  IN CONST UINT8  *Input,
  OUT UINT8       *Output
  );

BOOLEAN
AesDecrypt (
  IN VOID         *AesContext,
  IN CONST UINT8  *Input,
  OUT UINT8       *Output
  );

VOID *
HmacSha384New (
  VOID
  );

VOID
HmacSha384Free (
  IN VOID  *HmacSha384Ctx
  );

BOOLEAN
HmacSha384SetKey (
  OUT VOID        *HmacSha384Context,
  IN CONST UINT8  *Key,
  IN UINTN        KeySize
  );

BOOLEAN
HmacSha384Update (
  IN OUT VOID     *HmacSha384Context,
  IN CONST VOID   *Data,
  IN UINTN        DataSize
  );

BOOLEAN
HmacSha384Final (
  IN OUT VOID   *HmacSha384Context,
  OUT UINT8     *HmacValue
  );

#endif
