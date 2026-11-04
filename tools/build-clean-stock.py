#!/usr/bin/env python3
"""Build a clean 4 MiB stock ESP-AT recovery ZIP from the Espressif SDK archive."""

import argparse
from hashlib import sha256
from io import BytesIO
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


FLASH_BYTES = 0x400000
IMAGE_NAME = "stock-at40-sdk130-4mib.bin"
LICENSE_NAME = "ESPRESSIF-LICENSE.txt"
SDK_SHA256 = "24e470243f29cdcab35744f4c0c2dab4affe825250be8b777402c68c3433505c"
LICENSE_SHA256 = "61f328e0bf7f7746aaf3bf70db64a39684e66363ff731a01d74c7642a30d279e"
SDK_ROOT = "esp_iot_sdk_v1.3.0/bin/"
PARTS = (
    (0x000000, "boot_v1.4(b1).bin", "8ba27abd6c92af0a454fd73b895ed9c0d6e6a26f0ea6b53cf191aa8932ba75fb"),
    (0x001000, "at/user1.1024.new.2.bin", "84222baa92782ede25d6093bcad88243c43fd98c112ab2fd4c9ad2112d89c068"),
    (0x3FC000, "esp_init_data_default.bin", "4c90b1ededc6d859c13bc67cbbd1484cd033f709751ee479d470843446c9bab4"),
)


def verified(data, expected, label):
    if sha256(data).hexdigest() != expected:
        raise ValueError(f"SHA-256 mismatch: {label}")
    return data


def build(sdk_path):
    sdk_bytes = verified(sdk_path.read_bytes(), SDK_SHA256, "Espressif SDK archive")
    flash = bytearray(b"\xff" * FLASH_BYTES)
    with ZipFile(BytesIO(sdk_bytes)) as sdk:
        license_data = verified(sdk.read("License"), LICENSE_SHA256, "Espressif License")
        for offset, member, digest in PARTS:
            data = verified(sdk.read(SDK_ROOT + member), digest, member)
            flash[offset:offset + len(data)] = data

    # ESP8266 boot image header: DIO, 4 MiB with 512 KiB OTA slots, 40 MHz.
    # The archive and the boot image digest above pin the unmodified header.
    flash[2:4] = b"\x02\x40"
    return bytes(flash), license_data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True, help="Original Espressif NONOS SDK 1.3.0 ZIP")
    parser.add_argument("--output", type=Path, required=True, help="Path to a new recovery ZIP")
    args = parser.parse_args()
    image, license_data = build(args.sdk)
    files = {IMAGE_NAME: image, LICENSE_NAME: license_data}
    checksums = "".join(f"{sha256(data).hexdigest()}  {name}\n" for name, data in files.items())
    files["SHA256SUMS"] = checksums.encode("ascii")
    with ZipFile(args.output, "x") as output:
        for name, data in files.items():
            output.writestr(ZipInfo(name), data, compress_type=ZIP_DEFLATED, compresslevel=9)
    print(checksums, end="")


if __name__ == "__main__":
    main()
