#include <openssl/evp.h>

#include <string.h>

#include <Library/BaseCryptLib.h>

typedef struct {
  UINT8 Key[16];
} TEST_AES_CONTEXT;

UINTN
AesGetContextSize (
  VOID
  )
{
  return sizeof (TEST_AES_CONTEXT);
}

BOOLEAN
AesInit (
  OUT VOID        *AesContext,
  IN CONST UINT8  *Key,
  IN UINTN        KeyLength
  )
{
  TEST_AES_CONTEXT *Context;

  if ((AesContext == NULL) || (Key == NULL) || (KeyLength != 128)) {
    return FALSE;
  }

  Context = AesContext;
  memcpy (Context->Key, Key, sizeof (Context->Key));
  return TRUE;
}

BOOLEAN
AesCbcEncrypt (
  IN VOID         *AesContext,
  IN CONST UINT8  *Input,
  IN UINTN        InputSize,
  IN CONST UINT8  *Ivec,
  OUT UINT8       *Output
  )
{
  TEST_AES_CONTEXT *Context;
  EVP_CIPHER_CTX   *CipherContext;
  int              OutputLength;
  int              FinalLength;
  BOOLEAN          Result;

  if ((AesContext == NULL) || (Input == NULL) || (Ivec == NULL) ||
      (Output == NULL) || ((InputSize & 15) != 0) || (InputSize > INT_MAX))
  {
    return FALSE;
  }

  Context = AesContext;
  CipherContext = EVP_CIPHER_CTX_new ();
  if (CipherContext == NULL) {
    return FALSE;
  }

  Result = (BOOLEAN)(
             (EVP_EncryptInit_ex (
                CipherContext, EVP_aes_128_cbc (), NULL,
                Context->Key, Ivec) == 1) &&
             (EVP_CIPHER_CTX_set_padding (CipherContext, 0) == 1) &&
             (EVP_EncryptUpdate (
                CipherContext, Output, &OutputLength,
                Input, (int)InputSize) == 1) &&
             (EVP_EncryptFinal_ex (
                CipherContext, Output + OutputLength, &FinalLength) == 1) &&
             ((UINTN)(OutputLength + FinalLength) == InputSize));
  EVP_CIPHER_CTX_free (CipherContext);
  return Result;
}
