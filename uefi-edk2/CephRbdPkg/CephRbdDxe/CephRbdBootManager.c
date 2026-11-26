#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "CephRbdBootManager.h"

// Canonical EFI_GLOBAL_VARIABLE_GUID (gEfiGlobalVariableGuid in MdePkg).
// Kept local so this module needs no [Guids] entry and compiles unchanged in
// host tests.
STATIC CONST EFI_GUID  mCephRbdGlobalVariableGuid = {
  0x8be4df61, 0x93ca, 0x11d2,
  { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c }
};

// All names/descriptions are explicit UTF-16 code points: the host stub maps
// CHAR16 to uint16_t, so L"..." would produce 4-byte wchar_t literals.
STATIC CONST CHAR16  mBootOrderName[] = {
  'B', 'o', 'o', 't', 'O', 'r', 'd', 'e', 'r', 0
};

STATIC CONST CHAR16  mDefaultDescription[] = {
  'C', 'e', 'p', 'h', ' ', 'R', 'B', 'D', ' ',
  '(', 'G', 'P', 'T', '/', 'E', 'S', 'P', ')', 0
};

STATIC CONST CHAR16  mHexDigits[] = {
  '0', '1', '2', '3', '4', '5', '6', '7',
  '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};

// ---------------------------------------------------------------------------
// Small encoding helpers. Device paths and load options are written through
// explicit byte offsets rather than sizeof() so padding never leaks in.
// ---------------------------------------------------------------------------
STATIC
VOID
WriteLe16 (
  OUT UINT8   *Buffer,
  IN  UINT16  Value
  )
{
  Buffer[0] = (UINT8)Value;
  Buffer[1] = (UINT8)(Value >> 8);
}

STATIC
VOID
WriteLe32 (
  OUT UINT8   *Buffer,
  IN  UINT32  Value
  )
{
  WriteLe16 (Buffer, (UINT16)Value);
  WriteLe16 (Buffer + 2, (UINT16)(Value >> 16));
}

STATIC
VOID
WriteLe64 (
  OUT UINT8   *Buffer,
  IN  UINT64  Value
  )
{
  WriteLe32 (Buffer, (UINT32)Value);
  WriteLe32 (Buffer + 4, (UINT32)(Value >> 32));
}

STATIC
EFI_STATUS
StrLenCapped (
  IN  CONST CHAR16  *String,
  IN  UINTN          MaxChars,
  OUT UINTN          *Length
  )
{
  UINTN  Index;

  if ((String == NULL) || (Length == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  for (Index = 0; Index <= MaxChars; Index++) {
    if (String[Index] == 0) {
      *Length = Index;
      return EFI_SUCCESS;
    }
  }
  return EFI_BAD_BUFFER_SIZE;
}

STATIC
VOID
FormatBootVariableName (
  IN  UINT16  Index,
  OUT CHAR16  Name[9]
  )
{
  Name[0] = 'B';
  Name[1] = 'o';
  Name[2] = 'o';
  Name[3] = 't';
  Name[4] = mHexDigits[(Index >> 12) & 0xf];
  Name[5] = mHexDigits[(Index >> 8) & 0xf];
  Name[6] = mHexDigits[(Index >> 4) & 0xf];
  Name[7] = mHexDigits[Index & 0xf];
  Name[8] = 0;
}

STATIC
VOID
SetDevicePathHeader (
  OUT UINT8   *Node,
  IN  UINT8   Type,
  IN  UINT8   SubType,
  IN  UINT16  Length
  )
{
  Node[0] = Type;
  Node[1] = SubType;
  Node[2] = (UINT8)Length;
  Node[3] = (UINT8)(Length >> 8);
}

STATIC
BOOLEAN
ContainsIndex (
  IN CONST UINT16  *Entries,
  IN UINTN          Count,
  IN UINT16         Index
  )
{
  UINTN  Pos;

  if (Entries == NULL) {
    return FALSE;
  }
  for (Pos = 0; Pos < Count; Pos++) {
    if (Entries[Pos] == Index) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC
EFI_STATUS
ReadBootOrder (
  IN  CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  OUT UINT16                               **Entries,
  OUT UINTN                                *Count
  )
{
  EFI_STATUS  Status;
  UINTN       Size;
  UINTN       BufferSize;

  *Entries = NULL;
  *Count = 0;
  if (Backend->GetVariable == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Size = 0;
  Status = Backend->GetVariable (
                      mBootOrderName, &mCephRbdGlobalVariableGuid,
                      NULL, &Size, NULL
                      );
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status) && (Status != EFI_BUFFER_TOO_SMALL)) {
    return Status;
  }
  if ((Size == 0) || ((Size % sizeof (UINT16)) != 0)) {
    // Missing or malformed BootOrder is treated as empty.
    return EFI_SUCCESS;
  }

  BufferSize = Size;
  *Entries = Backend->PoolAllocate (BufferSize);
  if (*Entries == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = Backend->GetVariable (
                      mBootOrderName, &mCephRbdGlobalVariableGuid,
                      NULL, &BufferSize, *Entries
                      );
  if (EFI_ERROR (Status)) {
    Backend->PoolFree (*Entries);
    *Entries = NULL;
    return Status;
  }
  *Count = BufferSize / sizeof (UINT16);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
WriteBootOrder (
  IN CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN CONST UINT16                         *Entries,
  IN UINTN                                Count
  )
{
  EFI_STATUS  Status;
  UINT8       *Bytes;
  UINTN       Index;

  if (Backend->SetVariable == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Count == 0) {
    // SetVariable with zero size is the UEFI delete operation.
    return Backend->SetVariable (
                      mBootOrderName, &mCephRbdGlobalVariableGuid,
                      0, 0, NULL
                      );
  }

  Bytes = Backend->PoolAllocate (Count * sizeof (UINT16));
  if (Bytes == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  for (Index = 0; Index < Count; Index++) {
    WriteLe16 (Bytes + (Index * sizeof (UINT16)), Entries[Index]);
  }
  Status = Backend->SetVariable (
                      mBootOrderName, &mCephRbdGlobalVariableGuid,
                      CEPH_RBD_BM_BOOT_VARIABLE_ATTRS,
                      Count * sizeof (UINT16), Bytes
                      );
  Backend->PoolFree (Bytes);
  return Status;
}

STATIC
EFI_STATUS
FindFreeBootIndex (
  IN  CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN  CONST UINT16                         *BootOrder,
  IN  UINTN                                BootOrderCount,
  OUT UINT16                               *Index
  )
{
  EFI_STATUS  Status;
  UINT16      Candidate;
  CHAR16      Name[9];
  UINTN       Size;

  for (Candidate = 0; Candidate < CEPH_RBD_BM_AUTO_BOOT_INDEX; Candidate++) {
    if (ContainsIndex (BootOrder, BootOrderCount, Candidate)) {
      continue;
    }
    FormatBootVariableName (Candidate, Name);
    Size = 0;
    Status = Backend->GetVariable (
                        Name, &mCephRbdGlobalVariableGuid, NULL, &Size, NULL
                        );
    if (Status == EFI_NOT_FOUND) {
      *Index = Candidate;
      return EFI_SUCCESS;
    }
    if (EFI_ERROR (Status) && (Status != EFI_BUFFER_TOO_SMALL)) {
      return Status;
    }
  }
  return EFI_OUT_OF_RESOURCES;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
VOID
CephRbdBootManagerGuidFromGptBytes (
  IN     CONST UINT8  *GptGuidBytes,
  OUT    EFI_GUID     *Guid
  )
{
  if ((GptGuidBytes == NULL) || (Guid == NULL)) {
    return;
  }
  // GPT stores Data1/Data2/Data3 little-endian and Data4 verbatim, which is
  // exactly the EFI_GUID in-memory layout on the target; a raw 16-byte copy
  // yields the canonical GUID used by SIGNATURE_TYPE_GUID device paths.
  CopyMem (Guid, GptGuidBytes, sizeof (EFI_GUID));
}

EFI_STATUS
CephRbdBootManagerBuildLoaderDevicePath (
  IN     CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN     UINT32                               PartitionNumber,
  IN     UINT64                               PartitionStartLba,
  IN     UINT64                               PartitionSizeLba,
  IN     CONST EFI_GUID                       *PartitionSignature,
  IN     CONST CHAR16                         *LoaderPath OPTIONAL,
  OUT    EFI_DEVICE_PATH_PROTOCOL             **DevicePath,
  OUT    UINTN                                *DevicePathSize
  )
{
  EFI_STATUS    Status;
  CHAR16        LocalPath[CEPH_RBD_BM_MAX_LOADER_PATH_CHARS + 1];
  CONST CHAR16  *Path;
  UINTN         PathLen;
  UINTN         FilePathLen;
  UINTN         TotalLen;
  UINTN         Pos;
  UINT8         *Bytes;
  UINT8         *FilePathNode;
  UINT8         *EndNode;

  if ((Backend == NULL) || (Backend->PoolAllocateZero == NULL) ||
      (PartitionSignature == NULL) || (DevicePath == NULL) ||
      (DevicePathSize == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (LoaderPath == NULL) {
    Status = CephRbdGptBuildEspBootPath (
               LocalPath, CEPH_RBD_BM_MAX_LOADER_PATH_CHARS + 1
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Path = LocalPath;
  } else {
    Path = LoaderPath;
  }
  Status = StrLenCapped (Path, CEPH_RBD_BM_MAX_LOADER_PATH_CHARS, &PathLen);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // 42-byte HD node + FilePath node + 4-byte end node.
  FilePathLen = 4 + (2 * (PathLen + 1));
  TotalLen = 42 + FilePathLen + 4;
  Bytes = Backend->PoolAllocateZero (TotalLen);
  if (Bytes == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  // HD (GPT) node: PartitionNumber, start/size in BlockIo LBA units, the
  // partition GUID signature, MBR type GPT and SIGNATURE_TYPE_GUID.
  SetDevicePathHeader (
    Bytes, CEPH_RBD_BM_MEDIA_DEVICE_PATH, CEPH_RBD_BM_MEDIA_HARDDRIVE_DP, 42
    );
  WriteLe32 (Bytes + 4, PartitionNumber);
  WriteLe64 (Bytes + 8, PartitionStartLba);
  WriteLe64 (Bytes + 16, PartitionSizeLba);
  CopyMem (Bytes + 24, PartitionSignature, sizeof (EFI_GUID));
  Bytes[40] = CEPH_RBD_BM_MBR_TYPE_GPT;
  Bytes[41] = CEPH_RBD_BM_SIGNATURE_TYPE_GUID;

  // FilePath node: NUL-terminated UCS-2 loader path, one code point per 2
  // little-endian bytes.
  FilePathNode = Bytes + 42;
  SetDevicePathHeader (
    FilePathNode,
    CEPH_RBD_BM_MEDIA_DEVICE_PATH,
    CEPH_RBD_BM_MEDIA_FILEPATH_DP,
    (UINT16)FilePathLen
    );
  for (Pos = 0; Pos <= PathLen; Pos++) {
    WriteLe16 (FilePathNode + 4 + (Pos * 2), (UINT16)Path[Pos]);
  }

  // End entire device path.
  EndNode = Bytes + 42 + FilePathLen;
  SetDevicePathHeader (
    EndNode,
    CEPH_RBD_BM_END_DEVICE_PATH_TYPE,
    CEPH_RBD_BM_END_DP_SUBTYPE,
    4
    );

  *DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)Bytes;
  *DevicePathSize = TotalLen;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdBootManagerFreeDevicePath (
  IN     CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN     EFI_DEVICE_PATH_PROTOCOL             *DevicePath
  )
{
  if ((Backend == NULL) || (Backend->PoolFree == NULL) ||
      (DevicePath == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }
  Backend->PoolFree (DevicePath);
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdBootManagerBuildLoadOption (
  IN     CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN     CONST CHAR16                         *Description OPTIONAL,
  IN     CONST EFI_DEVICE_PATH_PROTOCOL       *DevicePath,
  IN     UINTN                                DevicePathSize,
  IN     CONST VOID                           *OptionalData OPTIONAL,
  IN     UINTN                                OptionalDataSize,
  OUT    VOID                                 **LoadOption,
  OUT    UINTN                                *LoadOptionSize
  )
{
  EFI_STATUS    Status;
  CONST CHAR16  *Desc;
  UINTN         DescLen;
  UINTN         DescBytes;
  UINTN         TotalLen;
  UINTN         Pos;
  UINT8         *Bytes;

  if ((Backend == NULL) || (Backend->PoolAllocateZero == NULL) ||
      (DevicePath == NULL) || (DevicePathSize == 0) ||
      (LoadOption == NULL) || (LoadOptionSize == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }
  if (DevicePathSize > 0xffff) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if ((OptionalData == NULL) && (OptionalDataSize != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Desc = (Description == NULL) ? mDefaultDescription : Description;
  Status = StrLenCapped (Desc, CEPH_RBD_BM_MAX_LOADER_PATH_CHARS, &DescLen);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  DescBytes = 2 * (DescLen + 1);
  TotalLen = sizeof (UINT32) + sizeof (UINT16) + DescBytes +
             DevicePathSize + OptionalDataSize;

  Bytes = Backend->PoolAllocateZero (TotalLen);
  if (Bytes == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  WriteLe32 (Bytes, LOAD_OPTION_ACTIVE);
  WriteLe16 (Bytes + 4, (UINT16)DevicePathSize);   // FilePathListLength
  for (Pos = 0; Pos <= DescLen; Pos++) {
    WriteLe16 (Bytes + 6 + (Pos * 2), (UINT16)Desc[Pos]);
  }
  CopyMem (Bytes + 6 + DescBytes, DevicePath, DevicePathSize);
  if (OptionalDataSize != 0) {
    CopyMem (Bytes + 6 + DescBytes + DevicePathSize, OptionalData, OptionalDataSize);
  }

  *LoadOption = Bytes;
  *LoadOptionSize = TotalLen;
  return EFI_SUCCESS;
}

EFI_STATUS
CephRbdBootManagerInstallLoadOption (
  IN     CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN     CONST CHAR16                         *Description OPTIONAL,
  IN     CONST EFI_DEVICE_PATH_PROTOCOL       *DevicePath,
  IN     UINTN                                DevicePathSize,
  IN     UINT16                               RequestedBootIndex,
  OUT OPTIONAL UINT16                         *BootIndex
  )
{
  EFI_STATUS  Status;
  UINT16      *BootOrder;
  UINTN       BootOrderCount;
  UINT16      Index;
  UINT16      *NewOrder;
  VOID        *LoadOption;
  UINTN       LoadOptionSize;
  CHAR16      Name[9];

  if ((Backend == NULL) || (Backend->GetVariable == NULL) ||
      (Backend->SetVariable == NULL) || (Backend->PoolAllocate == NULL) ||
      (Backend->PoolFree == NULL) || (DevicePath == NULL) ||
      (DevicePathSize == 0))
  {
    return EFI_INVALID_PARAMETER;
  }
  BootOrder = NULL;
  BootOrderCount = 0;
  LoadOption = NULL;

  Status = ReadBootOrder (Backend, &BootOrder, &BootOrderCount);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (RequestedBootIndex == CEPH_RBD_BM_AUTO_BOOT_INDEX) {
    Status = FindFreeBootIndex (Backend, BootOrder, BootOrderCount, &Index);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  } else {
    Index = RequestedBootIndex;
  }

  Status = CephRbdBootManagerBuildLoadOption (
             Backend, Description, DevicePath, DevicePathSize,
             NULL, 0, &LoadOption, &LoadOptionSize
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  FormatBootVariableName (Index, Name);
  Status = Backend->SetVariable (
                      Name, &mCephRbdGlobalVariableGuid,
                      CEPH_RBD_BM_BOOT_VARIABLE_ATTRS,
                      LoadOptionSize, LoadOption
                      );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (!ContainsIndex (BootOrder, BootOrderCount, Index)) {
    NewOrder = Backend->PoolAllocate ((BootOrderCount + 1) * sizeof (UINT16));
    if (NewOrder == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Done;
    }
    if (BootOrderCount != 0) {
      CopyMem (NewOrder, BootOrder, BootOrderCount * sizeof (UINT16));
    }
    NewOrder[BootOrderCount] = Index;
    Backend->PoolFree (BootOrder);
    BootOrder = NewOrder;
    BootOrderCount++;
  }
  Status = WriteBootOrder (Backend, BootOrder, BootOrderCount);
  if (!EFI_ERROR (Status) && (BootIndex != NULL)) {
    *BootIndex = Index;
  }

Done:
  if (LoadOption != NULL) {
    Backend->PoolFree (LoadOption);
  }
  if (BootOrder != NULL) {
    Backend->PoolFree (BootOrder);
  }
  return Status;
}

EFI_STATUS
CephRbdBootManagerRemoveLoadOption (
  IN     CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN     UINT16                               BootIndex
  )
{
  EFI_STATUS  Status;
  UINT16      *BootOrder;
  UINTN       BootOrderCount;
  UINT16      *NewOrder;
  UINTN       KeepCount;
  UINTN       Pos;
  CHAR16      Name[9];

  if ((Backend == NULL) || (Backend->GetVariable == NULL) ||
      (Backend->SetVariable == NULL) || (Backend->PoolAllocate == NULL) ||
      (Backend->PoolFree == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  FormatBootVariableName (BootIndex, Name);
  Status = Backend->SetVariable (
                      Name, &mCephRbdGlobalVariableGuid, 0, 0, NULL
                      );
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    return Status;
  }

  Status = ReadBootOrder (Backend, &BootOrder, &BootOrderCount);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  NewOrder = NULL;
  KeepCount = 0;
  if (BootOrderCount != 0) {
    NewOrder = Backend->PoolAllocate (BootOrderCount * sizeof (UINT16));
    if (NewOrder == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Done;
    }
    for (Pos = 0; Pos < BootOrderCount; Pos++) {
      if (BootOrder[Pos] != BootIndex) {
        NewOrder[KeepCount++] = BootOrder[Pos];
      }
    }
  }
  Status = WriteBootOrder (Backend, NewOrder, KeepCount);

Done:
  if (NewOrder != NULL) {
    Backend->PoolFree (NewOrder);
  }
  if (BootOrder != NULL) {
    Backend->PoolFree (BootOrder);
  }
  return Status;
}

EFI_STATUS
CephRbdBootManagerTryBoot (
  IN CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN EFI_HANDLE                           ParentImageHandle,
  IN CONST EFI_DEVICE_PATH_PROTOCOL       *DevicePath
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  ImageHandle;

  if ((Backend == NULL) || (DevicePath == NULL) ||
      (Backend->LoadImage == NULL) || (Backend->StartImage == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  ImageHandle = NULL;
  Status = Backend->LoadImage (
                      FALSE, ParentImageHandle, DevicePath, NULL, 0, &ImageHandle
                      );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // On a real firmware StartImage normally transfers control and never
  // returns. If it does return (image exited or failed), unload the image so
  // no handle is leaked.
  Status = Backend->StartImage (ImageHandle, NULL, NULL);
  if (Backend->UnloadImage != NULL) {
    Backend->UnloadImage (ImageHandle);
  }
  return Status;
}

EFI_STATUS
CephRbdBootManagerHandoff (
  IN     CONST CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend,
  IN     CONST CEPH_RBD_GPT_PARTITION         *Esp,
  IN     UINT32                               PartitionNumber,
  IN     UINT16                               RequestedBootIndex,
  IN     BOOLEAN                              TryBoot,
  IN     EFI_HANDLE                           ParentImageHandle,
  OUT OPTIONAL UINT16                         *BootIndex
  )
{
  EFI_STATUS                Status;
  EFI_GUID                  Signature;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  UINTN                     DevicePathSize;
  UINT16                    Index;

  if ((Backend == NULL) || (Esp == NULL) || (PartitionNumber == 0) ||
      (Esp->LastLba < Esp->FirstLba))
  {
    return EFI_INVALID_PARAMETER;
  }

  DevicePath = NULL;
  CephRbdBootManagerGuidFromGptBytes (Esp->UniqueGuid, &Signature);
  Status = CephRbdBootManagerBuildLoaderDevicePath (
             Backend, PartitionNumber, Esp->FirstLba,
             (Esp->LastLba - Esp->FirstLba + 1),
             &Signature, NULL, &DevicePath, &DevicePathSize
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = CephRbdBootManagerInstallLoadOption (
             Backend, NULL, DevicePath, DevicePathSize,
             RequestedBootIndex, &Index
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (TryBoot) {
    Status = CephRbdBootManagerTryBoot (
               Backend, ParentImageHandle, DevicePath
               );
  }
  if (!EFI_ERROR (Status) && (BootIndex != NULL)) {
    *BootIndex = Index;
  }

Done:
  CephRbdBootManagerFreeDevicePath (Backend, DevicePath);
  return Status;
}

// ---------------------------------------------------------------------------
// EDK II backend: thin wrappers around the real gBS/gRT. Excluded from host
// tests, whose stubs intentionally do not declare LoadImage/StartImage/etc.
// ---------------------------------------------------------------------------
#ifndef CEPH_RBD_BOOT_MANAGER_HOST_TEST

STATIC
EFI_STATUS
EFIAPI
EfiGetVariableWrapper (
  IN     CONST CHAR16    *VariableName,
  IN     CONST EFI_GUID  *VendorGuid,
  OUT OPTIONAL UINT32    *Attributes,
  IN OUT UINTN           *DataSize,
  OUT OPTIONAL VOID      *Data
  )
{
  // Cast away CONST: some EDK II revisions declare these parameters without
  // CONST; firmware never modifies the name or GUID.
  return gRT->GetVariable (
                (CHAR16 *)VariableName, (EFI_GUID *)VendorGuid,
                Attributes, DataSize, Data
                );
}

STATIC
EFI_STATUS
EFIAPI
EfiSetVariableWrapper (
  IN CONST CHAR16    *VariableName,
  IN CONST EFI_GUID  *VendorGuid,
  IN UINT32           Attributes,
  IN UINTN            DataSize,
  IN CONST VOID      *Data
  )
{
  return gRT->SetVariable (
                (CHAR16 *)VariableName, (EFI_GUID *)VendorGuid,
                Attributes, DataSize, (VOID *)Data
                );
}

STATIC
VOID *
EFIAPI
EfiAllocatePoolWrapper (
  IN UINTN  Size
  )
{
  return AllocatePool (Size);
}

STATIC
VOID *
EFIAPI
EfiAllocateZeroPoolWrapper (
  IN UINTN  Size
  )
{
  return AllocateZeroPool (Size);
}

STATIC
VOID
EFIAPI
EfiFreePoolWrapper (
  IN VOID  *Buffer
  )
{
  FreePool (Buffer);
}

STATIC
EFI_STATUS
EFIAPI
EfiLoadImageWrapper (
  IN     BOOLEAN                        BootPolicy,
  IN     EFI_HANDLE                     ParentImageHandle,
  IN     CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN     VOID                           *SourceBuffer OPTIONAL,
  IN     UINTN                          SourceSize,
  OUT    EFI_HANDLE                     *ImageHandle
  )
{
  return gBS->LoadImage (
                BootPolicy, ParentImageHandle,
                (EFI_DEVICE_PATH_PROTOCOL *)DevicePath,
                SourceBuffer, SourceSize, ImageHandle
                );
}

STATIC
EFI_STATUS
EFIAPI
EfiStartImageWrapper (
  IN     EFI_HANDLE    ImageHandle,
  OUT OPTIONAL UINTN   *ExitDataSize,
  OUT OPTIONAL CHAR16  **ExitData
  )
{
  return gBS->StartImage (ImageHandle, ExitDataSize, ExitData);
}

STATIC
EFI_STATUS
EFIAPI
EfiUnloadImageWrapper (
  IN EFI_HANDLE  ImageHandle
  )
{
  return gBS->UnloadImage (ImageHandle);
}

EFI_STATUS
CephRbdBootManagerGetEfiBackend (
  OUT CEPH_RBD_BOOT_MANAGER_BACKEND  *Backend
  )
{
  if (Backend == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Backend->GetVariable = EfiGetVariableWrapper;
  Backend->SetVariable = EfiSetVariableWrapper;
  Backend->PoolAllocate = EfiAllocatePoolWrapper;
  Backend->PoolAllocateZero = EfiAllocateZeroPoolWrapper;
  Backend->PoolFree = EfiFreePoolWrapper;
  Backend->LoadImage = EfiLoadImageWrapper;
  Backend->StartImage = EfiStartImageWrapper;
  Backend->UnloadImage = EfiUnloadImageWrapper;
  return EFI_SUCCESS;
}

#endif
