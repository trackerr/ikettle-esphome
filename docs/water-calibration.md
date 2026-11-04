# Calibrating water volume

Calibrate the **Water sensor raw** entity on your own jug and base. ADC readings vary between units and can shift with positioning and temperature. The volume package uses those readings to estimate water volume in mL. Cup size is a display conversion. The low-water entity is advisory. Follow the jug's physical fill marks for operation.

## Collect reference readings

1. Let the jug cool, empty it and set it on the base with the kettle idle. Record several stable **Water sensor raw** samples as it settles. Record the center of the readings and the highest stable empty reading. The latter is the empty cutoff so empty-jug variation can still display 0 mL.
2. Use a measuring jug or scale to add 500 mL of room-temperature water. Seat the jug consistently and record the center of several stable raw readings.
3. Repeat at 1,000 mL and 1,500 mL. Keep the fill within the limits printed on your jug. If you cannot measure all reference volumes within its markings, leave the volume package disabled.
4. Confirm that the readings increase with volume and are separated from one another by more than their normal sample variation. The calibration baseline sensor shows a different controller value and is not a volume reference.

## Enable the optional package

In `ikettle-smk20-eu.yaml`, enable the `water` include under `packages`. Add the measured values to the existing top-level `substitutions` block. The numbers below are synthetic and show the expected ordering. Replace them with your own readings before building.

```yaml
substitutions:
  kettle_empty_raw: "2000"
  kettle_empty_cutoff_raw: "2008"
  kettle_500_raw: "2100"
  kettle_1000_raw: "2200"
  kettle_1500_raw: "2300"
  kettle_cup_ml: "250"
  kettle_low_water_ml: "500"
```

The package rounds the estimate to the nearest 50 mL. With the example 250 mL cup setting, a 750 mL estimate displays as three cups. Use `kettle_cup_ml` to change the cup size and `kettle_low_water_ml` to set the advisory threshold. Values above the highest calibration point are estimates outside the measured range. If a reading is unavailable while the jug is lifted, the controller link is stale or the jug is being reseated, wait for a stable on-base reading.

After uploading the updated ESPHome configuration, compare the reading at an intermediate volume, for example 750 mL. Lift the jug, place it back and compare again. Record any repeatable offset and recalibrate if the error is larger than you can accept. Home Assistant shows **Water volume**, **Water volume in cups** and **Low water** after the package is enabled.
