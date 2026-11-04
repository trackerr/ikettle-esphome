"""ESPHome-native virtual AT endpoint for the original SMK20/F19 MCU."""

import zlib

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import binary_sensor, button, number, sensor, switch, text_sensor, uart
from esphome.const import (
    CONF_BAUD_RATE, CONF_DEBUG, CONF_ID, CONF_RX_PIN, CONF_TX_PIN, CONF_UART_ID,
    DEVICE_CLASS_DURATION, DEVICE_CLASS_ENERGY, DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC, UNIT_CELSIUS, UNIT_SECOND, UNIT_WATT_HOURS,
)

DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor", "button", "number", "sensor", "switch", "text_sensor", "preferences"]
CODEOWNERS = []

ns = cg.esphome_ns.namespace("ikettle_mcu")
IKettleComponent = ns.class_("IKettleComponent", cg.Component, uart.UARTDevice)
KettleNumber = ns.class_("KettleNumber", number.Number)
KettleButton = ns.class_("KettleButton", button.Button)
KettleTraceSwitch = ns.class_("KettleTraceSwitch", switch.Switch)
NumberKind = ns.enum("NumberKind", is_class=True)
ButtonAction = ns.enum("ButtonAction", is_class=True)


def _keep(value):
    value = cv.int_range(min=0, max=30)(value)
    if value not in (0, *range(5, 31)):
        raise cv.Invalid("Keep-warm requires 0 or 5..30 minutes")
    return value


def _trace_switch(value):
    if isinstance(value, dict):
        if str(value.get("restore_mode", "ALWAYS_OFF")).upper() != "ALWAYS_OFF":
            raise cv.Invalid("UART trace requires restore_mode ALWAYS_OFF")
        for hook in ("on_state", "on_turn_on", "on_turn_off"):
            if hook in value:
                raise cv.Invalid("UART trace does not allow custom automation hooks")
    return value


def _trace_timeout(value):
    value = cv.positive_time_period_milliseconds(value)
    if not 10000 <= value.total_milliseconds <= 300000 or value.total_milliseconds % 1000:
        raise cv.Invalid("trace_timeout must be whole seconds between 10s and 300s")
    return value


def _trace_options(config):
    if "uart_trace" not in config:
        if "trace_timeout" in config or "trace_rate_limit" in config:
            raise cv.Invalid("trace_timeout/trace_rate_limit require uart_trace")
    else:
        config.setdefault("trace_timeout", cv.TimePeriod(milliseconds=120000))
        config.setdefault("trace_rate_limit", 8)
    return config


def _cycle_options(config):
    if "cycle_heating_energy" in config and "heater_power_watts" not in config:
        raise cv.Invalid("cycle_heating_energy requires heater_power_watts")
    return config


CONFIG_SCHEMA = cv.All(
    cv.only_on_esp8266,
    cv.Schema({
        cv.GenerateID(): cv.declare_id(IKettleComponent),
        cv.Optional("initial_target", default=90): cv.int_range(min=20, max=100),
        cv.Optional("initial_keep_warm", default=0): _keep,
        cv.Optional("restore_selection", default=False): cv.boolean,
        cv.Optional("target_temperature"): number.number_schema(KettleNumber, unit_of_measurement=UNIT_CELSIUS),
        cv.Optional("keep_warm_minutes"): number.number_schema(KettleNumber, unit_of_measurement="min"),
        cv.Optional("temperature"): sensor.sensor_schema(unit_of_measurement=UNIT_CELSIUS, accuracy_decimals=0),
        cv.Optional("water_raw"): sensor.sensor_schema(accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional("base_raw"): sensor.sensor_schema(accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional("default_temperature"): sensor.sensor_schema(unit_of_measurement=UNIT_CELSIUS, accuracy_decimals=0,
                                                                 entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional("default_keep_warm"): sensor.sensor_schema(unit_of_measurement="min", accuracy_decimals=0,
                                                               entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional("default_formula_temperature"): sensor.sensor_schema(unit_of_measurement=UNIT_CELSIUS,
                                                                         accuracy_decimals=0,
                                                                         entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional("status_age"): sensor.sensor_schema(unit_of_measurement=UNIT_SECOND, accuracy_decimals=1,
                                                        entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional("mcu_reply_age"): sensor.sensor_schema(
            unit_of_measurement=UNIT_SECOND, accuracy_decimals=1, device_class=DEVICE_CLASS_DURATION,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional("serial_errors"): sensor.sensor_schema(
            accuracy_decimals=0, entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional("cycle_start_temperature"): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS, accuracy_decimals=0, device_class=DEVICE_CLASS_TEMPERATURE),
        cv.Optional("cycle_heating_time"): sensor.sensor_schema(
            unit_of_measurement=UNIT_SECOND, accuracy_decimals=0, device_class=DEVICE_CLASS_DURATION),
        cv.Optional("cycle_keep_warm_time"): sensor.sensor_schema(
            unit_of_measurement=UNIT_SECOND, accuracy_decimals=0, device_class=DEVICE_CLASS_DURATION),
        cv.Optional("cycle_heating_energy"): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT_HOURS, accuracy_decimals=1, device_class=DEVICE_CLASS_ENERGY),
        cv.Optional("heater_power_watts"): cv.float_range(min=1, max=4000),
        cv.Optional("on_base"): binary_sensor.binary_sensor_schema(),
        cv.Optional("protocol_ready"): binary_sensor.binary_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional("activity"): text_sensor.text_sensor_schema(),
        cv.Optional("mcu_firmware"): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional("protocol_error"): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        cv.Optional("command_status"): text_sensor.text_sensor_schema(),
        cv.Optional("uart_trace"): cv.All(
            _trace_switch,
            switch.switch_schema(KettleTraceSwitch, block_inverted=True,
                                 default_restore_mode="ALWAYS_OFF", entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
        ),
        cv.Optional("trace_timeout"): _trace_timeout,
        cv.Optional("trace_rate_limit"): cv.int_range(min=1, max=20),
        **{cv.Optional(key): button.button_schema(KettleButton) for key in
           ("start", "stop", "start_formula", "stop_keep_warm", "refresh")},
    }).extend(cv.COMPONENT_SCHEMA).extend(uart.UART_DEVICE_SCHEMA),
    _trace_options,
    _cycle_options,
)


def _final_validate(config):
    uart.final_validate_device_schema("ikettle_mcu", baud_rate=115200, data_bits=8, parity="NONE", stop_bits=1,
                                      require_tx=True, require_rx=True)(config)
    full = fv.full_config.get()
    path = full.get_path_for_id(config[CONF_UART_ID])[:-1]
    bus = full.get_config_for_path(path)
    pins = (bus[CONF_TX_PIN]["number"], bus[CONF_RX_PIN]["number"])
    if pins not in ((1, 3), (15, 13)):
        raise cv.Invalid("ikettle_mcu requires ESP8266 hardware UART0: TX/RX1/3 or swapped15/13; verify physical routing")
    if CONF_DEBUG in bus:
        raise cv.Invalid("UART debug could disclose MCU-stored Wi-Fi credentials; remove debug from this bus")
    logger = full.get("logger", {})
    if "uart_trace" in config and (not logger or logger.get("level", "DEBUG") not in
                                    ("INFO", "DEBUG", "VERBOSE", "VERY_VERBOSE")):
        raise cv.Invalid("UART trace requires logger with global level INFO or higher verbosity")
    if logger and logger.get(CONF_BAUD_RATE, 115200) != 0:
        raise cv.Invalid("Set logger.baud_rate:0 to keep logs out of the MCU modem UART")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_initial_target(config["initial_target"]))
    cg.add(var.set_initial_keep_warm(config["initial_keep_warm"]))
    cg.add(var.set_restore_selection(config["restore_selection"]))
    if "heater_power_watts" in config:
        cg.add(var.set_heater_power_watts(config["heater_power_watts"]))
    if "uart_trace" in config:
        cg.add(var.set_trace_timeout(config["trace_timeout"].total_milliseconds))
        cg.add(var.set_trace_rate_limit(config["trace_rate_limit"]))
        item = await switch.new_switch(config["uart_trace"])
        cg.add(item.set_parent(var))
        cg.add(var.set_uart_trace_switch(item))
    key = zlib.crc32((str(config[CONF_ID]) + ":ikettle-selection-v1").encode("ascii"))
    cg.add(var.set_preference_key(key))
    for field, kind, low, high, step in (
        ("target_temperature", NumberKind.TARGET, 20, 100, 1),
        ("keep_warm_minutes", NumberKind.KEEP_WARM, 0, 30, 1),
    ):
        if field in config:
            item = await number.new_number(config[field], min_value=low, max_value=high, step=step)
            cg.add(item.set_parent(var))
            cg.add(item.set_kind(kind))
            cg.add(getattr(var, f"set_{field}_number")(item))
    for field in ("temperature", "water_raw", "base_raw", "default_temperature", "default_keep_warm",
                  "default_formula_temperature", "status_age", "mcu_reply_age", "serial_errors",
                  "cycle_start_temperature", "cycle_heating_time",
                  "cycle_keep_warm_time", "cycle_heating_energy"):
        if field in config:
            cg.add(getattr(var, f"set_{field}_sensor")(await sensor.new_sensor(config[field])))
    for field in ("on_base", "protocol_ready"):
        if field in config:
            cg.add(getattr(var, f"set_{field}_binary_sensor")(await binary_sensor.new_binary_sensor(config[field])))
    for field in ("activity", "mcu_firmware", "protocol_error", "command_status"):
        if field in config:
            cg.add(getattr(var, f"set_{field}_text_sensor")(await text_sensor.new_text_sensor(config[field])))
    for field, action in (("start", ButtonAction.START), ("stop", ButtonAction.STOP),
                          ("start_formula", ButtonAction.FORMULA), ("stop_keep_warm", ButtonAction.STOP_WARM),
                          ("refresh", ButtonAction.REFRESH)):
        if field in config:
            item = await button.new_button(config[field])
            cg.add(item.set_parent(var))
            cg.add(item.set_action(action))
