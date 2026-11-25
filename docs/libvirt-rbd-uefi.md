# Boot a libvirt VM from a Ceph RBD image with UEFI

This is the supported **host-side** path for booting a libvirt/QEMU virtual
machine from RBD today. QEMU opens the RBD image with its native `librbd`
backend and presents it as a virtual disk; OVMF or the guest bootloader sees
that disk, not a Ceph network device. It does not depend on the unfinished
Ceph client in `uefi-edk2/`.

The domain and secret examples in `config/` use illustrative monitor addresses,
pool/image names, and a fixed example secret UUID. Replace them with values for
your cluster. The RBD image must already contain a UEFI-bootable disk layout
(normally a GPT EFI System Partition and an EFI bootloader), and the libvirt
host must have QEMU's RBD support installed and network access to the monitors
and OSDs.

## Define a Ceph secret

Create a least-privilege CephX identity for the VM, with access appropriate for
the image and read-only boot use. Do not put its key in the domain XML or the
guest configuration.

On the libvirt host, define the secret descriptor:

```sh
virsh secret-define --file config/libvirt-ceph-secret.xml
```

Install the base64 Ceph key using a root-only key file supplied by your secret
management system. `virsh secret-set-value --file` reads and decodes that
base64 value; avoid `--base64 <key>` because command-line arguments may be
visible to other host processes.

```sh
chmod 600 /run/credentials/libvirt-ceph.key
virsh secret-set-value \
  --secret aef7c9a8-5b3e-4e16-9bd1-f5c7e28f4c10 \
  --file /run/credentials/libvirt-ceph.key
```

Ensure that the secret is available to the libvirt QEMU driver and remove the
temporary key file according to your host's secret-handling policy after
provisioning. Libvirt retains the secret for QEMU; the VM does not receive the
Ceph key.

## Configure and start the VM

Edit `config/libvirt-rbd-uefi.xml` for the target pool/image, monitor endpoints,
secret UUID, OVMF firmware paths, VM sizing, and guest network. The example uses
the common OVMF 4 MiB firmware paths on Debian/Ubuntu and monitor v2 port 3300;
use the paths and ports configured on your host and cluster.

```sh
virt-xml-validate config/libvirt-rbd-uefi.xml domain
virsh define config/libvirt-rbd-uefi.xml
virsh start ceph-rbd-uefi-example
```

The disk is marked read-only and has boot order 1, so it is suitable for
validation against an image or snapshot without guest writes. Remove
`<readonly/>` only when the image is deliberately writable and is not being
used concurrently by another guest. Use a separate RBD clone per VM for
ordinary non-cluster-aware filesystems.

OVMF variable storage is per-domain and writable; libvirt creates the NVRAM
file from the specified template. If the image has no valid UEFI boot entry,
use the OVMF boot menu to select its EFI loader or repair the guest image's
boot entries. GRUB is not required on the host: a GRUB EFI executable already
installed in the RBD image is launched by OVMF like any other EFI application.

## Scope boundary

This path provides direct RBD-backed virtual-disk boot for libvirt without
claiming that the guest firmware implements Ceph. The repository's native
UEFI/BIOS goal—firmware connecting to Ceph and publishing/reading RBD itself—is
still incomplete; see `uefi-edk2/IMPLEMENTATION_STATUS.md`.
