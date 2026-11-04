# Operating and maintaining the kettle

## Connect Home Assistant

After the [serial installation](serial-migration.md), connect the reassembled base to power. Look for the iKettle under **Settings → Devices & services** in Home Assistant. If discovery does not find it, add the **ESPHome** integration and enter the kettle's hostname or address from your router. Use the `ikettle_api_encryption_key` from your private `secrets.yaml` when prompted. [Home Assistant ESPHome integration](https://www.home-assistant.io/integrations/esphome/)

With the kettle idle, check **Temperature**, **On base**, and **Kettle communication ready**. Temperature and water values can become unavailable when the jug is lifted or the controller's latest status is stale. **Physical button default** sensors report settings read from the kettle controller. The next-cycle settings in ESPHome are separate.

## Controls

| Control | Behavior |
| --- | --- |
| Next target temperature | Selects 20–100 °C for the next cycle |
| Next keep-warm duration | Selects off at 0 minutes or a duration from 5 to 30 minutes |
| Green tea, black tea and filter coffee | Set the next target to 75, 95 and 92 °C, respectively |
| Start heating | Requests heating to the selected target |
| Boil to 100 and cool to target | Requests a boil followed by cooling to the selected target |
| Stop cycle / Stop keep warm | Request the corresponding stop from the kettle controller |
| Drinking temperature | Sets the cooling notification threshold, 65 °C by default |
| Refresh kettle data | Requests fresh controller information when the link allows it |

Changing a preset, temperature or duration only changes the selection. The example persists the selected temperature and warm time in ESP flash. A new start requires a supported controller, a recent valid on-base idle status and an available UART session. **Command status** distinguishes sending an action from confirmation by a subsequent kettle status. Busy or refused actions require a fresh request. Use the physical controls if the network is unavailable. Follow the kettle's fill marks and stay with the first heating test.

### Drink temperature and chime

The ESP emits `drink_temperature_reached` once per tracked cycle when a fresh temperature sample reaches the selected threshold while cooling. It starts tracking from idle, requires the water to reach at least 2 °C above that threshold, and waits for the heating and keep-warm phases to end. Lifting the jug or losing the status stream cancels tracking. The firmware emits an event that Home Assistant can use for a phone notification or speaker action.

The included optional chime plays through a Home Assistant media player. Copy `home-assistant/media/ikettle-drink-temperature.mp3` into the root of **My media** and install `home-assistant/blueprints/automation/ikettle/drink-temperature-chime.yaml` under the matching blueprint directory in your Home Assistant configuration. Create an automation using that blueprint and select the kettle event and a speaker that supports local media. If your media source is configured under a custom path, use your installation's media picker to obtain the correct media ID. The blueprint example targets Home Assistant 2026.10.4 or newer. [Home Assistant blueprints](https://www.home-assistant.io/docs/automation/using_blueprints/)

### Water and cycle readings

Follow the [calibration procedure](water-calibration.md) to add water volume, cups and the advisory low-water sensor. The raw water sensor remains useful for calibration and diagnosis.

Cycle statistics publish after a cycle that the ESP observed from heating through its end. The duration reflects the sampled phase changes. Set `kettle_heater_watts` to the rated power of your own appliance to use the energy estimate. The example value is illustrative. The estimate multiplies that power by heating duration. It excludes the keep-warm phase and cannot account for relay cycling. Home Assistant can record the published values in its history.

## Diagnostics

Use the **Wi-Fi signal**, **ESP reset reason**, **ESPHome version** and **iKettle ESP firmware version** entities for network and ESP issues. **Time since last MCU reply**, **Status sample age**, **Kettle communication ready** and **Serial errors** describe the controller link. A live ESP with a steadily rising reply age indicates that the ESP is reachable but the controller has stopped sending valid replies. The error count starts from zero at each ESP boot.

**UART trace** is a time-limited, redacted diagnostic stream in the ESPHome logs. The example resets it to off at boot and turns it off automatically after 120 seconds. It exposes bounded protocol details for troubleshooting without printing Wi-Fi credentials. GPIO1 and GPIO3 carry the controller UART, so the example disables serial logger output with `logger.baud_rate: 0`. Use the encrypted network logs for routine diagnosis. [ESPHome UART](https://esphome.io/components/uart/)

## Updates and recovery

With ESPHome already running, build and upload from ESPHome Builder or use the CLI with the same `ikettle-smk20-eu.yaml` and the same API encryption key. In this profile native ESPHome OTA requires encryption and uses that key. Update with the kettle idle.

```shell
python -m esphome run ikettle-smk20-eu.yaml --device ikettle-smk20-eu.local
```

Use the current device address if hostname lookup fails. After reboot, check that the firmware version, controller status and temperature return in Home Assistant.

If a build will not run normally, the profile enters safe mode after three failed boots. The **Restart in safe mode** control lets you request it while the device is still reachable. Safe mode keeps networking and OTA running while the kettle component is inactive. Allow the kettle to reconnect. If it cannot join your network, connect your computer to the configured fallback Wi-Fi using its separate password. The usual ESPHome fallback address is `192.168.4.1`. Repeat the encrypted OTA command with `--device 192.168.4.1`, then reconnect your computer to its usual network and verify the device. Wait for the profile's 90-second fallback interval if the access point takes time to appear. [ESPHome OTA](https://esphome.io/components/ota/esphome/) · [ESPHome safe mode](https://esphome.io/components/safe_mode/) · [ESPHome captive portal](https://esphome.io/components/captive_portal/)

If both normal and fallback OTA are unavailable, use the serial connection in the [hardware guide](serial-migration.md). Returning to stock uses your own full flash backup and the serial restore procedure.
