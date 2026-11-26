// Host-side tests for the GPT/ESP -> UEFI Boot Manager handoff layer.
// Compiled with -DCEPH_RBD_BOOT_MANAGER_HOST_TEST and its own main().
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CephRbdBootManager.h"

#define MAX_MOCK_VARS    16
#define MOCK_VAR_DATA    512
#define MAX_CALL_LOG     16

typedef struct {
  CHAR16   Name[16];
  EFI_GUID Guid;
  UINT32   Attributes;
  UINT8    Data[MOCK_VAR_DATA];
  UINTN    Size;
  int      Present;
} MOCK_VAR;

typedef struct {
  MOCK_VAR Vars[MAX_MOCK_VARS];

  int          OutstandingAllocs;
  int          SetVariableCalls;
  CHAR16       LastSetName[16];
  UINT32       LastSetAttributes;
  UINTN        LastSetSize;
  EFI_STATUS   SetVariableFailStatus;

  int          LoadImageCalls;
  int          StartImageCalls;
  int          UnloadImageCalls;
  EFI_STATUS   LoadImageStatus;
  EFI_STATUS   StartImageStatus;
  BOOLEAN      LastBootPolicy;
  EFI_HANDLE   LastParentHandle;
  CONST EFI_DEVICE_PATH_PROTOCOL  *LastLoadImageDp;
  VOID         *LastSourceBuffer;
  UINTN        LastSourceSize;
  EFI_HANDLE   LoadedHandle;
  EFI_HANDLE   LastStartedHandle;
  EFI_HANDLE   LastUnloadedHandle;
  int          CallLog[MAX_CALL_LOG];
  int          CallLogCount;
} MOCK_ENV;

STATIC CONST EFI_GUID  mMockGlobalGuid = {
  0x8be4df61, 0x93ca, 0x11d2,
  { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c }
};

STATIC CONST UINT8  mSignatureBytes[16] = {
  0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
  0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00
};

STATIC CONST CHAR16  mExpectedLoaderPath[] = {
  '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\',
  'B', 'O', 'O', 'T', 'X', '6', '4', '.', 'E', 'F', 'I', 0
};

STATIC CONST CHAR16  mExpectedDescription[] = {
  'C', 'e', 'p', 'h', ' ', 'R', 'B', 'D', ' ',
  '(', 'G', 'P', 'T', '/', 'E', 'S', 'P', ')', 0
};

static MOCK_ENV  mEnv;

static int
NameEquals (
  CONST CHAR16  *A,
  CONST CHAR16  *B
  )
{
  while ((*A != 0) && (*B != 0)) {
    if (*A != *B) {
      return 0;
    }
    A++;
    B++;
  }
  return *A == *B;
}

static void
CopyName (
  CHAR16        *Destination,
  CONST CHAR16  *Source,
  UINTN          Capacity
  )
{
  UINTN  Index;

  for (Index = 0; (Index < (Capacity - 1)) && (Source[Index] != 0); Index++) {
    Destination[Index] = Source[Index];
  }
  Destination[Index] = 0;
}

static MOCK_VAR *
FindVar (
  CONST CHAR16   *Name,
  CONST EFI_GUID *Guid
  )
{
  int  Index;

  for (Index = 0; Index < MAX_MOCK_VARS; Index++) {
    if (mEnv.Vars[Index].Present &&
        NameEquals (mEnv.Vars[Index].Name, Name) &&
        (memcmp (&mEnv.Vars[Index].Guid, Guid, sizeof (EFI_GUID)) == 0))
    {
      return &mEnv.Vars[Index];
    }
  }
  return NULL;
}

static void
PutVarRaw (
  CONST CHAR16   *Name,
  CONST EFI_GUID *Guid,
  UINT32          Attributes,
  CONST VOID     *Data,
  UINTN           Size
  )
{
  int        Index;
  MOCK_VAR  *Var;

  Var = FindVar (Name, Guid);
  if (Var == NULL) {
    for (Index = 0; Index < MAX_MOCK_VARS; Index++) {
      if (!mEnv.Vars[Index].Present) {
        Var = &mEnv.Vars[Index];
        break;
      }
    }
  }
  assert (Var != NULL);
  assert (Size <= MOCK_VAR_DATA);

  memset (Var, 0, sizeof (*Var));
  CopyName (Var->Name, Name, 16);
  memcpy (&Var->Guid, Guid, sizeof (EFI_GUID));
  Var->Attributes = Attributes;
  if ((Data != NULL) && (Size != 0)) {
    memcpy (Var->Data, Data, Size);
  }
  Var->Size = Size;
  Var->Present = 1;
}

static EFI_STATUS
EFIAPI
MockGetVariable (
  IN     CONST CHAR16    *VariableName,
  IN     CONST EFI_GUID  *VendorGuid,
  OUT OPTIONAL UINT32    *Attributes,
  IN OUT UINTN           *DataSize,
  OUT OPTIONAL VOID      *Data
  )
{
  MOCK_VAR  *Var;

  Var = FindVar (VariableName, VendorGuid);
  if (Var == NULL) {
    return EFI_NOT_FOUND;
  }
  if (Attributes != NULL) {
    *Attributes = Var->Attributes;
  }
  if (Data == NULL) {
    *DataSize = Var->Size;
    return EFI_BUFFER_TOO_SMALL;
  }
  if (*DataSize < Var->Size) {
    *DataSize = Var->Size;
    return EFI_BUFFER_TOO_SMALL;
  }
  memcpy (Data, Var->Data, Var->Size);
  *DataSize = Var->Size;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockSetVariable (
  IN CONST CHAR16    *VariableName,
  IN CONST EFI_GUID  *VendorGuid,
  IN UINT32           Attributes,
  IN UINTN            DataSize,
  IN CONST VOID      *Data
  )
{
  MOCK_VAR  *Var;

  mEnv.SetVariableCalls++;
  CopyName (mEnv.LastSetName, VariableName, 16);
  mEnv.LastSetAttributes = Attributes;
  mEnv.LastSetSize = DataSize;
  if (mEnv.SetVariableFailStatus != EFI_SUCCESS) {
    return mEnv.SetVariableFailStatus;
  }

  Var = FindVar (VariableName, VendorGuid);
  if ((DataSize == 0) && (Data == NULL)) {
    if (Var != NULL) {
      Var->Present = 0;
    }
    return EFI_SUCCESS;
  }
  assert (Data != NULL);
  assert (DataSize <= MOCK_VAR_DATA);
  assert (memcmp (VendorGuid, &mMockGlobalGuid, sizeof (EFI_GUID)) == 0);

  if (Var == NULL) {
    int  Index;
    for (Index = 0; Index < MAX_MOCK_VARS; Index++) {
      if (!mEnv.Vars[Index].Present) {
        Var = &mEnv.Vars[Index];
        break;
      }
    }
  }
  assert (Var != NULL);
  memset (Var, 0, sizeof (*Var));
  CopyName (Var->Name, VariableName, 16);
  memcpy (&Var->Guid, VendorGuid, sizeof (EFI_GUID));
  Var->Attributes = Attributes;
  memcpy (Var->Data, Data, DataSize);
  Var->Size = DataSize;
  Var->Present = 1;
  return EFI_SUCCESS;
}

static VOID *
EFIAPI
MockPoolAllocate (
  IN UINTN  Size
  )
{
  mEnv.OutstandingAllocs++;
  return malloc (Size);
}

static VOID *
EFIAPI
MockPoolAllocateZero (
  IN UINTN  Size
  )
{
  mEnv.OutstandingAllocs++;
  return calloc (1, Size);
}

static VOID
EFIAPI
MockPoolFree (
  IN VOID  *Buffer
  )
{
  if (Buffer != NULL) {
    mEnv.OutstandingAllocs--;
    free (Buffer);
  }
}

static void
AppendCall (
  int  Code
  )
{
  if (mEnv.CallLogCount < MAX_CALL_LOG) {
    mEnv.CallLog[mEnv.CallLogCount++] = Code;
  }
}

static EFI_STATUS
EFIAPI
MockLoadImage (
  IN     BOOLEAN                        BootPolicy,
  IN     EFI_HANDLE                     ParentImageHandle,
  IN     CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN     VOID                           *SourceBuffer OPTIONAL,
  IN     UINTN                          SourceSize,
  OUT    EFI_HANDLE                     *ImageHandle
  )
{
  AppendCall (1);
  mEnv.LoadImageCalls++;
  mEnv.LastBootPolicy = BootPolicy;
  mEnv.LastParentHandle = ParentImageHandle;
  mEnv.LastLoadImageDp = DevicePath;
  mEnv.LastSourceBuffer = SourceBuffer;
  mEnv.LastSourceSize = SourceSize;
  if (EFI_ERROR (mEnv.LoadImageStatus)) {
    return mEnv.LoadImageStatus;
  }
  *ImageHandle = mEnv.LoadedHandle;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockStartImage (
  IN     EFI_HANDLE    ImageHandle,
  OUT OPTIONAL UINTN   *ExitDataSize,
  OUT OPTIONAL CHAR16  **ExitData
  )
{
  (void)ExitDataSize;
  (void)ExitData;
  AppendCall (2);
  mEnv.StartImageCalls++;
  mEnv.LastStartedHandle = ImageHandle;
  return mEnv.StartImageStatus;
}

static EFI_STATUS
EFIAPI
MockUnloadImage (
  IN EFI_HANDLE  ImageHandle
  )
{
  AppendCall (3);
  mEnv.UnloadImageCalls++;
  mEnv.LastUnloadedHandle = ImageHandle;
  return EFI_SUCCESS;
}

static void
InitBackend (
  CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend
  )
{
  memset (Backend, 0, sizeof (*Backend));
  Backend->GetVariable = MockGetVariable;
  Backend->SetVariable = MockSetVariable;
  Backend->PoolAllocate = MockPoolAllocate;
  Backend->PoolAllocateZero = MockPoolAllocateZero;
  Backend->PoolFree = MockPoolFree;
  Backend->LoadImage = MockLoadImage;
  Backend->StartImage = MockStartImage;
  Backend->UnloadImage = MockUnloadImage;
}

static void
ResetEnv (
  void
  )
{
  assert (mEnv.OutstandingAllocs == 0);
  memset (&mEnv, 0, sizeof (mEnv));
  mEnv.LoadedHandle = (EFI_HANDLE)(uintptr_t)0x1234;
}

static UINT32
ReadLe32 (
  CONST UINT8  *Buffer
  )
{
  return (UINT32)Buffer[0] |
         ((UINT32)Buffer[1] << 8) |
         ((UINT32)Buffer[2] << 16) |
         ((UINT32)Buffer[3] << 24);
}

static UINT64
ReadLe64 (
  CONST UINT8  *Buffer
  )
{
  return (UINT64)ReadLe32 (Buffer) |
         ((UINT64)ReadLe32 (Buffer + 4) << 32);
}

static void
CheckBootOrder (
  CONST UINT16  *Expected,
  UINTN          Count
  )
{
  static CONST CHAR16  BootOrderName[] = {
    'B', 'o', 'o', 't', 'O', 'r', 'd', 'e', 'r', 0
  };
  MOCK_VAR  *Var;
  UINTN      Index;

  Var = FindVar (BootOrderName, &mMockGlobalGuid);
  assert (Var != NULL);
  assert (Var->Size == Count * 2);
  for (Index = 0; Index < Count; Index++) {
    assert ((UINT16)(Var->Data[Index * 2] |
            ((UINT16)Var->Data[(Index * 2) + 1] << 8)) == Expected[Index]);
  }
}

static MOCK_VAR *
FindBootVar (
  UINT16  Index
  )
{
  CHAR16  Name[9];
  CONST CHAR16  Hex[] = { '0', '1', '2', '3', '4', '5', '6', '7',
                          '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };

  Name[0] = 'B';
  Name[1] = 'o';
  Name[2] = 'o';
  Name[3] = 't';
  Name[4] = Hex[(Index >> 12) & 0xf];
  Name[5] = Hex[(Index >> 8) & 0xf];
  Name[6] = Hex[(Index >> 4) & 0xf];
  Name[7] = Hex[Index & 0xf];
  Name[8] = 0;
  return FindVar (Name, &mMockGlobalGuid);
}

static EFI_DEVICE_PATH_PROTOCOL *
BuildTestDevicePath (
  IN     CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  OUT    UINTN                          *Size
  )
{
  EFI_GUID                  Signature;
  EFI_DEVICE_PATH_PROTOCOL  *Dp;

  CephRbdBootManagerGuidFromGptBytes (mSignatureBytes, &Signature);
  assert (memcmp (&Signature, mSignatureBytes, 16) == 0);
  assert (CephRbdBootManagerBuildLoaderDevicePath (
            Backend, 2, UINT64_C(0x1000), UINT64_C(0x200),
            &Signature, NULL, &Dp, Size) == EFI_SUCCESS);
  return Dp;
}

static void
TestDevicePath (
  void
  )
{
  CEPH_RBD_BOOT_MANAGER_BACKEND  Backend;
  EFI_DEVICE_PATH_PROTOCOL      *Dp;
  UINTN                          Size;
  UINT8                         *Bytes;
  UINTN                          Index;
  EFI_GUID                       Signature;
  CHAR16                         AltPath[] = { '\\', 'X', '\\', 'Y', 0 };

  ResetEnv ();
  InitBackend (&Backend);
  Dp = BuildTestDevicePath (&Backend, &Size);
  assert (Size == 94);
  Bytes = (UINT8 *)Dp;

  // HD(GPT) node.
  assert (Bytes[0] == 0x01 && Bytes[1] == 0x01);
  assert (Bytes[2] == 42 && Bytes[3] == 0);
  assert (ReadLe32 (Bytes + 4) == 2);
  assert (ReadLe64 (Bytes + 8) == UINT64_C(0x1000));
  assert (ReadLe64 (Bytes + 16) == UINT64_C(0x200));
  assert (memcmp (Bytes + 24, mSignatureBytes, 16) == 0);
  assert (Bytes[40] == 0x02 && Bytes[41] == 0x02);

  // FilePath node \EFI\BOOT\BOOTX64.EFI.
  assert (Bytes[42] == 0x01 && Bytes[43] == 0x04);
  assert (Bytes[44] == 48 && Bytes[45] == 0);
  for (Index = 0; Index < 22; Index++) {
    assert ((UINT16)(Bytes[46 + (Index * 2)] |
            ((UINT16)Bytes[47 + (Index * 2)] << 8)) == mExpectedLoaderPath[Index]);
  }

  // End node.
  assert (Bytes[90] == 0x7f && Bytes[91] == 0xff);
  assert (Bytes[92] == 4 && Bytes[93] == 0);
  assert (CephRbdBootManagerFreeDevicePath (&Backend, Dp) == EFI_SUCCESS);

  // Alternate loader path.
  CephRbdBootManagerGuidFromGptBytes (mSignatureBytes, &Signature);
  assert (CephRbdBootManagerBuildLoaderDevicePath (
            &Backend, 1, 0, 0, &Signature, AltPath, &Dp, &Size) == EFI_SUCCESS);
  assert (Size == 42 + 14 + 4);
  Bytes = (UINT8 *)Dp;
  assert (Bytes[44] == 14 && Bytes[45] == 0);
  assert ((UINT16)(Bytes[46] | ((UINT16)Bytes[47] << 8)) == '\\');
  assert ((UINT16)(Bytes[48] | ((UINT16)Bytes[49] << 8)) == 'X');

  assert (CephRbdBootManagerBuildLoaderDevicePath (
            NULL, 1, 0, 0, &Signature, NULL, &Dp, &Size) == EFI_INVALID_PARAMETER);
  assert (CephRbdBootManagerBuildLoaderDevicePath (
            &Backend, 1, 0, 0, NULL, NULL, &Dp, &Size) == EFI_INVALID_PARAMETER);
  assert (CephRbdBootManagerFreeDevicePath (&Backend, Dp) == EFI_SUCCESS);

  Dp = BuildTestDevicePath (&Backend, &Size);
  assert (CephRbdBootManagerFreeDevicePath (&Backend, Dp) == EFI_SUCCESS);
  assert (mEnv.OutstandingAllocs == 0);
  puts("boot manager: device path tests passed");
}

static void
TestLoadOption (
  void
  )
{
  CEPH_RBD_BOOT_MANAGER_BACKEND  Backend;
  EFI_DEVICE_PATH_PROTOCOL      *Dp;
  UINTN                          DpSize;
  VOID                          *Option;
  UINTN                          OptionSize;
  UINT8                         *Bytes;
  UINTN                          Index;
  UINT8                          OptData[2] = { 0xde, 0xad };

  ResetEnv ();
  InitBackend (&Backend);
  Dp = BuildTestDevicePath (&Backend, &DpSize);

  assert (CephRbdBootManagerBuildLoadOption (
            &Backend, NULL, Dp, DpSize, NULL, 0, &Option, &OptionSize) == EFI_SUCCESS);
  assert (OptionSize == (4 + 2 + (2 * 19) + DpSize));
  Bytes = (UINT8 *)Option;
  assert (ReadLe32 (Bytes) == 1);
  assert ((UINT16)(Bytes[4] | ((UINT16)Bytes[5] << 8)) == DpSize);
  for (Index = 0; Index < 19; Index++) {
    assert ((UINT16)(Bytes[6 + (Index * 2)] |
            ((UINT16)Bytes[7 + (Index * 2)] << 8)) == mExpectedDescription[Index]);
  }
  assert (memcmp (Bytes + 6 + 38, Dp, DpSize) == 0);
  Backend.PoolFree (Option);

  assert (CephRbdBootManagerBuildLoadOption (
            &Backend, NULL, Dp, DpSize, OptData, sizeof (OptData),
            &Option, &OptionSize) == EFI_SUCCESS);
  assert (OptionSize == (4 + 2 + 38 + DpSize + 2));
  Bytes = (UINT8 *)Option;
  assert (memcmp (Bytes + 6 + 38 + DpSize, OptData, 2) == 0);
  Backend.PoolFree (Option);

  assert (CephRbdBootManagerBuildLoadOption (
            &Backend, NULL, Dp, 0, NULL, 0, &Option, &OptionSize) ==
          EFI_INVALID_PARAMETER);
  assert (CephRbdBootManagerBuildLoadOption (
            &Backend, NULL, Dp, DpSize, NULL, 1, &Option, &OptionSize) ==
          EFI_INVALID_PARAMETER);

  assert (CephRbdBootManagerFreeDevicePath (&Backend, Dp) == EFI_SUCCESS);
  assert (mEnv.OutstandingAllocs == 0);
  puts("boot manager: load option tests passed");
}

static void
TestInstallAndRemove (
  void
  )
{
  CEPH_RBD_BOOT_MANAGER_BACKEND  Backend;
  EFI_DEVICE_PATH_PROTOCOL      *Dp;
  UINTN                          DpSize;
  UINT16                         Index;
  UINT16                         Expected[4];
  MOCK_VAR                      *Var;

  ResetEnv ();
  InitBackend (&Backend);
  Dp = BuildTestDevicePath (&Backend, &DpSize);

  assert (CephRbdBootManagerInstallLoadOption (
            &Backend, NULL, Dp, DpSize, CEPH_RBD_BM_AUTO_BOOT_INDEX, &Index) ==
          EFI_SUCCESS);
  assert (Index == 0);
  Var = FindBootVar (0);
  assert (Var != NULL);
  assert (Var->Attributes == 7);
  assert (Var->Size == (4 + 2 + 38 + DpSize));
  Expected[0] = 0;
  CheckBootOrder (Expected, 1);

  assert (CephRbdBootManagerInstallLoadOption (
            &Backend, NULL, Dp, DpSize, CEPH_RBD_BM_AUTO_BOOT_INDEX, &Index) ==
          EFI_SUCCESS);
  assert (Index == 1);
  Expected[1] = 1;
  CheckBootOrder (Expected, 2);

  // Caller-provided index is honored with uppercase hex digits.
  assert (CephRbdBootManagerInstallLoadOption (
            &Backend, NULL, Dp, DpSize, 0x000a, &Index) == EFI_SUCCESS);
  assert (Index == 0x000a);
  assert (FindBootVar (0x000a) != NULL);
  Expected[2] = 0x000a;
  CheckBootOrder (Expected, 3);

  // Reinstalling an existing index updates it without duplicating BootOrder.
  mEnv.SetVariableCalls = 0;
  assert (CephRbdBootManagerInstallLoadOption (
            &Backend, NULL, Dp, DpSize, 1, &Index) == EFI_SUCCESS);
  assert (FindBootVar (1) != NULL);
  CheckBootOrder (Expected, 3);

  // An occupied Boot0002 that is absent from BootOrder is skipped.
  {
    static CONST CHAR16  Name2[] = { 'B', 'o', 'o', 't', '0', '0', '0', '2', 0 };
    PutVarRaw (Name2, &mMockGlobalGuid, 7, Dp, DpSize);
  }
  assert (CephRbdBootManagerInstallLoadOption (
            &Backend, NULL, Dp, DpSize, CEPH_RBD_BM_AUTO_BOOT_INDEX, &Index) ==
          EFI_SUCCESS);
  assert (Index == 3);
  Expected[3] = 3;
  CheckBootOrder (Expected, 4);

  // Remove: delete the variable and every BootOrder occurrence.
  assert (CephRbdBootManagerRemoveLoadOption (&Backend, 1) == EFI_SUCCESS);
  assert (FindBootVar (1) == NULL);
  {
    UINT16  After[3] = { 0, 0x000a, 3 };
    CheckBootOrder (After, 3);
  }
  assert (CephRbdBootManagerRemoveLoadOption (&Backend, 0x1234) == EFI_SUCCESS);
  {
    UINT16  After[3] = { 0, 0x000a, 3 };
    CheckBootOrder (After, 3);
  }

  // SetVariable failure is propagated.
  mEnv.SetVariableFailStatus = EFI_ACCESS_DENIED;
  assert (CephRbdBootManagerInstallLoadOption (
            &Backend, NULL, Dp, DpSize, CEPH_RBD_BM_AUTO_BOOT_INDEX, &Index) ==
          EFI_ACCESS_DENIED);
  mEnv.SetVariableFailStatus = EFI_SUCCESS;

  assert (CephRbdBootManagerFreeDevicePath (&Backend, Dp) == EFI_SUCCESS);
  assert (mEnv.OutstandingAllocs == 0);
  puts("boot manager: install/remove tests passed");
}

static void
TestTryBoot (
  void
  )
{
  CEPH_RBD_BOOT_MANAGER_BACKEND  Backend;
  EFI_DEVICE_PATH_PROTOCOL      *Dp;
  UINTN                          DpSize;
  EFI_HANDLE                     Parent = (EFI_HANDLE)(uintptr_t)0x55;

  ResetEnv ();
  InitBackend (&Backend);
  Dp = BuildTestDevicePath (&Backend, &DpSize);

  assert (CephRbdBootManagerTryBoot (&Backend, Parent, Dp) == EFI_SUCCESS);
  assert (mEnv.LoadImageCalls == 1 && mEnv.StartImageCalls == 1);
  assert (mEnv.UnloadImageCalls == 1);
  assert (mEnv.CallLog[0] == 1 && mEnv.CallLog[1] == 2 && mEnv.CallLog[2] == 3);
  assert (mEnv.LastBootPolicy == FALSE);
  assert (mEnv.LastParentHandle == Parent);
  assert (mEnv.LastLoadImageDp == Dp);
  assert (mEnv.LastSourceBuffer == NULL && mEnv.LastSourceSize == 0);
  assert (mEnv.LastStartedHandle == mEnv.LoadedHandle);
  assert (mEnv.LastUnloadedHandle == mEnv.LoadedHandle);
  assert (CephRbdBootManagerFreeDevicePath (&Backend, Dp) == EFI_SUCCESS);

  // LoadImage failure: no StartImage / UnloadImage.
  ResetEnv ();
  InitBackend (&Backend);
  Dp = BuildTestDevicePath (&Backend, &DpSize);
  mEnv.LoadImageStatus = EFI_DEVICE_ERROR;
  assert (CephRbdBootManagerTryBoot (&Backend, Parent, Dp) == EFI_DEVICE_ERROR);
  assert (mEnv.StartImageCalls == 0 && mEnv.UnloadImageCalls == 0);
  assert (CephRbdBootManagerFreeDevicePath (&Backend, Dp) == EFI_SUCCESS);

  // StartImage failure: image is still unloaded.
  ResetEnv ();
  InitBackend (&Backend);
  Dp = BuildTestDevicePath (&Backend, &DpSize);
  mEnv.StartImageStatus = EFI_ABORTED;
  assert (CephRbdBootManagerTryBoot (&Backend, Parent, Dp) == EFI_ABORTED);
  assert (mEnv.UnloadImageCalls == 1);
  assert (CephRbdBootManagerFreeDevicePath (&Backend, Dp) == EFI_SUCCESS);
  assert (mEnv.OutstandingAllocs == 0);
  puts("boot manager: try-boot tests passed");
}

static void
TestHandoff (
  void
  )
{
  CEPH_RBD_BOOT_MANAGER_BACKEND  Backend;
  CEPH_RBD_GPT_PARTITION         Esp;
  UINT16                         Index;
  MOCK_VAR                      *Var;
  UINT8                         *Option;
  UINT8                         *DpBytes;

  ResetEnv ();
  InitBackend (&Backend);
  memset (&Esp, 0, sizeof (Esp));
  memcpy (Esp.UniqueGuid, mSignatureBytes, 16);
  Esp.FirstLba = UINT64_C(0x1000);
  Esp.LastLba = UINT64_C(0x11ff);

  // Variable-only handoff (TryBoot == FALSE).
  assert (CephRbdBootManagerHandoff (
            &Backend, &Esp, 2, CEPH_RBD_BM_AUTO_BOOT_INDEX, FALSE,
            (EFI_HANDLE)(uintptr_t)0x55, &Index) == EFI_SUCCESS);
  assert (Index == 0 && mEnv.LoadImageCalls == 0);
  Var = FindBootVar (0);
  assert (Var != NULL);
  Option = Var->Data;
  DpBytes = Option + 6 + 38;
  assert (ReadLe32 (DpBytes + 4) == 2);
  assert (ReadLe64 (DpBytes + 8) == UINT64_C(0x1000));
  assert (ReadLe64 (DpBytes + 16) == UINT64_C(0x200));
  assert (memcmp (DpBytes + 24, mSignatureBytes, 16) == 0);
  assert (mEnv.OutstandingAllocs == 0);

  // Immediate handoff is opt-in.
  ResetEnv ();
  InitBackend (&Backend);
  assert (CephRbdBootManagerHandoff (
            &Backend, &Esp, 2, CEPH_RBD_BM_AUTO_BOOT_INDEX, TRUE,
            (EFI_HANDLE)(uintptr_t)0x55, &Index) == EFI_SUCCESS);
  assert (mEnv.LoadImageCalls == 1 && mEnv.StartImageCalls == 1);
  assert (mEnv.UnloadImageCalls == 1);
  assert (mEnv.OutstandingAllocs == 0);

  assert (CephRbdBootManagerHandoff (
            &Backend, NULL, 2, CEPH_RBD_BM_AUTO_BOOT_INDEX, FALSE,
            NULL, &Index) == EFI_INVALID_PARAMETER);
  puts("boot manager: handoff tests passed");
}

int
main (
  void
  )
{
  TestDevicePath ();
  TestLoadOption ();
  TestInstallAndRemove ();
  TestTryBoot ();
  TestHandoff ();
  puts("boot manager handoff tests: passed");
  return 0;
}
