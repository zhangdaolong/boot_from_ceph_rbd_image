# EDK II / OVMF 实际构建 CephRbdDxe

本文记录用真实 EDK II 工具链编译 `CephRbdDxe.efi` 的环境准备与命令。仅做**构建使能**，不实现协议功能，不做真机测试。

## 1. 环境准备（本机已完成）

- EDK II 工作区：`/home/ldzh/edk2-work/edk2`（commit `fd16b14`，稀疏检出）
- 编译器：系统 gcc 14.2；EDK II 标签 `GCC5`
- `/tmp` 只有 10 MB tmpfs，gcc 中间文件会写爆；构建统一使用
  `TMPDIR=/home/ldzh/edk2-work/tmp`（脚本自动设置）
- 没有可用的 dpkg 数据库，`apt-get install` 无法运行。nasm/iasl 通过直接下载
  Ubuntu 25.04 的 `.deb` 并 `dpkg-deb -x` 解包到隔离目录获得：
  - `nasm_2.16.03-1_amd64.deb`
  - `acpica-tools_20241212-2_amd64.deb`（提供 `iasl`）
  - 解包到 `/home/ldzh/edk2-work/tools/root/usr/bin`；可用 `UEFI_TOOLS_BIN` 覆盖
- 子模块未初始化；git submodule 因 pinned commit 无法解析而失败，改为下载精确
  commit 的源码 tarball：
  - `BaseTools/Source/C/BrotliCompress/brotli` (db24a98)
  - `CryptoPkg/Library/OpensslLib/openssl` (8cf17aa)
  - `CryptoPkg/Library/MbedTlsLib/mbedtls` (0bebf8b)
  - `MdePkg/Library/MipiSysTLib/mipisyst` (370b594)
  - `MdeModulePkg/Library/BrotliCustomDecompressLib/brotli` (db24a98)
- BaseTools 已用 `make -C BaseTools -j4` 编译成功（产物在 `BaseTools/Source/C/bin`）

## 2. 构建命令

```sh
cd /home/ldzh/WorkBuddy/2026-11-25-15-20-29/ceph-rbd-image-boot
./scripts/build-edk2.sh
```

覆盖工作区：

```sh
EDK2_WORKSPACE=/path/to/edk2 ./scripts/build-edk2.sh
# 或
./scripts/build-edk2.sh /path/to/edk2 /path/to/packages
```

脚本会 `source edksetup.sh`，设置 `PACKAGES_PATH`（仓库 `uefi-edk2` 在前，EDK II 在后），
然后执行：

```sh
build -p CephRbdPkg/CephRbdPkg.dsc -a X64 -b DEBUG -t GCC5
```

## 3. 脚本内的构建环境兼容层

- 当前 EDK II 已移除 `GCC5` 工具链标签（更名为 `GCC`）。脚本在缺失时从
  `tools_def.template` 生成一份 `GCC5` 别名写入 `Conf/tools_def.txt`，
  使 `-t GCC5` 仍可用，无需改动上游文件。
- `CephRbdPkg.dec` 中 `gCephRbdConfigGuid` 使用了带连字符的 GUID 写法，而 EDK II
  的 `.dec` 文件只接受 C 结构体形式。由于 `.dec` 不在本次允许修改的文件范围内，
  脚本会在 `TMPDIR/ceph-dec-overlay` 生成一份修正过的 DEC 覆盖层并置于
  `PACKAGES_PATH` 最前，原文件保持不变。**建议后续直接修正源文件**（见第 5 节）。

## 4. 元数据修复

- `CephRbdPkg.dsc` 增加 `CephRbdPkg/CephRbdPkg.dec` 到 `[Packages]`。
- `CephRbdDxe.inf` 同步增加该 `.dec` 到 `[Packages]`。
- `CephRbdPkg.dsc` 补齐库类映射：`UefiRuntimeServicesTableLib`、`OpensslLib`、
  `IntrinsicLib`、`PrintLib`、`RngLib`、`SynchronizationLib`、`PcdLib`、
  `TimerLib`、`DebugPrintErrorLevelLib`、`DevicePathLib`、`StackCheckLib`、
  `RegisterFilterLib`。

## 5. 当前构建状态：失败（源码问题，需用户修复）

完整 `BaseCryptLib`/`OpensslLib` 已成功编译；所有元数据问题已解决。构建最终在链接
阶段失败。以下源码问题均**未在本次修改**（按范围要求只报告）：

1. `CephRbdDxe.c:532` — 使用 `gRT` 但未声明。
   建议：在 `#include <Library/UefiBootServicesTableLib.h>` 之后加
   `#include <Library/UefiRuntimeServicesTableLib.h>`。
2. `CephRbdFormat2.c:45` — `LShiftU64` 隐式声明。
   建议：加 `#include <Library/BaseLib.h>`。
3. `CephRbdGpt.h:15-16` — `UINT64_C` / `UINT32_C` 隐式声明。
   建议：加 `#include <stdint.h>`，或改用 `(UINT64)0x...ULL` / `(UINT32)0x...U` 常量。
4. `CephRbdConfig.h:50` 声明 `extern CONST CHAR16 gCephRbdConfigVariableName[]`，
   但**没有任何 `.c` 定义它**；`CephRbdConfig.c:112` 引用它，链接报
   `undefined reference to gCephRbdConfigVariableName`。
   建议：在 `CephRbdConfig.c` 中定义，例如
   `CONST CHAR16 gCephRbdConfigVariableName[] = L"CephRbdConfig";`（名称请按实际约定确认）。

修复上述 4 项后重新运行 `./scripts/build-edk2.sh`，预期产物：

```
/home/ldzh/edk2-work/edk2/Build/CephRbdPkg/DEBUG_GCC5/X64/CephRbdDxe.efi
```
