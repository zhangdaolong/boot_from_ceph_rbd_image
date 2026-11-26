# Booting a virtual machine directly from a Ceph RBD image

This document explains how to make a VM boot from an image that lives in Ceph RBD.
There are **two fundamentally different ways** to do this, and it is important
not to confuse them:

| | Path A — hypervisor-side RBD | Path B — firmware-side RBD (this project) |
|---|---|---|
| Who talks to Ceph | QEMU/libvirt via `librbd`, on the host | UEFI DXE driver (`CephRbdDxe.efi`) inside the VM firmware |
| What the guest sees | a normal virtio-blk device provided by QEMU | an `EFI_BLOCK_IO_PROTOCOL` device provided by firmware |
| Host needs `librbd` + credentials | **yes** | no |
| Works today | **yes** | **no — see status below** |
| Survives live migration / host change | depends on host Ceph access | yes, credentials are in firmware config |

Most "boot a VM from RBD" recipes on the Internet are **Path A**. This repository
is implementing **Path B** so that the guest firmware itself can read the boot
disk without a host-side Ceph client. Use Path A today; use Path B once the
driver is finished.

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

---

## Path A — QEMU / libvirt boot from RBD (works today)

### A.1 Prerequisites

- QEMU built with the RBD block driver (`--enable-rbd`, package `qemu-block-rbd`
  on RHEL-family hosts). Verify with:
  ```bash
  qemu-img --help | grep -w rbd      # 'rbd' appears in "Supported formats"
  ```
  If opening `rbd:` fails with `Unknown protocol 'rbd'`, the block driver is
  missing — install it before continuing.
- `librbd.so.1` present (`ldconfig -p | grep librbd`).
- `ceph.conf` and a keyring readable by the user that runs QEMU.
- A bootable OS image in the pool.

### A.2 Prepare a bootable image

```bash
# Create an empty image (format 2 is required for the firmware path as well).
rbd create rbd/vm-root --size 20480 --image-format 2

# Import an existing raw image, or install the OS with this image attached.
rbd import --image-format 2 vm.raw rbd/vm-root
rbd ls -p rbd                       # confirm vm-root is present
qemu-img info rbd:rbd/vm-root       # confirm QEMU can read the image
```

### A.3 Use a least-privilege identity (do not use client.admin in production)

```bash
ceph auth get-or-create client.vmboot \
    mon 'profile rbd' \
    osd 'profile rbd pool=rbd' \
    -o /etc/ceph/ceph.client.vmboot.keyring
```

For the development vstart cluster the existing `client.admin` key in
`<build dir>/keyring` is sufficient, but never copy that key into a guest or a
firmware image.

### A.4 Boot with the QEMU command line

Using a keyring file (recommended — the key never appears in the process list):

```bash
qemu-system-x86_64 \
  -enable-kvm -m 2048 -smp 2 \
  -drive file=rbd:rbd/vm-root:id=vmboot:keyring=/etc/ceph/ceph.client.vmboot.keyring:conf=/etc/ceph/ceph.conf,if=virtio,format=rbd,cache=none \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -vnc :0
```

Equivalent form with an inline key (useful for tests, not for production):

```bash
KEY="$(ceph auth get-key client.vmboot)"
qemu-system-x86_64 \
  -drive "file=rbd:rbd/vm-root:id=vmboot:key=$KEY:conf=/etc/ceph/ceph.conf,if=virtio,format=rbd" \
  ...
```

With OVMF firmware, add the pflash drives:

```bash
qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/var/lib/libvirt/qemu/nvram/vm-root_VARS.fd \
  ...
```

RBD URI syntax (fields after the image name are colon-separated):

```text
rbd:<pool>/<image>[@<snapshot>][:id=<client-id>][:key=<base64-key>][:keyring=<path>][:conf=<path>]
```

- `id=` is the Ceph client id **without** the `client.` prefix (`vmboot`, not
  `client.vmboot`).
- `conf=` points to a `ceph.conf` that lists the monitors. Without it QEMU tries
  the default `/etc/ceph/ceph.conf`.

### A.5 Boot with libvirt

Define a secret that holds the Ceph key:

```xml
<!-- ceph-secret.xml -->
<secret ephemeral='no' private='no'>
  <uuid>11111111-2222-3333-4444-555555555555</uuid>
  <usage type='ceph'>
    <name>client.vmboot secret</name>
  </usage>
</secret>
```

```bash
virsh secret-define ceph-secret.xml
virsh secret-set-value \
  --secret 11111111-2222-3333-4444-555555555555 \
  --base64 "$(ceph auth get-key client.vmboot)"
```

Then attach the RBD disk to the domain (in `virsh edit <vm>`):

```xml
<disk type='network' device='disk'>
  <driver name='qemu' type='raw' cache='none'/>
  <auth username='vmboot'>
    <secret type='ceph' uuid='11111111-2222-3333-4444-555555555555'/>
  </auth>
  <source protocol='rbd' name='rbd/vm-root'>
    <!-- production: port 3300 (msgr2) or 6789 (msgr1) -->
    <host name='192.168.122.89' port='40337'/>
    <host name='192.168.122.89' port='40339'/>
    <host name='192.168.122.89' port='40341'/>
  </source>
  <target dev='vda' bus='virtio'/>
  <boot order='1'/>
</disk>
```

Start the VM and confirm it boots:
```bash
virsh create vm.xml
virsh domblklist <vm>          # vda should be rbd:rbd/vm-root
```

Notes:

- `cache='none'` (or `cache='directsync'`) avoids host page cache coherency
  problems with RBD; do not use `writethrough` for a shared writable image.
- For a read-only boot disk, add `<readonly/>` and use a snapshot/clone so
  multiple VMs never write the same image concurrently.
- For this vstart cluster use the v2 high ports shown above. This project's
  messenger only implements **v1**, so the firmware path must use the
  `40338/40340/40342` ports (see Path B).

### A.6 Troubleshooting Path A

| Symptom | Likely cause / fix |
|---|---|
| `Unknown protocol 'rbd'` / `Unknown driver 'rbd'` | QEMU lacks the RBD block driver; install `qemu-block-rbd` / rebuild with `--enable-rbd`. |
| `failed for service _ceph-mon._tcp` | no monitor list; pass `conf=` with a valid `mon_host`, or add monitors to `<source>`. |
| `auth: no secret` / `Permission denied` | wrong `id=`/`username`, or the keyring file is not readable by the QEMU user. |
| `BADPROTOVER` / `msgr1` mismatch | the image is fine but the firmware probe used a v2 port; use a v1 port. |
| Guest boots but kernel panics on rootfs | the image has no bootloader/rootfs; install an OS first with the image attached as a data disk. |

---

## Path B — firmware-side boot from RBD (this project, in progress)

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
VM — it does **not** need `librbd`, a keyring, or an RBD `<disk>` entry. The
credentials live in protected firmware configuration.

### B.2 Current implementation status (be honest about this)

The following pieces exist and are host-tested:

- Ceph messenger v1 framing, connect negotiation, `TAG_SEQ` recovery, ACKs,
  keepalive, bounded connect retry and a retransmit/replay queue.
- CephX primitive layer: AES-128-CBC challenge-response, ticket decryption and
  validation, authorizer reply verification.
- Monitor/OSD RPC codecs with automatic reconnect/replay on recoverable errors.
- RBD format 2 metadata discovery (`get_id`, `get_size`, `get_features`,
  `get_object_prefix`, `get_stripe_unit_count`) and read-only extent mapping.
- A fixed-size `CephRbdConfig` firmware variable plus an
  `EFI_DRIVER_BINDING_PROTOCOL` that maps the image with per-monitor failover.
- A read-only `EFI_BLOCK_IO_PROTOCOL` publication path.

The following are **still missing**, so Path B cannot boot a VM yet:

- CephX challenge/ticket exchange is not driven from the connect flow, so a
  live monitor drops `MON_GET_MAP` with `is not authenticated, dropping`.
- CRUSH map parsing / PG-to-OSD placement, so OSD reads are not resolved against
  a real cluster.
- GPT/ESP discovery and UEFI Boot Manager integration.
- An actual EDK II / OVMF build of the driver, and end-to-end boot validation.

See [`IMPLEMENTATION_STATUS.md`](../uefi-edk2/IMPLEMENTATION_STATUS.md) for the
per-area table and [`firmware-rbd-protocol.md`](firmware-rbd-protocol.md) for the
protocol contract.

### B.3 Building the firmware driver

```bash
# 1. Build the DXE module against an EDK II workspace.
scripts/build-edk2.sh <EDK2_WORKSPACE> <PACKAGES_PATH>
#    produces CephRbdDxe.efi

# 2. Generate the CephRbdConfig blob from the INI template.
python3 scripts/make-ceph-rbd-config.py \
    config/ceph-rbd-example.ini CephRbdConfig.bin

# 3. Install the blob as a UEFI variable, and put CephRbdDxe.efi in the OVMF
#    firmware volume or load it from an ESP. From a UEFI shell:
#      fs0:> load CephRbdDxe.efi
#      fs0:> setvar CephRbdConfig -guid 9E5F1A2B-3C4D-4E5F-8A9B-0C1D2E3F4A5B -bs CephRbdConfig.bin
```

Template for `config/ceph-rbd-example.ini` (the generated blob is 372 bytes,
fixed layout, no heap allocation in DXE):

```ini
[ceph]
fsid = 223a96f9-5a21-47e8-98a8-722553609174
monitors = 192.168.122.89:40338,192.168.122.89:40340,192.168.122.89:40342
pool = 4
image = verifyimg
client-id = firmware01
key-hex = 42bf0d364befdff7d95e2c3058f2729c
# 0x2 makes the DXE entry point map the image without a controller binding.
flags = 2
```

- Monitor endpoints **must be v1-enabled**. The v2-only default port `3300`
  will not work with this messenger.
- `pool` is the numeric pool id (`ceph osd lspools` shows `rbd` is `4` here).
- Exactly one of `key-hex` or `key-source` must be set:
  - `key-hex` is a raw 16-byte AES-128 secret (32 hex digits), consumed
    directly by the CephX handshake. This clears the `KEY_REFERENCE` flag.
  - `key-source` is an opaque reference to protected firmware storage, which
    sets the `KEY_REFERENCE` flag. Resolving it needs a platform-specific
    credential provider that this generic driver does not include, so a config
    using `key-source` returns `EFI_UNSUPPORTED` at driver start.
- The secret must belong to a Ceph identity whose key type is `aes` (AES-128).
  Ceph's AES256-KRB5 keys are not supported by this firmware subset:
  ```bash
  ceph-authtool --gen-print-key --key-type=aes
  ```
- The identity needs the `class-read` cap on the OSD side, not just plain
  `allow r`. The driver discovers an image through RBD metadata class methods
  (`rbd.get_id`, `rbd.get_size`, `rbd.get_features`,
  `rbd.get_object_prefix`); these are `CEPH_OSD_OP_CALL` ops, and Ceph rejects
  them with `-EPERM` (`result=-1`) for a client that only has `allow r`.
  Create the identity with the minimum caps that let the driver work:
  ```bash
  ceph auth get-or-create client.firmware01 \
    mon 'allow r' \
    osd 'allow r class-read' \
    pool rbd 'allow r class-read'
  ```
  with `class-read` granted on the pool that holds the image when you use
  per-pool caps.

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
```

`MON_GET_MAP` is answered with a **MonMap** (`CEPH_MSG_MON_MAP=4`), not an
OSDMap; the probe uses a separate `osd getmap` MON command for the OSDMap.
Without the CephX step the monitor drops the request with `is not
authenticated`, which is how the need for the handshake was isolated. The
`dxe_discover`/`dxe_read` lines confirm the full RBD path — metadata class
methods plus a data-block read over the primary OSD session — works against a
real cluster. If `dxe_discover` fails with `status=-13` and the class-method
`result=-1`, the identity is missing the `class-read` cap (see B.3).

---

## Choosing an approach

- **Need a bootable VM now?** Use Path A. It is the standard, supported way and
  is already running in this environment.
- **Developing or testing this repository's firmware driver?** Use Path B.
  Track progress in `IMPLEMENTATION_STATUS.md`; the guide above documents the
  intended configuration so the pieces can be validated incrementally.
- **Never** put the development `client.admin` key into a guest image, a
  firmware binary, or a public repository. Create a dedicated identity with
  the minimum caps for each VM.
