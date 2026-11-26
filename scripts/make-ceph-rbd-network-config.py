#!/usr/bin/env python3
"""Build the fixed-size CephRbdNetworkConfig blob for the firmware UEFI variable.

Usage: make-ceph-rbd-network-config.py MODE OUTPUT.bin [--station IP] \
       [--mask MASK] [--gateway IP] [--dns IP]

Layout (all little-endian, 36 bytes) must stay in sync with
uefi-edk2/CephRbdPkg/CephRbdDxe/CephRbdNetwork.h:

    u32 signature   'CRBN' = 0x4352424E
    u32 version     1
    u32 length      48
    u32 mode        0 = DHCP (platform default), 1 = static
    u32 flags       bit0 = gateway address valid
    u8[4] station address
    u8[4] subnet mask
    u8[4] gateway address
    u8[4] DNS address

DHCP mode ignores the address fields; static mode requires --station and
--mask. The generated blob is written with `setvar` under the variable name
CephRbdNetworkConfig and the GUID A7E4C1B2-3D5F-4E68-9B12-770A44632EF1.
"""

import argparse
import ipaddress
import struct
import sys

SIGNATURE = 0x4352424E
VERSION = 1
MODE_DHCP = 0
MODE_STATIC = 1
FLAG_GATEWAY = 0x1
BLOB_SIZE = 36


def parse_ip(value):
    if not value:
        return bytes(4)
    try:
        return ipaddress.IPv4Address(value).packed
    except ipaddress.AddressValueError:
        raise SystemExit("invalid IPv4 address: " + value)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=["dhcp", "static"])
    parser.add_argument("output")
    parser.add_argument("--station")
    parser.add_argument("--mask")
    parser.add_argument("--gateway")
    parser.add_argument("--dns")
    args = parser.parse_args()

    if args.mode == "static":
        if not args.station or not args.mask:
            raise SystemExit("static mode requires --station and --mask")
        mode = MODE_STATIC
        station = parse_ip(args.station)
        mask = parse_ip(args.mask)
        gateway = parse_ip(args.gateway)
        dns = parse_ip(args.dns)
        flags = FLAG_GATEWAY if args.gateway else 0
    else:
        mode = MODE_DHCP
        station = mask = gateway = dns = bytes(4)
        flags = 0

    blob = struct.pack(
        "<5I4s4s4s4s",
        SIGNATURE, VERSION, BLOB_SIZE, mode, flags,
        station, mask, gateway, dns,
    )
    assert len(blob) == BLOB_SIZE, len(blob)
    with open(args.output, "wb") as output:
        output.write(blob)
    print("wrote %s (%d bytes, mode=%s)" % (args.output, len(blob), args.mode))


if __name__ == "__main__":
    main()
