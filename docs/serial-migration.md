# Serial migration for the SMK20-EU

This procedure was used on a Smarter iKettle 2.0 marked **SMK20-EU** with an Ai-Thinker ESP8266MOD in an ESP-12E/F-style footprint, 4 MiB of ESP flash, and kettle-controller firmware 19. Confirm the model, module orientation, flash size and circuit continuity on your own board before proceeding. Other revisions and models need their own verification.

ESPHome replaces the software on the ESP8266 Wi-Fi module. The separate kettle controller still handles the heater, sensing and physical button. The one-time serial procedure isolates the ESP's UART receive trace while programming and restores the original connection afterwards.

## Safety and tools

**The base contains mains circuitry.** Unplug it from the wall before opening it and keep the mains plug disconnected throughout the work. Stored charge can remain after unplugging. Only continue once you can verify that the open base is safe to handle and the logic board is separated from the mains power supply and any backfeed path. Have a qualified repairer check the board if the power arrangement is uncertain. Reconnect mains only after removing all bench leads and fully reassembling the base.

Use a multimeter, magnification, fine soldering tools, an insulated jumper, a USB UART adapter with verified **3.3 V logic**, and a stable, isolated regulated 3.3 V bench supply. The ESP8266 can draw short current peaks, so use a supply rated for at least 500 mA for the module and verify the load on your isolated board. An adapter's `3V3` supply pin does not establish its UART signal voltage or its ability to power the board. Use short leads and a common logic ground. Keep the adapter's VCC pin disconnected when using a separate supply. [Espressif serial connection](https://docs.espressif.com/projects/esptool/en/latest/esp8266/esptool/serial-connection.html) · [Espressif power guidance](https://docs.espressif.com/projects/esp-techpedia/en/latest/esp8266/esp-friends/get-started/try-firmware/try-firmware-hardware/esp8266.html)

## Identify the module pads and isolate RX

Look down at the **metal shield on the populated side** with the printed antenna pointing **left**. Count each eight-pad row from the antenna towards the right. The six short-edge pads are on the right. The `U` and `L` labels below are orientation labels for this guide. Ai-Thinker's underside diagrams are mirrored relative to this view. [Ai-Thinker module specification](https://docs.ai-thinker.com/_media/esp8266/docs/esp-12f_product_specification_en.pdf)

```text
                   TOP VIEW   metal shield facing you

Upper row         U1    U2    U3    U4    U5    U6    U7    U8
                  TXD0  RXD0  IO5   IO4   IO0   IO2   IO15 GND
                 +------------------------------------------+  .
 PCB antenna <== |               metal shield               |  .  six
     left        |                ESP8266MOD                |  .  edge
                 +------------------------------------------+  .  pads
                  RST   ADC   EN    IO16  IO14  IO12  IO13 3V3
Lower row         L1    L2    L3    L4    L5    L6    L7    L8
```

With all sources disconnected, identify the PCB test point marked `B2` and check direct continuity to `U2` (`RXD0`). On the tested board, `B2` was on the kettle controller's transmit path to ESP RX. The pad marked `B4` had continuity to `U1` (`TXD0`) and its trace was left intact. These labels identify test points, independently of any header pin numbering.

Follow the copper between `U2` and `B2`. Make one narrow cut in that trace so the ESP module's `U2` remains accessible and `B2` remains on the controller side. Recheck that `U2` and `B2` are no longer directly continuous and inspect adjacent traces and pads for damage. If the visible routing or meter readings differ, stop and identify the board before cutting. The two wires below serve different purposes and are fitted at different stages.

```text
Before             ESP U2/RXD0 ------- copper trace ------- B2 / controller TX
While flashing     ESP U2/RXD0 ------- USB UART TX          B2 / isolated
                   ESP U5/GPIO0 ------ temporary strap ---- U8/GND
After flashing     ESP U2/RXD0 ------- insulated jumper ---- B2 / controller TX
                   ESP U5/GPIO0        strap removed
```

## Wire the serial bootloader

| Source | Destination | Purpose |
| --- | --- | --- |
| USB UART TX | `U2` | Drive ESP RX on the module side of the cut |
| USB UART RX | `U1` | Read ESP TX |
| USB UART ground and supply ground | `U8` | Common logic reference |
| Regulated 3.3 V | `L8` | Power the verified, isolated logic circuit |
| Temporary ground strap | `U5` to `U8` | Select the ROM serial bootloader at reset |

Fit the temporary `GPIO0` strap before applying low-voltage power. Leave the other boot straps in their original state. These commands use manual boot selection, so leave the adapter's DTR and RTS leads disconnected. [Espressif boot mode selection](https://docs.espressif.com/projects/esptool/en/latest/esp8266/advanced-topics/boot-mode-selection.html)

## Identify and back up your own flash

The commands below use esptool 5 and the example Windows port `COM7`. Substitute your port, such as `/dev/ttyUSB0` on Linux. Close other programs that have the port open. If you need to install esptool separately, use a Python virtual environment and run `python -m pip install "esptool>=5,<6"`.

```shell
python -m esptool --chip esp8266 --port COM7 --baud 115200 --before no-reset --after no-reset flash-id
```

The tested board had 4 MiB. For a confirmed 4 MiB device, read all `0x400000` bytes from address `0x0` into two different files. Start each read from a fresh ROM boot, with the strap still fitted. Disconnect low-voltage power before changing leads and avoid backfeeding the unpowered board from the UART adapter. For a different or ambiguous capacity, stop and establish the correct model and layout first.

```shell
python -m esptool --chip esp8266 --port COM7 --baud 115200 --before no-reset --after no-reset read-flash 0x0 0x400000 stock-a.bin
python -m esptool --chip esp8266 --port COM7 --baud 115200 --before no-reset --after no-reset read-flash 0x0 0x400000 stock-b.bin
```

Each file must be 4,194,304 bytes and the two SHA-256 values must match. The same check works in PowerShell, Command Prompt or a Unix shell when `python` points at your environment.

```shell
python -c "from pathlib import Path; from hashlib import sha256; a,b=(Path(n).read_bytes() for n in ('stock-a.bin','stock-b.bin')); assert len(a)==len(b)==0x400000 and a==b, 'Incomplete or inconsistent backup'; print(sha256(a).hexdigest())"
```

Record the size and hash, then keep the backups in private storage. A full flash read can include Wi-Fi credentials and device-specific data. The two backups cover the ESP8266 flash. The separate kettle controller is outside that address space. [Espressif flash commands](https://docs.espressif.com/projects/esptool/en/latest/esp8266/esptool/basic-commands.html)

## Build, write and reconnect

1. Configure the example with your own `secrets.yaml` and build it as described in the [README](../README.md). Use the **factory** binary from the ESPHome build, typically `firmware.factory.bin`, for this first serial write. Keep the binary private because it contains your settings.
2. Inspect its header with the command below. The tested profile built for 4 MiB at 40 MHz. Use `keep` so esptool preserves the factory image's own flash mode, size and frequency.
3. With the ESP back in ROM mode and the RX trace still open, write the factory image to `0x0`. Wait for esptool to report successful verification. Keep the verified original backups available.

```shell
python -m esptool --chip esp8266 image-info firmware.factory.bin
python -m esptool --chip esp8266 --port COM7 --baud 115200 --before no-reset --after no-reset write-flash --flash-mode keep --flash-size keep --flash-freq keep 0x0 firmware.factory.bin
```

4. Disconnect all power and the UART adapter. Remove the `U5` to `U8` boot strap. Restore the cut with a short, insulated jumper from **`U2` to `B2`**. Verify direct continuity across the jumper and check that adjacent pads remain separate. This reconnects the kettle controller to ESP RX for normal operation.
5. Remove bench wiring and tools, restore any verified original power connectors and reassemble the base. Check that the jumper is secured and clear of mains circuitry, enclosure edges and moving parts. Only then reconnect mains. Verify that the physical button, idle status, temperature and on-base sensor behave normally before attempting a supervised heat cycle with water.

If the ROM connection fails, confirm the 3.3 V supply under load, boot strap, crossed TX/RX, ground and correct serial port. Recheck the cut and adjacent pads with power disconnected. Intermittent reads or mismatched hashes are a reason to fix the hardware connection before writing. [Espressif troubleshooting](https://docs.espressif.com/projects/esptool/en/latest/esp8266/troubleshooting.html)

## Restore the original ESP firmware

Use the backup from that exact module. Reopen only after unplugging and making the base safe. Disconnect one end of the `U2` to `B2` jumper to isolate ESP RX again, fit the temporary `GPIO0` strap and enter ROM mode with the same isolated supply and UART wiring. Check the stored size and hash before writing. For the confirmed 4 MiB layout, write your entire saved flash image from `0x0` while preserving its own header. This overwrites the installed ESPHome image and restores the original ESP flash contents.

```shell
python -m esptool --chip esp8266 --port COM7 --baud 115200 --before no-reset --after no-reset write-flash --flash-mode keep --flash-size keep --flash-freq keep 0x0 stock-a.bin
```

Start another fresh ROM boot and read the full flash back to a third file with the same read command. Check the result against the original backup byte for byte. Disconnect power, remove the temporary boot strap, reconnect the `U2` to `B2` jumper and reassemble before returning to mains. The full serial restore procedure follows the flash layout and verified backup. A stock rollback on the installed kettle has not been exercised.

### Why the Espressif download is not a complete backup

On the tested unit, `bin/at/user1.1024.new.2.bin` from Espressif's NONOS SDK 1.3.0 (`esp_iot_sdk_v1.3.0_15_08_08.zip`) matched the 262,948 bytes at `0x1000` exactly. Its SHA-256 is `84222baa92782ede25d6093bcad88243c43fd98c112ab2fd4c9ad2112d89c068`. The SDK's `boot_v1.4(b1).bin` differed from the installed bootloader by one flash header byte. The full dump also had data in other sectors, including the last three sectors used for ESP8266 system parameters. [Espressif ESP8266EX resources](https://www.espressif.com/en/products/socs/esp8266ex/resources/)

A generic SDK-based AT installation booted on a spare ESP8266. It has not been verified as a substitute for a complete stock restore on an assembled kettle. Read and retain your own complete backup before installing ESPHome. The separate kettle-controller update image from the Smarter app cannot restore the ESP8266. The repository provides ESPHome source to build with your own settings. It does not publish device dumps or third-party firmware binaries.

### Stock OTA experiment

The stock `AT+CIUPDATE` investigation used a spare ESP8266. A custom bridge successfully migrated that module after a serial AT command. The experiment did not establish an equivalent way to start migration over Wi-Fi on an assembled kettle. The original updater's slot image format and ESPHome's factory image format differ. The serial procedure above is the supported first install and stock recovery route. Once ESPHome is installed, use its [encrypted OTA and recovery options](operations.md#updates-and-recovery).
