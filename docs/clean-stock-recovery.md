# Clean stock recovery for the ESP8266

Use your own verified [full flash backup](serial-migration.md#restore-the-original-esp-firmware) to restore the original bytes and settings. The clean image is a fallback when that backup is unavailable. It rebuilds a generic Espressif AT 0.40.0.0 installation from NONOS SDK 1.3.0 for an **ESP8266 with 4 MiB of flash**, the 512 KiB slot layout, **DIO** and **40 MHz**. It replaces the entire ESP flash, including saved network settings. The separate STM32 kettle controller and its stored settings are outside that flash.

The AT application matches the version observed on the tested SMK20-EU. The clean image uses DIO, while that kettle's saved stock boot header used QIO. Treat the image as a recovery option for confirmed matching hardware. Rollback with this clean image has not been verified on an assembled kettle. [Espressif flash modes](https://docs.espressif.com/projects/esptool/en/latest/esp8266/esptool/flash-modes.html)

## Obtain and verify the image

Download the [recovery ZIP](../recovery/stock-at40-sdk130-4mib.zip?raw=1) or generate an equivalent package locally from the [original Espressif SDK archive](https://bbs.espressif.com/download/file.php?id=664).

```shell
python tools/build-clean-stock.py --sdk PATH_TO_SDK_ZIP --output stock-at40-sdk130-4mib.zip
```

Extract `stock-at40-sdk130-4mib.bin` and verify its SHA-256 against the package's `SHA256SUMS` file. It must be exactly 4,194,304 bytes. The ZIP includes Espressif's notice and license for use on Espressif ESP8266 hardware. The image contains the stock bootloader, AT application in slot 1 and SDK initialization data. The second slot is empty. The build uses SDK files and erased-byte padding, so it carries no saved settings from any kettle.

```shell
python -c "from hashlib import sha256; from pathlib import Path; data = Path('stock-at40-sdk130-4mib.bin').read_bytes(); print(len(data), sha256(data).hexdigest())"
```

## Restore and reconnect

Follow the [serial wiring and safety steps](serial-migration.md) with the base unplugged and the logic circuit isolated. Use a regulated 3.3 V supply and 3.3 V UART. Isolate `U2` from `B2` and strap `GPIO0` low to enter the ESP ROM bootloader. Back up the current flash twice and check that the copies match before writing, even if the current firmware is ESPHome. Replace `COM7` with the port for the ESP you intend to restore.

```shell
python -m esptool --chip esp8266 --port COM7 --baud 115200 --before no-reset --after no-reset flash-id
python -m esptool --chip esp8266 image-info stock-at40-sdk130-4mib.bin
```

Proceed only after confirming the ESP8266 and its 4 MiB flash, the target's compatibility with the image's DIO and 40 MHz settings, and the file's checksum. The write covers all 4 MiB starting at `0x0`.

```shell
python -m esptool --chip esp8266 --port COM7 --baud 115200 --before no-reset --after no-reset write-flash --flash-mode keep --flash-size keep --flash-freq keep 0x0 stock-at40-sdk130-4mib.bin
```

Wait for verification to complete. For an optional bench check, remove the `GPIO0` strap with power off, then boot again with isolated 3.3 V and `U2` still isolated from `B2`. Open a serial terminal at 115200 baud, 8N1, with no flow control and send `AT+GMR` with CRLF line endings. The expected output reports AT 0.40.0.0 and SDK 1.3.0.

Disconnect power and the UART adapter, remove the `GPIO0` strap if it is still fitted, reconnect `U2` to `B2`, and fully reassemble the base before connecting mains. Once it boots, check whether the original kettle controller reconnects the module using its own saved settings. If the kettle needs to join Wi-Fi again, follow the [local pairing guide](https://github.com/trackerr/hassio-addon-ikettle2mqtt/blob/main/tools/wifi-setup/README.md). After joining, use the MQTT add-on for Home Assistant control with stock firmware.

## Bench verification

The published image was flashed to a separate 4 MiB ESP8266EX. Esptool verified the complete 4 MiB against the binary. It booted into AT 0.40.0.0 with SDK 1.3.0 and reported no station association. The module scanned for nearby Wi-Fi networks and a separate computer detected its temporary test access point with the matching BSSID. The temporary settings were gone after reset. These checks cover the ESP firmware and radio. Testing an assembled kettle would also require the original controller and its UART link.
