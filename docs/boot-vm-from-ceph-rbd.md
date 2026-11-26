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


## Firmware-side boot from RBD (this project, in progress)

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

The following are **still missing**, so this path cannot boot a VM yet:

- UEFI Boot Manager integration (handing the discovered ESP/loader to the boot
  manager).
- An actual EDK II / OVMF build of the driver, and end-to-end boot validation.
- DHCP / static network configuration inside the guest firmware.

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
incrementally, and B.5 shows the live-protocol validation that is already
passing against a real cluster.

**Never** put the development `client.admin` key into a guest image, a firmware
binary, or a public repository. Create a dedicated identity with the minimum
caps (`allow r class-read`) for each VM.
