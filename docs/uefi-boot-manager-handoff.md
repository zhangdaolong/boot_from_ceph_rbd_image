# GPT/ESP → UEFI Boot Manager 启动链交接层

本目录记录 `CephRbdBootManager.*` —— 从已验证的 GPT/ESP 发现结果到 UEFI Boot Manager
的交接层。它**只**负责构造设备路径、创建/删除 `Boot####` 启动项、更新 `BootOrder`，以及
可选的 `LoadImage`/`StartImage` 立即启动。

边界说明：

- **不实现 FAT**。真实固件要求 ESP 分区内是合法的 FAT 文件系统且包含有效的
  `BOOTX64.EFI`（或其它指定的 loader）。本层只生成一个指向该文件的设备路径并把它交给
  Boot Manager / `LoadImage`；文件系统的解析由固件自身的 FAT 驱动在完成。
- **不安装任何文件系统协议**（不安装 `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`）。RBD 磁盘仍以
  只读 `EFI_BLOCK_IO_PROTOCOL` 暴露；ESP 是该磁盘上的一个 GPT 分区。
- 不触碰 CephX 加密、配置、DHCP/网络、EDK II 构建文件。

## 新增文件

- `uefi-edk2/CephRbdPkg/CephRbdDxe/CephRbdBootManager.h`
- `uefi-edk2/CephRbdPkg/CephRbdDxe/CephRbdBootManager.c`
- `tests/host/test_boot_manager.c`
- `tests/run-core-tests.sh` 末尾追加一个独立的编译/运行块（未改动原有内容）

## API

所有固件服务调用都通过一个函数指针表 `CEPH_RBD_BOOT_MANAGER_BACKEND` 完成。字段：
`GetVariable`、`SetVariable`、`PoolAllocate`、`PoolAllocateZero`、`PoolFree`、
`LoadImage`、`StartImage`、`UnloadImage`。字段以 `Pool*` 命名是因为 host stub 把
`AllocatePool`/`FreePool` 定义成函数式宏。字段名为 `NULL` 时相应的公开函数返回
`EFI_INVALID_PARAMETER`。

```c
#ifndef CEPH_RBD_BOOT_MANAGER_HOST_TEST
EFI_STATUS CephRbdBootManagerGetEfiBackend (
  OUT CEPH_RBD_BOOT_MANAGER_BACKEND *Backend);
#endif

VOID CephRbdBootManagerGuidFromGptBytes (
  IN CONST UINT8 *GptGuidBytes, OUT EFI_GUID *Guid);

EFI_STATUS CephRbdBootManagerBuildLoaderDevicePath (
  IN CONST CEPH_RBD_BOOT_MANAGER_BACKEND *Backend,
  IN UINT32 PartitionNumber,
  IN UINT64 PartitionStartLba,
  IN UINT64 PartitionSizeLba,
  IN CONST EFI_GUID *PartitionSignature,
  IN CONST CHAR16 *LoaderPath OPTIONAL,
  OUT EFI_DEVICE_PATH_PROTOCOL **DevicePath,
  OUT UINTN *DevicePathSize);

EFI_STATUS CephRbdBootManagerFreeDevicePath (
  IN CONST CEPH_RBD_BOOT_MANAGER_BACKEND *Backend,
  IN EFI_DEVICE_PATH_PROTOCOL *DevicePath);

EFI_STATUS CephRbdBootManagerBuildLoadOption (
  IN CONST CEPH_RBD_BOOT_MANAGER_BACKEND *Backend,
  IN CONST CHAR16 *Description OPTIONAL,
  IN CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath,
  IN UINTN DevicePathSize,
  IN CONST VOID *OptionalData OPTIONAL,
  IN UINTN OptionalDataSize,
  OUT VOID **LoadOption,
  OUT UINTN *LoadOptionSize);

EFI_STATUS CephRbdBootManagerInstallLoadOption (
  IN CONST CEPH_RBD_BOOT_MANAGER_BACKEND *Backend,
  IN CONST CHAR16 *Description OPTIONAL,
  IN CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath,
  IN UINTN DevicePathSize,
  IN UINT16 RequestedBootIndex,          /* 0xffff = auto */
  OUT OPTIONAL UINT16 *BootIndex);

EFI_STATUS CephRbdBootManagerRemoveLoadOption (
  IN CONST CEPH_RBD_BOOT_MANAGER_BACKEND *Backend,
  IN UINT16 BootIndex);

EFI_STATUS CephRbdBootManagerTryBoot (
  IN CONST CEPH_RBD_BOOT_MANAGER_BACKEND *Backend,
  IN EFI_HANDLE ParentImageHandle,
  IN CONST EFI_DEVICE_PATH_PROTOCOL *DevicePath);

EFI_STATUS CephRbdBootManagerHandoff (
  IN CONST CEPH_RBD_BOOT_MANAGER_BACKEND *Backend,
  IN CONST CEPH_RBD_GPT_PARTITION *Esp,
  IN UINT32 PartitionNumber,
  IN UINT16 RequestedBootIndex,
  IN BOOLEAN TryBoot,
  IN EFI_HANDLE ParentImageHandle,
  OUT OPTIONAL UINT16 *BootIndex);
```

`CephRbdBootManagerHandoff()` 是给 DXE 驱动用的一站式入口：它从 `Esp->UniqueGuid`
派生分区签名，使用 `Esp->FirstLba`/`Esp->LastLba` 计算分区大小，调用
`CephRbdGptBuildEspBootPath()` 得到 `\EFI\BOOT\BOOTX64.EFI`，创建启动项，然后仅在
`TryBoot == TRUE` 时执行 `LoadImage`+`StartImage`。`BuildLoaderDevicePath()` 传入非
`NULL` 的 `LoaderPath` 可覆盖默认 loader（例如其它架构的回退路径）。所有中间分配在
返回前释放。

## UEFI 语义

- **设备路径**：`HD(1,GPT)` 节点 42 字节 — `Type=0x01`、`SubType=0x01`、
  `PartitionNumber`（GPT 中 1 基序号）、`PartitionStart`/`PartitionSize`（BlockIo LBA
  单位，512 字节）、16 字节签名、`MBRType=0x02`（GPT）、`SignatureType=0x02`（GUID）。
  后接 `FilePath(0x01,0x04)` 节点，内含 NUL 结尾的 UCS-2 loader 路径，最后以
  `EndEntire(0x7f,0xff,4)` 结束。默认路径 `\EFI\BOOT\BOOTX64.EFI` 得到 94 字节。
  GPT GUID 的字节序与 `EFI_GUID` 内存布局一致，故 16 字节直接拷贝。
- **启动项**：`Attributes = LOAD_OPTION_ACTIVE (1)`，`FilePathListLength = 设备路径字节数`，
  `Description = L"Ceph RBD (GPT/ESP)"`（源码中用显式 UTF-16 码点数组，避免 host stub 的
  `CHAR16=uint16_t` 与 `L"..."` 冲突），可选数据默认为空。
- **变量**：名称 `Boot<4 位大写十六进制>`（UCS-2，9 个 `CHAR16`），厂商 GUID 为
  `EFI_GLOBAL_VARIABLE_GUID`，属性为
  `NV | BOOTSERVICE | RUNTIME`（`0x00000007`）。本层内建该 GUID 的本地常量，因此 INF
  不需要新增 `[Guids]` 项。
- **索引选择**：`RequestedBootIndex != 0xffff` 时直接使用该索引（已存在则更新）；自动模式
  扫描 `BootOrder` 和 `Boot0000..BootFFFF`，取最低的空闲索引。
- **BootOrder**：保留原有条目并追加新索引（已存在则不重复添加）；删除时移除该索引的所有
  出现，空数组通过 `SetVariable(..., 0, NULL)` 删除变量。
- **TryBoot**：`LoadImage(FALSE, Parent, Dp, NULL, 0, &Image)` → `StartImage(Image, NULL, NULL)`
  → `UnloadImage(Image)`。真实固件上 `StartImage` 成功时不返回；一旦返回（退出/失败），
  image 会被卸载以保证清理。该调用完全可选，变量方式交接不需要它。

## Host 测试

```sh
sh tests/run-core-tests.sh
```

新增的独立测试块使用 `-DCEPH_RBD_BOOT_MANAGER_HOST_TEST` 编译
`test_boot_manager.c` + `CephRbdBootManager.c` + `CephRbdGpt.c`，与原有 core 测试、PXE
测试一起运行。当前结果：全部通过（新增 5 组：device path、load option、install/remove、
try-boot、handoff）。

测试中 mock 的内容：一个 16 项的 `Boot####`/`BootOrder` 变量存储（含
`GetVariable` 的 `Data==NULL` 尺寸查询与 `SetVariable` 零尺寸删除），`Pool*` 分配/释放
计数，`LoadImage`/`StartImage`/`UnloadImage` 的调用参数与顺序以及可注入的返回状态。
验证点包括设备路径的逐字节布局、load option 编码、索引分配/去重、删除、调用顺序和分配
计数归零。

未被 host 测试覆盖的部分：真实 EFI 变量服务、真实 `LoadImage`/`StartImage` 行为、FAT
文件系统、真实 BlockIo 读取、以及真正的 EDK II 构建与固件启动（禁止真机测试）。EDK2
专用分支（`#ifndef CEPH_RBD_BOOT_MANAGER_HOST_TEST`）仅用 `/tmp` 中增强的仿 EDK II 桩
做过语法检查，未在 EDK II 构建系统中编译。

## 接线到 CephRbdDxe.c / INF

1. `CephRbdDxe.c` 增加 `#include "CephRbdBootManager.h"`（该头文件已包含
   `CephRbdGpt.h`）。
2. 在 `CephRbdGptDiscoverEsp()` 成功之后调用：

   ```c
   CEPH_RBD_BOOT_MANAGER_BACKEND  Bm;
   UINT16                         BootIndex;

   Status = CephRbdBootManagerGetEfiBackend (&Bm);
   if (!EFI_ERROR (Status)) {
     Status = CephRbdBootManagerHandoff (
                &Bm, &Esp, EspPartitionNumber,
                CEPH_RBD_BM_AUTO_BOOT_INDEX,
                FALSE, ImageHandle, &BootIndex
                );
   }
   ```

   `FALSE` 表示仅变量交接；改成 `TRUE` 才执行立即 `LoadImage`/`StartImage`。
   `EspPartitionNumber` 是 GPT 条目序号（1 基），由调用方从分区表索引推导。
3. `CephRbdDxe.inf` 的 `[Sources]` 增加 `CephRbdBootManager.c` 和
   `CephRbdBootManager.h`。
4. 不需要新增 `[Protocols]` 或 `[Guids]`：设备路径结构来自已有的 `MdePkg` 依赖，库类
   `UefiBootServicesTableLib`、`UefiRuntimeServicesTableLib`、`MemoryAllocationLib`、
   `BaseMemoryLib` 均已在 INF 中。若希望改用 `gEfiGlobalVariableGuid` 而非本地常量，
   则需在 `[Guids]` 添加它并自行替换 `mCephRbdGlobalVariableGuid`。
5. DSC 无需改动。
