#!/usr/bin/env python3
"""Build the fixed-size CEPH_RBD_CONFIG blob for the firmware UEFI variable.

Usage: make-ceph-rbd-config.py CONFIG.ini OUTPUT.bin

The binary layout must stay in sync with uefi-edk2/CephRbdPkg/CephRbdDxe/
CephRbdConfig.h. All integers are little-endian and the blob is 372 bytes.
Two key forms are supported:
  * "key-source" stores an opaque reference that a platform's protected
    credential provider resolves at runtime (sets KEY_REFERENCE flag).
  * "key-hex" stores a raw 16-byte AES-128 secret for drivers that consume
    key material directly (clears KEY_REFERENCE flag).
"""

import configparser
import struct
import sys
import uuid

SIGNATURE = 0x52424443
VERSION = 1
MAX_MONITORS = 4
MAX_IMAGE = 96
MAX_IDENTITY = 64
MAX_KEY = 128
BLOB_SIZE = 372


def encode(config_path):
    with open(config_path, encoding="utf-8") as source:
        lines = source.readlines()
    # The example file wraps the INI block in markdown fences; parse only
    # that block and ignore surrounding documentation.
    start = next(
        (i + 1 for i, line in enumerate(lines)
         if line.lstrip().startswith("```ini")),
        0,
    )
    end = next(
        (i for i in range(start, len(lines))
         if lines[i].lstrip().startswith("```")),
        len(lines),
    )
    text = "".join(lines[start:end])
    parser = configparser.ConfigParser()
    parser.read_string(text)
    if "ceph" not in parser:
        raise SystemExit("missing [ceph] section in " + config_path)
    section = parser["ceph"]

    fsid = uuid.UUID(section["fsid"]).bytes
    monitors = []
    for entry in section["monitors"].split(","):
        address, port = entry.strip().rsplit(":", 1)
        octets = tuple(int(part) for part in address.split("."))
        if len(octets) != 4 or any(part < 0 or part > 255 for part in octets):
            raise ValueError("invalid monitor address: " + address)
        monitors.append((bytes(octets), int(port), 0))
    if not monitors or len(monitors) > MAX_MONITORS:
        raise ValueError("monitor count must be 1..%d" % MAX_MONITORS)

    image = section["image"].encode("utf-8")
    identity = section["client-id"].encode("utf-8")
    flags = int(section.get("flags", "0"), 0)
    key_hex = section.get("key-hex", "").strip()
    key_source = section.get("key-source", "").strip()
    if bool(key_hex) == bool(key_source):
        raise ValueError("exactly one of key-hex or key-source is required")
    if key_hex:
        try:
            key_blob = bytes.fromhex(key_hex)
        except ValueError:
            raise ValueError("key-hex is not valid hexadecimal")
        if len(key_blob) != 16:
            raise ValueError("key-hex must be exactly 16 bytes (AES-128)")
        # Raw key bytes are consumed directly by the driver.
        flags &= ~0x1
    else:
        key_blob = key_source.encode("utf-8")
        if not key_blob or len(key_blob) > MAX_KEY:
            raise ValueError("key-source is empty or too long")
        # An opaque reference needs the platform secret resolver.
        flags |= 0x1
    if not image or len(image) >= MAX_IMAGE:
        raise ValueError("image name is empty or too long")
    if not identity or len(identity) >= MAX_IDENTITY:
        raise ValueError("client-id is empty or too long")

    fields = [
        SIGNATURE,
        VERSION,
        BLOB_SIZE,
        flags,
        fsid,
        int(section["pool"]),
        len(monitors),
    ]
    for monitor in monitors:
        fields.extend(monitor)
    # The wire layout always carries MAX_MONITORS slots; unused ones are zero.
    for _ in range(MAX_MONITORS - len(monitors)):
        fields.extend((b"\x00\x00\x00\x00", 0, 0))
    fields.extend([
        len(image),
        image,
        len(identity),
        identity,
        len(key_blob),
        key_blob,
    ])

    layout = "<4I16s2I"
    layout += "4s2H" * MAX_MONITORS
    layout += "I%dsI%dsI%ds" % (MAX_IMAGE, MAX_IDENTITY, MAX_KEY)
    blob = struct.pack(layout, *fields)
    assert len(blob) == BLOB_SIZE, len(blob)
    return blob


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: make-ceph-rbd-config.py CONFIG.ini OUTPUT.bin")
    with open(sys.argv[2], "wb") as output:
        output.write(encode(sys.argv[1]))


if __name__ == "__main__":
    main()
