# Boot Linux directly from a Ceph RBD image

This repository provides a practical PXE/iPXE boot design and an initramfs-tools prototype for Linux systems whose root filesystem lives on Ceph RBD.

## Key design decision

UEFI and legacy BIOS firmware do not natively speak Ceph RBD. Rather than implementing a Ceph client in firmware, use firmware only to load iPXE, then fetch the Linux kernel and initramfs over HTTP(S). The initramfs brings up networking, maps the RBD image with the Linux `rbd` client, and mounts the root partition. This works with both UEFI and legacy BIOS PXE and keeps Ceph protocol/authentication in Linux.

```text
UEFI PXE / BIOS PXE
        |
        v
      iPXE  -- DHCP --> boot configuration
        |                  |
        +-- HTTPS: kernel + initramfs
                           |
                           v
       initramfs: NIC + IP + Ceph config/key + rbd kernel module
                           |
                           v
          Ceph monitors (public network) --> RBD mapping
                           |
                           v
                 /dev/rbd0pN --> switch_root
```

An alternative is to export RBD through a dedicated iSCSI gateway and use iPXE `sanboot`. That makes the firmware stage see an iSCSI disk, not a native Ceph device; it adds a gateway and does not remove the need to plan multipath, authentication, and image ownership. The initramfs approach is the recommended starting point.

## Boot flow

1. Configure DHCP/PXE to load an architecture-appropriate iPXE binary (UEFI x86-64 and BIOS require different binaries). For UEFI Secure Boot, use a trusted signed shim/GRUB/iPXE chain; an unsigned custom iPXE binary will not boot under Secure Boot.
2. iPXE downloads the kernel and initramfs from an HTTP(S) boot server.
3. The kernel command line uses `ip=dhcp` and identifies the image with `ceph_rbd=pool/image`. Do not put Ceph secrets in the command line.
4. The initramfs configures the NIC and IP, loads the `rbd` module, and runs `scripts/local-top/ceph-rbd` (this prototype). `rbd map` creates `/dev/rbd0` and the kernel exposes its partition(s).
5. The normal initramfs root-mount flow mounts the requested `root=` device and switches to the real root.

## Prototype

`scripts/local-top/ceph-rbd` is an initramfs-tools `local-top` hook. It expects these command-line arguments:

- `ceph_rbd=pool/image` (required)
- `ceph_id=boot` (optional; defaults to `boot`)
- `ceph_conf=/etc/ceph/ceph.conf` (optional)
- `ceph_keyring=/etc/ceph/ceph.client.boot.keyring` (optional; defaults based on `ceph_id`)
- `ceph_rbd_device=/dev/rbd0` (optional; expected mapping path)

Example iPXE script (adjust URLs, paths, and root partition):

```ipxe
#!ipxe
dhcp
kernel https://boot.example/vmlinuz ip=dhcp root=/dev/rbd0p1 ro ceph_rbd=images/node01 ceph_id=boot ceph_conf=/etc/ceph/ceph.conf ceph_keyring=/etc/ceph/ceph.client.boot.keyring
initrd https://boot.example/initrd.img
boot
```

The initramfs must contain the hook, `rbd` and its dependencies, the `rbd` userspace utility, Ceph configuration/keyring, NIC firmware/modules, and the tools needed to bring up the network. The network must be usable before `local-top` runs. On Debian/Ubuntu with initramfs-tools, install the script into `/etc/initramfs-tools/scripts/local-top/ceph-rbd`, make it executable, include the required files/modules with initramfs-tools configuration, and rebuild the initramfs (for example, `update-initramfs -u`). Exact packaging steps vary by distribution and Ceph release.

The prototype assumes a single RBD mapping in the initramfs so that the requested device is predictable. Ensure `root=` names the correct filesystem partition (for example `/dev/rbd0p1`), or use `/dev/rbd0` for a filesystem directly on the image. Verify partition naming and discovery on the target kernel before rollout.

## Ceph and security requirements

- The boot host must reach the Ceph monitors and OSD public network. Ensure DHCP provides the correct interface configuration, routes, and DNS if monitor names are used.
- Provide a Ceph config with monitor addresses and a narrowly scoped CephX identity. Grant only the required pool/image permissions; avoid an administrative key.
- This prototype stores the keyring in the initramfs. Treat the initramfs as a secret-bearing artifact, restrict its distribution, and use per-host or short-lived credentials where possible. For stronger isolation, fetch per-host credentials over authenticated HTTPS from a provisioning service.
- Protect kernel/initramfs delivery with HTTPS and/or a verified Secure Boot chain. Secure Boot verifies the boot chain; it does not by itself encrypt the embedded keyring.
- Do not let multiple hosts mount the same writable filesystem image unless the filesystem and storage design explicitly support it. Prefer one clone per host (often from a common snapshot); use read-only shared images only for suitable workloads.
- Test failure handling, monitor changes, image resize/partition behavior, and reconnects. Production deployments should define an upgrade/rollback scheme for kernel, initramfs, and image snapshots.

## Validation checklist

1. Test in a VM first with the same firmware mode, NIC model, kernel, and initramfs.
2. Confirm iPXE can fetch both artifacts and that the initramfs has working networking before the RBD hook runs.
3. Confirm `rbd map` succeeds with the boot identity and that the expected `/dev/rbd0[pN]` appears.
4. Confirm the filesystem mounts read-only for the first boot; then test the intended writable/clone strategy.
5. Test both BIOS PXE and UEFI PXE independently. Firmware PXE binaries and Secure Boot behavior differ.

This is a bootstrap prototype, not a drop-in distribution image builder. Production use requires distro-specific initramfs integration and testing against the exact Ceph/kernel versions.
