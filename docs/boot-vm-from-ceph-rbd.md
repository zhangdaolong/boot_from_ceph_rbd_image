# Booting a virtual machine directly from a Ceph RBD image

This document describes the **firmware-side** approach implemented by this
project: the guest firmware (`CephRbdDxe.efi`) talks to Ceph itself and exposes
the RBD image as an `EFI_BLOCK_IO_PROTOCOL` device, so the host needs no Ceph
client, keyring, or RBD `<disk>` entry. The credentials live in firmware
configuration. This path is still in progress — see the status section below
and [`IMPLEMENTATION_STATUS.md`](../uefi-edk2/IMPLEMENTATION_STATUS.md).

The concrete values below come from the development cluster used for this
project. Replace them with your own.

```text
FSID      223a96f9-5a21-47e8-98a8-722553609174
mon.a     v1:192.168.122.89:40338   v2:192.168.122.89:40337
mon.b     v1:192.168.122.89:40340   v2:192.168.122.89:40339
mon.c     v1:192.168.122.89:40342   v2:192.168.122.89:40341
pool      rbd (pool id 4)
images    rbd/verifyimg, rbd/close-race-test
keyring   <build dir>/keyring  (client.admin for the dev cluster)
config    <build dir>/ceph.conf
```

> The vstart cluster uses non-standard high ports. A production cluster uses
> `6789` for messenger v1 and `3300` for messenger v2.


## Firmware-side boot from RBD (this project)

### B.1 Intended boot chain

```text
Power on
  │
  ▼
OVMF loads CephRbdDxe.efi
  │
  ├─ read CephRbdConfig UEFI variable (FSID, monitors, pool id, image,
  │  identity, key reference, START_ON_ENTRY)
  │
  ├─ UEFI TCP4: connect to a v1-enabled monitor port
  │
  ├─ messenger v1 connect + SEQ recovery
  │
  ├─ CephX challenge/response -> session authenticated
  │
  ├─ MON_COMMAND / OSD RPC: resolve rbd_id.<name>, read rbd_header metadata
  │
  ├─ install read-only EFI_BLOCK_IO_PROTOCOL on the controller
  │
  ▼
UEFI Boot Manager discovers GPT/ESP, loads \EFI\BOOT\BOOTX64.EFI
  │
  ▼
guest kernel + rootfs on the RBD image
```

Once this chain is complete the host only needs to provide networking to the
VM — it needs no host-side Ceph client, keyring, or RBD `<disk>` entry. The
credentials live in protected firmware configuration.

### B.2 Current implementation status (be honest about this)

The following pieces exist and are host-tested, and most have been validated
against a real Ceph 22 cluster (see B.5):

- Ceph messenger v1 framing, connect negotiation, `TAG_SEQ` recovery, ACKs,
  keepalive, bounded connect retry and a retransmit/replay queue.
- CephX AES-128-CBC challenge-response driven from the connect flow, ticket
  decryption/validation, OSD service-ticket request and authorizer verification.
- Monitor/OSD RPC codecs with automatic reconnect/replay on recoverable errors.
- CRUSH map parsing and object-to-PG-to-acting-OSD placement against a live
  OSDMap.
- RBD format 2 metadata discovery (`get_id`, `get_size`, `get_features`,
  `get_object_prefix`) and read-only extent mapping, with a host-side probe
  confirming a full metadata + data-block read over authenticated per-OSD
  sessions.
- A fixed-size `CephRbdConfig` firmware variable plus an
  `EFI_DRIVER_BINDING_PROTOCOL` that maps the image with per-monitor failover.
- A read-only `EFI_BLOCK_IO_PROTOCOL` publication path.
- GPT/ESP discovery (`CephRbdGpt.c`): protective MBR check, GPT header CRC,
  128-byte partition entry parsing, ESP type-GUID match and the
  `\EFI\BOOT\BOOTX64.EFI` fallback path; the host-side probe confirms an ESP is
  found on a GPT-formatted RBD image over the DXE BlockIo read path
  (`gpt_esp: status=0 esp_lba=6144-8192`) while a non-GPT image is rejected
  (`status=-4`). Long read sequences are safe: lossy OSD sessions skip
  retransmit tracking and short sparse reads are zero-filled like librbd.
- A real EDK II build: `bash scripts/build-edk2.sh` produces `CephRbdDxe.efi`
  (PE32+ EFI boot service driver) from `CephRbdPkg.dsc` with GCC5.
- GPT/ESP → UEFI Boot Manager handoff (`CephRbdBootManager.c`): builds an
  HD(GPT)+FilePath device path for the ESP, installs a `Boot####` load option
  and registers it in `BootOrder`, with an optional immediate
  `LoadImage`/`StartImage` of the loader. A minimal FAT12/16 ESP reader
  (`CephRbdEspFat.c`) reads `\EFI\BOOT\BOOTX64.EFI` straight from the mapped
  RBD image into a buffer; because the synthetic HD(GPT) device path has no
  partition/FAT child handles when the driver is loaded from the UEFI shell,
  TRY_BOOT loads the buffer directly with `LoadImage`/`StartImage`.
- DHCP/static local IPv4 configuration (`CephRbdNetwork.c`): by default the
  platform (OVMF) DHCP auto-config is used; a `CephRbdNetworkConfig` UEFI
  variable can supply a static station address/subnet mask.
- `KEY_REFERENCE` credentials resolved through a platform secret resolver
  (`CephRbdSecretResolver.c`) backed by a protected UEFI variable, and
  CephX AES-256-KRB5 (AES-256-CBC-CTS CS3 + HMAC-SHA384[0..23]) support.
- Legacy BIOS PXE/UNDI (`!PXE` ROM discovery and raw Ethernet frame I/O) in
  `bios/stage2/pxe_undi.c`.

### B.2.1 End-to-end OVMF guest boot — validated

The complete firmware-side chain has been exercised in an OVMF guest on the
test cluster (10.40.125.243, Ceph `192.168.122.89`). A network-enabled OVMF
(`scripts/build-ovmf-network.sh`, because the distro `OVMF_CODE.fd` omits
NetworkPkg) loads `CephRbdDxe.efi` from the shell; the driver authenticates to
the monitor, maps the `boot-test` RBD image, discovers its GPT/ESP, registers
`Boot0005`, reads `\EFI\BOOT\BOOTX64.EFI` from the ESP into a buffer and starts
it. The guest serial log confirms every step:

```text
CephRbdDxe: monitor 0 192.168.122.89:40340 init status=0x0
CephRbdDxe: image mapped, BlockIo published
CephRbdDxe: BootManager handoff status=Success Boot0005
CephRbdDxe: FAT loader read status=Success size=24576
CephRbdDxe: LoadImage(buffer) status=Success
* CEPH_RBD_BOOTLOADER_STARTED: booted from Ceph RBD ESP  *
CephRbdDxe: StartImage status=Success
CephRbdDxe: try-boot-from-buffer status=%r
```

The `CEPH_RBD_BOOTLOADER_STARTED` marker is printed by `CephRbdBootTest.efi`
(the minimal ESP app), proving firmware executed a binary read directly from
the Ceph RBD image — no host-side `librbd`, QEMU `-drive rbd:` or libvirt volume
is involved. `CephRbdBootTest.efi` is a validation helper only and must not be
shipped in a production image.

The following are still outstanding (protocol/tooling work, not gaps in the
firmware-side boot chain above): full ARP/IP/DHCP + Ceph socket transport over
BIOS UNDI, and AES-256 live cluster validation.

See [`IMPLEMENTATION_STATUS.md`](../uefi-edk2/IMPLEMENTATION_STATUS.md) for the
per-area table and [`firmware-rbd-protocol.md`](firmware-rbd-protocol.md) for the
protocol contract.

### B.3 Step-by-step: build and boot a guest from an RBD image

The flow has seven stages. Every command and every option is explained below.
Paths and values match the development cluster; substitute your own.

```text
Stage 0  Prerequisites (Ceph cluster, identity, EDK II workspace, host tools)
Stage 1  Create the Ceph identity and extract its AES key
Stage 2  Prepare the RBD image: GPT + ESP + \EFI\BOOT\BOOTX64.EFI  (build time only)
Stage 3  Build a network-enabled OVMF  (build-ovmf-network.sh)
Stage 4  Build the DXE driver and validation apps  (build-edk2.sh)
Stage 5  Generate CephRbdConfig.bin / CephRbdNetworkConfig.bin
Stage 6  Embed the blobs, assemble the ESP boot disk, write startup.nsh
Stage 7  Launch the guest with QEMU and verify CEPH_RBD_BOOTLOADER_STARTED
```

> Stage 2 uses host Ceph tools **once, at image-build time**, to lay down a GPT
> and an ESP. From Stage 7 onward the VM boots with **no** host-side Ceph
> client, keyring, `rbd map`, or QEMU `-drive rbd:` — the firmware does all of
> its own Ceph I/O.

#### Stage 0 — Prerequisites

- A Ceph cluster with at least one **messenger-v1-enabled** monitor endpoint.
  A production cluster uses `6789` for v1 (`3300` is v2-only and will **not**
  work with this messenger); the vstart dev cluster uses non-standard high
  ports (`40338/40340/40342` here). Check the real endpoints with:
  ```bash
  ceph mon dump                 # "v1:" addresses are the usable ones
  ```
- An EDK II workspace containing `edksetup.sh` (this project built against
  `/home/ldzh/edk2-work/edk2`). The build scripts assume GCC5, nasm and iasl;
  see [`docs/build/edk2-build.md`](build/edk2-build.md) for the toolchain setup
  and the isolated `UEFI_TOOLS_BIN` directory used on this host.
- Host tools: `python3`, `qemu-system-x86_64`, and `mtools` (`mformat`, `mcopy`).
  Stage 2 additionally needs `rbd`, `sgdisk` and `mkfs.vfat` on a machine that
  does have a Ceph client.
- Guest→Ceph network reachability. The validation guest used QEMU user-mode
  NAT and reached the Ceph endpoints at `192.168.122.89`.

#### Stage 1 — Create the Ceph identity and extract its AES key

```bash
ceph auth get-or-create client.firmware01 \
  mon 'allow r' \
  osd 'allow r class-read' \
  pool rbd 'allow r class-read'
```

| Argument | Meaning |
|---|---|
| `get-or-create client.firmware01` | Create the identity if absent, otherwise print its existing key. Use a dedicated identity per VM — never `client.admin`. |
| `mon 'allow r'` | Read access to the monitor (needed for `MON_GET_MAP` / `osd getmap`). |
| `osd 'allow r class-read'` | Read access **plus** `class-read`, which permits RBD metadata `CEPH_OSD_OP_CALL` class methods (`rbd.get_id`, `rbd.get_size`, `rbd.get_features`, `rbd.get_object_prefix`). Without `class-read` these return `-EPERM`/`result=-1`. |
| `pool rbd 'allow r class-read'` | Equivalent per-pool form; use this when you scope caps to the pool that holds the image instead of globally. |

The identity's key type must be `aes` (AES-128) or `aes256` (AES-256-KRB5,
`CEPH_CRYPTO_AES256KRB5`). To generate one explicitly:

```bash
ceph-authtool --gen-print-key --key-type=aes        # or --key-type=aes256
```

Ceph stores the secret as base64-encoded `CryptoKey`; the firmware needs the
raw key in hex. Decode it with:

```bash
python3 - <<'PY'
import base64, struct
b64 = "<output of: ceph auth print-key client.firmware01>"
raw = base64.b64decode(b64)
# CryptoKey wire layout: u16 type, u32 created_sec, u32 created_nsec,
#                        u16 len, then len key bytes -> key starts at 12.
ktype, sec, nsec, klen = struct.unpack_from("<HIIH", raw, 0)
key = raw[12:12 + klen]
print("type=%d len=%d key-hex=%s" % (ktype, klen, key.hex()))
PY
```

For the development `firmware01` identity this yields the 16-byte secret
`42bf0d364befdff7d95e2c3058f2729c` used in the config blob below. Keep this
value out of public binaries; `key-source` (Stage 5) is the production-safe
form.

#### Stage 2 — Prepare the RBD image (build time only)

Create a small image, write a protective MBR + GPT with one EFI System
Partition, put the bootloader at `\EFI\BOOT\BOOTX64.EFI`, then unmap it:

```bash
rbd create boot-test --size 64 --pool rbd
sudo rbd map rbd/boot-test --id admin --keyring /etc/ceph/ceph.client.admin.keyring
sudo sgdisk --zap-all /dev/rbd0
sudo sgdisk --new=1:6144:0 --typecode=1:EF00 /dev/rbd0
sudo mkfs.vfat -F12 -s 1 -n CEPHESP /dev/rbd0p1
mkdir -p /mnt/esp && sudo mount /dev/rbd0p1 /mnt/esp
sudo mkdir -p /mnt/esp/EFI/BOOT
sudo cp CephRbdBootTest.efi /mnt/esp/EFI/BOOT/BOOTX64.EFI
sudo umount /mnt/esp && sudo rbd unmap /dev/rbd0
```

| Argument | Meaning |
|---|---|
| `rbd create boot-test --size 64 --pool rbd` | Create a 64 MiB image named `boot-test` in pool `rbd` (the config `image` field must match). |
| `rbd map ... --id admin --keyring ...` | Map the image to a local block device **only for preparation**. `--id` is the Ceph identity; `--keyring` points at its keyring file. |
| `sgdisk --zap-all /dev/rbd0` | Remove any existing MBR/GPT so the new table is clean. |
| `sgdisk --new=1:6144:0 --typecode=1:EF00 /dev/rbd0` | Create partition 1 starting at LBA 6144 through the end (`0`); `EF00` is the EFI System Partition type GUID. The driver's ESP discovery reports `esp_lba=6144-...`. |
| `mkfs.vfat -F12 -s 1 -n CEPHESP /dev/rbd0p1` | Make a FAT12 filesystem with 1 sector per cluster and volume label `CEPHESP`; the built-in reader supports FAT12/16. For larger ESPs use `-F16`. |
| `cp CephRbdBootTest.efi .../BOOTX64.EFI` | Place the validation app at the x64 fallback path the firmware reads. Replace it with a real Guest Firmware/OS loader in production. |

For a quick smoke test, `CephRbdBootTest.efi` is built in Stage 4; it only
prints `CEPH_RBD_BOOTLOADER_STARTED` and is never part of a production image.

#### Stage 3 — Build a network-enabled OVMF

The distribution `OVMF_CODE.fd` on Debian/Ubuntu omits NetworkPkg, so
`EFI_TCP4_PROTOCOL` is unavailable and the driver cannot connect. Build a full
OVMF with networking:

```bash
bash scripts/build-ovmf-network.sh /home/ldzh/edk2-work/edk2
```

| Argument | Meaning |
|---|---|
| `/home/ldzh/edk2-work/edk2` (positional `$1`, env `EDK2_WORKSPACE`) | EDK II workspace root containing `edksetup.sh`. Omit to use the default. |
| `-D NETWORK_ENABLE=TRUE` | Include the UEFI network stack (MNP/ARP/IP4/TCP4/UDP4/DHCP4). |
| `-D NETWORK_IP4_ENABLE=TRUE` / `-D NETWORK_IP6_ENABLE=FALSE` | Enable IPv4 (used by this driver) and disable IPv6 to reduce build size. |
| `-D NETWORK_TLS_ENABLE=FALSE`, `-D NETWORK_HTTP_BOOT_ENABLE=FALSE` | Disable TLS/HTTP boot — not needed here. |
| `-D NETWORK_SNP_ENABLE=TRUE` | Keep the Simple Network Protocol, which TCP4 sits on. |
| `-D TPM2_ENABLE=FALSE`, `-D CC_MEASUREMENT_ENABLE=FALSE` | Simplify the build for validation. |

Outputs (confirm with the script's `BUILD OK` message):

```text
<EDK2>/Build/OvmfX64/DEBUG_GCC5/FV/OVMF_CODE.fd
<EDK2>/Build/OvmfX64/DEBUG_GCC5/FV/OVMF_VARS.fd
```

Run the script with `bash`, not `sh` (it uses `BASH_SOURCE`). `TMPDIR` defaults
to `/home/ldzh/edk2-work/tmp` because `/tmp` is a tiny tmpfs on this host.

#### Stage 4 — Build the DXE driver and validation apps

```bash
bash scripts/build-edk2.sh \
  /home/ldzh/edk2-work/edk2 \
  /home/ldzh/WorkBuddy/2026-11-25-15-20-29/ceph-rbd-image-boot/uefi-edk2
```

| Argument | Meaning |
|---|---|
| `/home/ldzh/edk2-work/edk2` (positional `$1`, env `EDK2_WORKSPACE`) | EDK II workspace root; must contain `edksetup.sh`. |
| `.../uefi-edk2` (positional `$2`, env `CEPH_PACKAGES_PATH`) | Directory containing `CephRbdPkg`; appended to `PACKAGES_PATH` ahead of the EDK II tree. |
| `TMPDIR` | gcc scratch directory; override if your `/tmp` is small. |
| `UEFI_TOOLS_BIN` | Directory holding `nasm`/`iasl` when they are not installed system-wide. |
| `PACKAGES_PATH` | If set externally, the script uses it verbatim instead of composing one. |

Internally the script runs `build -p CephRbdPkg/CephRbdPkg.dsc -a X64 -b DEBUG
-t GCC5` and generates a corrected `CephRbdPkg.dec` overlay so the hyphenated
GUID in the repo file is accepted. Outputs:

```text
<EDK2>/Build/CephRbdPkg/DEBUG_GCC5/X64/CephRbdDxe.efi
<EDK2>/Build/CephRbdPkg/DEBUG_GCC5/X64/CephRbdSetVar.efi
<EDK2>/Build/CephRbdPkg/DEBUG_GCC5/X64/CephRbdBootTest.efi
```

#### Stage 5 — Generate the configuration blobs

**5.1 `CephRbdConfig` (372 bytes, fixed layout, no DXE heap allocation):**

```bash
python3 scripts/make-ceph-rbd-config.py config/ceph-rbd-example.ini CephRbdConfig.bin
```

| Argument | Meaning |
|---|---|
| `config/ceph-rbd-example.ini` (positional 1) | Source INI. The parser reads only the ` ```ini ... ``` ` block, so the template can carry surrounding documentation. |
| `CephRbdConfig.bin` (positional 2) | Output blob for the `CephRbdConfig` UEFI variable (GUID `9E5F1A2B-3C4D-4E5F-8A9B-0C1D2E3F4A5B`). |

INI fields (use the example below; all are required unless noted):

```ini
[ceph]
fsid = 223a96f9-5a21-47e8-98a8-722553609174
monitors = 192.168.122.89:40340,192.168.122.89:40342
pool = 4
image = boot-test
client-id = firmware01
key-hex = 42bf0d364befdff7d95e2c3058f2729c
flags = 14
```

| Field | Meaning |
|---|---|
| `fsid` | Ceph cluster FSID (`ceph -s`); parsed with `uuid.UUID`, so hyphens are accepted. |
| `monitors` | Comma-separated `IP:port` list (1–4 entries). **Must be v1-enabled ports**; v2-only `3300` will not work. The driver fails over between them. |
| `pool` | Numeric pool id (`ceph osd lspools` → `rbd` is `4` here), not the pool name. |
| `image` | RBD image name without the pool prefix (must match Stage 2). |
| `client-id` | Ceph identity id (Stage 1), e.g. `firmware01`. |
| `key-hex` / `key-source` | Exactly one is required. `key-hex` = 32 hex digits / 16 raw AES-128 bytes consumed directly (clears flag `0x1`). `key-source` = opaque reference string that sets flag `0x1` and is resolved at driver start by `CephRbdSecretResolver.c` from a UEFI variable under GUID `6F3D9C1A-7B52-4E08-A1C3-C35E7209A48B` whose data is the raw key (16 bytes for AES-128, 32 for AES-256-KRB5). Use `key-source` in production. |
| `flags` | Integer bitmask (parsed with base 0, so `14`/`0xE` both work). See below. |

`flags` bitmask:

| Bit | Name | Effect |
|---|---|---|
| `0x1` | `KEY_REFERENCE` | Key field is an opaque reference resolved via the platform secret resolver. |
| `0x2` | `START_ON_ENTRY` | Map the image directly from the DXE entry point (no controller binding needed). |
| `0x4` | `BOOT_MANAGER` | Discover GPT/ESP and install a `Boot####` load option. |
| `0x8` | `TRY_BOOT` | With `BOOT_MANAGER`, `LoadImage`/`StartImage` the loader immediately. |

Combine as needed: `flags = 6` (`0x2|0x4`) registers the load option without
auto-booting; `flags = 14` (`0x2|0x4|0x8`, used for the validated guest boot)
also starts the loader from the buffer.

**5.2 `CephRbdNetworkConfig` (36 bytes, optional — skip for platform DHCP):**

```bash
python3 scripts/make-ceph-rbd-network-config.py static CephRbdNetworkConfig.bin \
  --station 10.0.2.15 --mask 255.255.255.0 --gateway 10.0.2.2
```

| Argument | Meaning |
|---|---|
| `static` / `dhcp` (positional 1) | `dhcp` uses the platform default address (OVMF auto-config); `static` supplies addresses below. |
| `CephRbdNetworkConfig.bin` (positional 2) | Output blob for the `CephRbdNetworkConfig` variable (GUID `A7E4C1B2-3D5F-4E68-9B12-770A44632EF1`). |
| `--station` | Static IPv4 address of the guest (required in `static` mode). |
| `--mask` | Subnet mask (required in `static` mode). |
| `--gateway` | Default gateway (optional; sets the "gateway valid" flag bit). |
| `--dns` | DNS server (optional). |

The values above are the guest side of QEMU user-mode NAT. With DHCP mode the
platform assigns the address and these options are ignored.

#### Stage 6 — Embed the blobs, assemble the ESP boot disk, write startup.nsh

The validation helper `CephRbdSetVar.efi` writes the blobs compiled into it as
UEFI variables (the Shell `setvar` command cannot create variables under a
non-EDK2-global GUID). Convert the two `.bin` files to C arrays and replace
`uefi-edk2/CephRbdPkg/CephRbdSetVar/CephRbdConfigBlob.h`:

```bash
xxd -i CephRbdConfig.bin        # emits unsigned char CephRbdConfig_bin[] and CephRbdConfig_bin_len
xxd -i CephRbdNetworkConfig.bin # emits unsigned char CephRbdNetworkConfig_bin[] and ..._len
```

Rename the emitted arrays to match the header's expectations
(`unsigned char mCephRbdConfig[]` with `#define MCEPHRBDCONFIG_SIZE 372`, and
`unsigned char mCephRbdNetworkConfig[]` with
`#define MCEPHRBDNETWORKCONFIG_SIZE 36`), then rebuild (Stage 4) so
`CephRbdSetVar.efi` carries the new blobs. Alternatively,
from a UEFI shell with a FAT-formatted `fs0:` containing the `.bin` files:

```text
fs0:> setvar CephRbdConfig -guid 9E5F1A2B-3C4D-4E5F-8A9B-0C1D2E3F4A5B -bs CephRbdConfig.bin
fs0:> setvar CephRbdNetworkConfig -guid A7E4C1B2-3D5F-4E68-9B12-770A44632EF1 -bs CephRbdNetworkConfig.bin
```

Build a FAT boot image containing the three `.efi` files and an auto-run
`startup.nsh`, then run the guest:

```bash
mformat -i boot.img -f 1440 ::
mcopy -i boot.img CephRbdDxe.efi CephRbdSetVar.efi CephRbdBootTest.efi ::
mcopy -i boot.img startup.nsh ::
```

`startup.nsh` (the exact sequence used for the validated boot):

```text
echo MARK_BEGIN
CephRbdSetVar.efi
load fs0:\CephRbdDxe.efi
echo MARK_END
```

For static IP, insert before `load` (QEMU user-net guest address):

```text
ifconfig -s eth0 static 10.0.2.15 255.255.255.0 10.0.2.2
```

| Shell command | Meaning |
|---|---|
| `CephRbdSetVar.efi` | Write `CephRbdConfig`/`CephRbdNetworkConfig` with attributes NV+BS+RT. |
| `load fs0:\CephRbdDxe.efi` | Load **and** start the DXE driver; its binding sees the config and maps the image. `load -nc` loads without starting (useful with `connect` to inspect DriverBinding). |
| `ifconfig -s eth0 static ...` | Assign the static guest address; with DHCP this step is omitted. |
| `dh -p 964E5B21-6459-11D2-8E39-00A0C969723B` | List handles exposing the Block I/O protocol GUID — confirms the RBD image was published. |
| `dh -p 18A03E5C-06F8-4D9D-9B2E-4A7E5F6C3A18` | List handles exposing the Driver Binding protocol GUID. |

#### Stage 7 — Launch the guest with QEMU and verify

```bash
timeout 90 qemu-system-x86_64 \
  -m 512 -nographic -machine accel=kvm \
  -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=vars-net.fd \
  -drive file=boot.img,format=raw,if=virtio \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -device virtio-rng-pci \
  -monitor none -serial stdio -display none
```

| Option | Meaning |
|---|---|
| `-m 512` | 512 MB guest RAM. |
| `-nographic` | No graphical display; firmware/serial output goes to the terminal. |
| `-machine accel=kvm` | Use KVM acceleration; use `accel=tcg` if KVM is unavailable. |
| `-drive if=pflash,...,readonly=on,file=OVMF_CODE.fd` | OVMF code volume (read-only). Must be the network-enabled build from Stage 3. |
| `-drive if=pflash,...,file=vars-net.fd` | Writable variable store — take a **fresh copy** of `OVMF_VARS.fd` per VM so NVRAM format matches the CODE build. |
| `-drive file=boot.img,format=raw,if=virtio` | The FAT ESP disk from Stage 6 (`fs0:` in the shell). |
| `-netdev user,id=net0 -device virtio-net-pci,netdev=net0` | User-mode NAT networking (`10.0.2.0/24`) with a virtio NIC; this is the TCP4 path to Ceph. |
| `-device virtio-rng-pci` | Entropy source — required by the CephX/random-number code; without it the guest can stall. |
| `-monitor none -serial stdio -display none` | Disable the QEMU monitor/framebuffer and stream the serial console to stdout. |
| `timeout 90` | Bound the run so a hang does not block automation. |

Expected serial tail (the exact validated result):

```text
CephRbdDxe: monitor 0 192.168.122.89:40340 init status=0x0
CephRbdDxe: image mapped, BlockIo published
CephRbdDxe: BootManager handoff status=Success Boot0005
CephRbdDxe: FAT loader read status=Success size=24576
CephRbdDxe: LoadImage(buffer) status=Success
* CEPH_RBD_BOOTLOADER_STARTED: booted from Ceph RBD ESP  *
CephRbdDxe: StartImage status=Success
CephRbdDxe: try-boot-from-buffer status=Success
```

See B.2.1 for the full log and B.5 for the host-side protocol probe used to
debug each layer separately.

**Troubleshooting**

| Symptom | Likely cause / fix |
|---|---|
| driver cannot find `EFI_TCP4_PROTOCOL` | OVMF lacks NetworkPkg — rebuild with Stage 3. |
| monitor connect times out | Wrong port or no v1 endpoint — use the `v1:` port from `ceph mon dump` (`6789` in production, not `3300`), and check guest→Ceph reachability. |
| `rbd.get_id result=-1` / `dxe_discover status=-13` | Identity missing `class-read` — re-run Stage 1 `ceph auth caps`. |
| `result=-1` on metadata calls with `allow r` only | Same as above: `allow r class-read` is required. |
| Block I/O handles disappear / duplicate mapping | Old driver loaded twice — use one `load`, or `load -nc` + `connect`; the driver returns `EFI_ALREADY_STARTED` on a second `Start()`. |
| `LoadImage(buffer) Not Found` on a synthetic HD path | Expected when loaded from the shell — TRY_BOOT reads the loader into a buffer instead; ensure `flags` includes `0x4|0x8` and the ESP holds `\EFI\BOOT\BOOTX64.EFI`. |
| guest stalls before CephX | Add `-device virtio-rng-pci` for entropy. |

### B.4 Running the OVMF smoke test (does not boot from RBD)

```bash
bash scripts/run-ovmf-ceph-rbd.sh [OVMF_CODE_FD] [KVM|TCG] [OVMF_VARS_FD]
```

This only verifies that OVMF starts and that a temporary variable store is
used. It does not load `CephRbdDxe.efi` and does not connect to Ceph.

### B.5 Validating the live protocol path (host-side probe, no guest)

A POSIX-socket probe exercises the same messenger/RPC code used by the driver
against a real monitor, without needing a guest or firmware:

```bash
sh tests/run-live-tests.sh           # builds tests/live/.build/live_probe
# no authentication (baseline): connect + MON_GET_MAP are dropped
tests/live/.build/live_probe 40338
# full handshake: connect + CephX MAuth + MON_GET_MAP + MonMap epoch
tests/live/.build/live_probe 40338 \
  223a96f95a2147e898a8722553609174 firmware01 \
  42bf0d364befdff7d95e2c3058f2729c
```

A complete run performs messenger v1 connect (including `TAG_SEQ`), the CephX
challenge-response handshake, then `MON_GET_MAP` and decodes the MonMap epoch.
With an authenticated identity it goes on to request the OSDMap, place the
object via CRUSH, connect to the primary OSD, issue a real `CEPH_OSD_OP_READ`,
and finally drive the same DXE BlockIo discovery + read path the firmware uses:

```text
connect: status=0 seq=1/0 tag=13 action=0
auth: status=0 global_id=28407 has_session_key=1
osd_service_ticket: status=0 valid=1 secret_id=306 blob_len=150
get_map: status=0 reply_len=461
epoch: status=0 value=2
osd_getmap: status=0 reply_len=78 map_len=4100
placement: status=0 epoch=3765 pg_num=32 pgp_num=32 rule=0 size=3
test object: status=0 pg=28 osd_count=3 acting=[2,1,0]
osd_connect: osd=2 addr=192.168.122.89:6819 status=0 ready=1
osd_read: status=0 result=0 num_ops=1 data_len=16
dxe_discover: status=0 ready=1 image_size=1073741824
dxe_read: status=0 data: 7d ef f3 7e 35 30 b6 60 ...
gpt_esp: status=-4 last_lba=2097151
```

`MON_GET_MAP` is answered with a **MonMap** (`CEPH_MSG_MON_MAP=4`), not an
OSDMap; the probe uses a separate `osd getmap` MON command for the OSDMap.
Without the CephX step the monitor drops the request with `is not
authenticated`, which is how the need for the handshake was isolated. The
`dxe_discover`/`dxe_read` lines confirm the full RBD path — metadata class
methods plus a data-block read over the primary OSD session — works against a
real cluster. If `dxe_discover` fails with `status=-13` and the class-method
`result=-1`, the identity is missing the `class-read` cap (see B.3). The
`gpt_esp` line above is `status=-4` because this raw test image has no GPT;
set `CEPH_RBD_TEST_IMAGE` to a GPT-formatted image to verify ESP discovery:

```sh
CEPH_RBD_TEST_IMAGE=gpt-test tests/live/.build/live_probe 40338 \
  223a96f95a2147e898a8722553609174 firmware01 \
  42bf0d364befdff7d95e2c3058f2729c
# ...
# dxe_discover: status=0 ready=1 image_size=16777216
# gpt_esp: status=0 last_lba=32767 esp_lba=6144-8192
```

---

## Status and next steps

Track implementation progress in
[`IMPLEMENTATION_STATUS.md`](../uefi-edk2/IMPLEMENTATION_STATUS.md). The guide
above documents the intended configuration so the pieces can be validated
incrementally, B.5 shows the live-protocol validation, and B.2.1 records the
end-to-end OVMF guest boot that is passing against a real cluster.

**Never** put the development `client.admin` key into a guest image, a firmware
binary, or a public repository. Create a dedicated identity with the minimum
caps (`allow r class-read`) for each VM.
