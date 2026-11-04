# Home Assistant examples

Add the kettle through **Settings → Devices & services → ESPHome**. If discovery does not find it, enter its IP address and the same encryption key as in `secrets.yaml`. Entity IDs depend on the device name, area and existing entity registry. Find the actual IDs on the kettle's device page before copying any example.

## Dashboard

Add a manual card and paste [`dashboard-card.yaml`](dashboard-card.yaml). Replace the example entity IDs with those from your device page. Uncomment the optional water volume rows once the [water calibration](../docs/water-calibration.md) package is enabled. The presets only select the target temperature. The separate heating and boil-and-cool rows start the heater.

## Drinking-temperature chime

1. Place [`ikettle-drink-temperature.mp3`](media/ikettle-drink-temperature.mp3) in the root of **My media** in Home Assistant. In a standard Home Assistant OS installation this is `/media`.
2. Copy [`drink-temperature-chime.yaml`](blueprints/automation/ikettle/drink-temperature-chime.yaml) into the `blueprints/automation/ikettle/` directory under your Home Assistant configuration. Reload automations.
3. Create an automation from **iKettle drinking temperature chime**. Select the kettle's **Drinking temperature reached** event and a media player that accepts local MP3 playback. A speaker that supports announcements may resume its previous playback.

The automation listens for `drink_temperature_reached`. If your media library uses a custom source, select the file in the media picker and use its URI in the `media_content_id` field. The kettle controller's built-in beeper remains under its own control.
