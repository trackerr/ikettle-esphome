# iKettle 2.0 for ESPHome

Run ESPHome on the existing ESP8266 Wi-Fi module of a Smarter iKettle 2.0. The original kettle controller still handles the heater, sensors and physical button. ESPHome replaces the Wi-Fi firmware and talks to the controller over its existing UART.

This route was demonstrated on one **SMK20-EU** base with an ESP8266EX, 4 MiB of flash and kettle MCU firmware **19**. Confirm your hardware before proceeding. The first installation requires opening the base, isolating one UART trace, reading a complete backup and flashing over serial. After the trace is repaired, later ESPHome updates can use encrypted OTA.

| Guide | What it covers |
| --- | --- |
| [Serial migration and stock recovery](docs/serial-migration.md) | Check the board, isolate ESP RX, back up the original ESP flash twice, install ESPHome and repair the UART link. |
| [Clean stock ESP-AT recovery](docs/clean-stock-recovery.md) | Download or build a clean 4 MiB recovery image for matching ESP8266 hardware when an original backup is unavailable. |
| [Configuration and operation](docs/operations.md) | Join Wi-Fi, add the kettle to Home Assistant, use the controls and diagnose communication problems. |
| [Water calibration](docs/water-calibration.md) | Measure your own kettle before enabling mL, cups and the low-water indicator. |
| [Home Assistant examples](home-assistant/README.md) | Dashboard card and drinking-temperature audio automation. |

## What ESPHome exposes

- Current water temperature, on-base state, activity and a separate command status. A successful send is shown before the MCU confirms heating or stopping.
- Target temperature and keep-warm duration, saved on the ESP. Green tea, black tea and filter coffee presets select a target. Use the separate start button when ready.
- Heat to a target, boil to 100 °C and cool to a target, stop the cycle or stop keep-warm. The ESP emits one event when an observed cycle cools to the configured drinking temperature. Home Assistant can play the included chime on a compatible speaker.
- Water volume and a low-water indicator after calibration. The last observed cycle reports starting temperature, heating duration, keep-warm duration and an estimated heating energy based on the configured wattage.
- Wi-Fi signal, firmware versions, reset reason, age of the last MCU reply and receive errors. The optional UART trace redacts credentials and turns itself off.

## Build and test

Use Python 3.13 and the pinned ESPHome version. Copy `secrets.yaml.example` to `secrets.yaml` and supply your own Wi-Fi credentials, recovery password and generated API encryption key. Keep that file and the firmware built with it private. Set the example wattage in `ikettle-smk20-eu.yaml` to your kettle's rating if you use the energy estimate. The water volume package is disabled until you provide calibration points.

```sh
python -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements-build.txt
esphome config ikettle-smk20-eu.yaml
esphome compile ikettle-smk20-eu.yaml
```

On Windows PowerShell use `.venv\Scripts\Activate.ps1` to activate the environment. Use the [serial migration guide](docs/serial-migration.md) for the first upload and the checks before reconnecting power. For subsequent OTA uploads, use `esphome upload ikettle-smk20-eu.yaml --device <device-ip>` after compiling. The API key also authenticates native OTA. A WPA-protected recovery AP and ESPHome safe mode are included.

Run `python tests/run_offline.py` for the native protocol and trace tests plus configuration validation. The native tests require a C++17 compiler. On Windows, install the Microsoft C++ Build Tools. The tests use simulated UART data and do not open a serial port or contact a kettle.

The ESPHome migration was tested on the hardware above. The test suite also exercises command confirmation, cooling events and cycle tracking offline. The repository contains source and examples. Build your own ESPHome image and retain your own stock backup.

For a stock iKettle with no hardware modifications, see the [iKettle2mqtt fork and local pairing tool](https://github.com/trackerr/hassio-addon-ikettle2mqtt).

The adapter source, documentation and chime are available under the [MIT license](LICENSE). The build pulls in ESPHome and its separately licensed runtime. This project is independent of Smarter and ESPHome.
