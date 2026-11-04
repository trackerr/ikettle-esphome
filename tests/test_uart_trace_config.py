"""Real ESPHome trace schema validation; no builds, sockets or device I/O.

Always-off defaults and hook restrictions belong to the component schema.
The two fixture checks below additionally enforce this release profile's
explicit diagnostic label, 120-second window and eight-record rate limit.
"""
import importlib.metadata

from tests.test_config import ConfigValidationSupport, FIXTURE, PROJECT, resolved_fixture


class UARTTraceConfigTests(ConfigValidationSupport):
    @classmethod
    def setUpClass(cls):
        if importlib.metadata.version("esphome") != "2026.10.2":
            raise RuntimeError("Use the isolated ESPHome 2026.10.2 environment")
        cls.base = resolved_fixture(FIXTURE)

    def test_both_release_profiles_explicitly_bounded(self):
        for fixture in (FIXTURE, PROJECT / "validation/esp8266-1mb-lab.yaml"):
            with self.subTest(fixture=fixture.name):
                config = resolved_fixture(fixture)
                trace = config["ikettle_mcu"]["uart_trace"]
                self.assertEqual(trace["name"], "UART-trace")
                self.assertEqual(trace["entity_category"], "diagnostic")
                self.assertEqual(trace["restore_mode"], "ALWAYS_OFF")
                self.assertEqual(config["ikettle_mcu"]["trace_timeout"], "120s")
                self.assertEqual(config["ikettle_mcu"]["trace_rate_limit"], 8)
                self.validate(lambda c: (c.clear(), c.update(config)), valid=True)

    def test_traceless_component_remains_valid(self):
        def mutate(config):
            for key in ("uart_trace", "trace_timeout", "trace_rate_limit"):
                config["ikettle_mcu"].pop(key)
            config.pop("logger")
        result = self.validate(mutate, valid=True)
        self.assertNotIn("trace_timeout", result["ikettle_mcu"])
        self.assertNotIn("trace_rate_limit", result["ikettle_mcu"])

    def test_trace_defaults_are_off_and_bounded(self):
        def mutate(config):
            config["ikettle_mcu"]["uart_trace"] = {"name": "UART-trace"}
            config["ikettle_mcu"].pop("trace_timeout")
            config["ikettle_mcu"].pop("trace_rate_limit")
        result = self.validate(mutate, valid=True)["ikettle_mcu"]
        self.assertEqual(result["trace_timeout"], 120000)
        self.assertEqual(result["trace_rate_limit"], 8)
        self.assertEqual(result["uart_trace"]["restore_mode"], "ALWAYS_OFF")

    def test_timeout_limits(self):
        for value, valid in (("10s", True), ("300s", True), ("9s", False), ("301s", False), ("10.5s", False)):
            with self.subTest(value=value):
                self.validate(lambda c: c["ikettle_mcu"].update(trace_timeout=value),
                              valid=valid, diagnostic="trace_timeout")

    def test_rate_limits(self):
        for value, valid in ((1, True), (20, True), (0, False), (21, False)):
            with self.subTest(value=value):
                self.validate(lambda c: c["ikettle_mcu"].update(trace_rate_limit=value),
                              valid=valid, diagnostic="trace_rate_limit")

    def test_options_without_trace_are_rejected(self):
        for key in ("trace_timeout", "trace_rate_limit"):
            with self.subTest(key=key):
                def mutate(config):
                    config["ikettle_mcu"].pop("uart_trace")
                    config["ikettle_mcu"].pop("trace_rate_limit" if key == "trace_timeout" else "trace_timeout")
                self.validate(mutate, diagnostic="require uart_trace")

    def test_logger_required(self):
        self.validate(lambda c: c.pop("logger"), diagnostic="requires logger")

    def test_logger_levels(self):
        for level in ("INFO", "DEBUG", "VERBOSE", "VERY_VERBOSE", "WARN", "ERROR", "NONE"):
            with self.subTest(level=level):
                self.validate(lambda c: c["logger"].update(level=level),
                              valid=level in ("INFO", "DEBUG", "VERBOSE", "VERY_VERBOSE"),
                              diagnostic="global level INFO")

    def test_trace_cannot_use_modem_uart_logger(self):
        self.validate(lambda c: c["logger"].update(baud_rate=115200), diagnostic="logger.baud_rate")

    def test_uart_debug_still_forbidden(self):
        self.validate(lambda c: c["uart"].update(debug={}), diagnostic="UART debug")

    def test_restore_modes_cannot_enable_trace_at_boot(self):
        for mode in ("ALWAYS_ON", "RESTORE_DEFAULT_OFF", "RESTORE_DEFAULT_ON", "DISABLED"):
            with self.subTest(mode=mode):
                self.validate(lambda c: c["ikettle_mcu"]["uart_trace"].update(restore_mode=mode),
                              diagnostic="ALWAYS_OFF")

    def test_inverted_is_rejected_even_when_false(self):
        for value in (True, False):
            with self.subTest(value=value):
                self.validate(lambda c: c["ikettle_mcu"]["uart_trace"].update(inverted=value),
                              diagnostic="Inverted")

    def test_custom_switch_hooks_are_rejected(self):
        for hook in ("on_state", "on_turn_on", "on_turn_off"):
            with self.subTest(hook=hook):
                self.validate(lambda c: c["ikettle_mcu"]["uart_trace"].update({hook: []}),
                              diagnostic="custom automation hooks")


if __name__ == "__main__":
    import unittest
    unittest.main(verbosity=2)
