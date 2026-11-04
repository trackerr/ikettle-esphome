"""Native ESPHome schemas plus explicit acceptance policy for our recovery profile.

Policy is a release-fixture contract, NOT an extra ESPHome/component validator.
Policy-negative cases first prove native schema acceptance, then reject the
configuration for this particular profile. Nothing is built or run on a board.
"""

import importlib.metadata
from pathlib import Path

from tests.test_config import ConfigValidationSupport, PROJECT, resolved_fixture


FIXTURE = PROJECT / "validation" / "esp8266-1mb-lab.yaml"


def profile_errors(config):
    """Inspect ESPHome's normalized config; TimePeriod values are milliseconds."""
    errors = []
    wifi = config.get("wifi", {})
    ap = wifi.get("ap", {})
    api = config.get("api", {})
    safe = config.get("safe_mode", {})
    native_ota = [item for item in config.get("ota", []) if item.get("platform") == "esphome"]
    if len(native_ota) != 1 or not native_ota[0].get("password"):
        errors.append("native_ota_password_required")
    if not ap.get("password") or not ap.get("ssid") or ap.get("ap_timeout") != 90000:
        errors.append("protected_fallback_ap_required")
    if not api.get("encryption", {}).get("key"):
        errors.append("static_api_key_required")
    if (safe.get("disabled") or safe.get("num_attempts") != 3
            or safe.get("boot_is_good_after") != 60000
            or safe.get("boot_is_good_on_shutdown") is not False
            or safe.get("reboot_timeout") != 600000):
        errors.append("safe_mode_policy")
    if wifi.get("reboot_timeout") != 0 or api.get("reboot_timeout") != 0:
        errors.append("loss_reboots_forbidden")
    if config.get("logger", {}).get("baud_rate") != 0:
        errors.append("uart_logging_forbidden")
    buses = config.get("uart", [])
    if isinstance(buses, dict):
        buses = [buses]
    if any("debug" in bus for bus in buses):
        errors.append("uart_debug_forbidden")
    if "captive_portal" not in config or "web_server" in config:
        errors.append("portal_without_full_dashboard_required")
    if not any(item.get("platform") == "safe_mode" for item in config.get("button", [])):
        errors.append("explicit_safe_mode_button_required")
    # No automatic user actions: profile events must not start heating, restart
    # the modem on Wi-Fi/API loss or bypass the single-owner UART parser.
    forbidden_hooks = {
        "on_boot", "on_shutdown", "on_loop", "on_connect", "on_disconnect",
        "on_client_connected", "on_client_disconnected", "on_safe_mode",
    }
    forbidden_actions = {"lambda", "uart.write", "uart.read"}

    def inspect(value):
        if isinstance(value, dict):
            for key, child in value.items():
                if key in forbidden_hooks or key in forbidden_actions:
                    return True
                if inspect(child):
                    return True
        elif isinstance(value, list):
            return any(inspect(child) for child in value)
        return False

    if "interval" in config or "script" in config or inspect(config):
        errors.append("automatic_actions_or_uart_passthrough_forbidden")
    return errors


class RecoveryConfigTests(ConfigValidationSupport):
    @classmethod
    def setUpClass(cls):
        if importlib.metadata.version("esphome") != "2026.10.2":
            raise RuntimeError("Use the isolated ESPHome2026.10.2 environment")
        cls.base = resolved_fixture(FIXTURE)

    def policy_rejects_native_valid(self, mutate, expected):
        normalized = self.validate(mutate, valid=True)
        self.assertIn(expected, profile_errors(normalized))

    def test_valid_1mb_recovery_profile_normalized_by_esphome(self):
        normalized = self.validate(valid=True)
        self.assertEqual(normalized["esp8266"]["board"], "esp01_1m")
        self.assertEqual(profile_errors(normalized), [])
        # Captive portal auto-loads HTTP OTA. The native password does not
        # authenticate that separate endpoint; do not assert that it does.
        self.assertTrue(any(item["platform"] == "web_server" for item in normalized["ota"]))
        self.assertNotIn("web_server", normalized)

    def test_native_schema_rejects_short_ap_password(self):
        self.validate(lambda c: c["wifi"]["ap"].update(password="1234567"), diagnostic="at least 8")

    def test_native_schema_rejects_overlong_ap_password(self):
        self.validate(lambda c: c["wifi"]["ap"].update(password="x" * 65), diagnostic="at most 64")

    def test_native_schema_rejects_invalid_api_key(self):
        self.validate(lambda c: c["api"]["encryption"].update(key="not-base64"), diagnostic="key")

    def test_native_schema_rejects_zero_safe_mode_attempts(self):
        self.validate(lambda c: c["safe_mode"].update(num_attempts=0), diagnostic="num_attempts")

    def test_native_schema_rejects_invalid_shutdown_boolean(self):
        self.validate(lambda c: c["safe_mode"].update(boot_is_good_on_shutdown="not-a-boolean"),
                      diagnostic="boot_is_good_on_shutdown")

    def test_native_schema_rejects_safe_mode_inside_ota(self):
        self.validate(lambda c: c["ota"][0].update(safe_mode=True), diagnostic="moved")

    def test_profile_policy_rejects_native_valid_empty_ota_password(self):
        self.policy_rejects_native_valid(lambda c: c["ota"][0].update(password=""),
                                         "native_ota_password_required")

    def test_profile_policy_rejects_native_valid_open_fallback_ap(self):
        self.policy_rejects_native_valid(lambda c: c["wifi"]["ap"].update(password=""),
                                         "protected_fallback_ap_required")

    def test_profile_policy_rejects_runtime_only_api_key(self):
        self.policy_rejects_native_valid(lambda c: c["api"].update(encryption={}),
                                         "static_api_key_required")

    def test_profile_policy_rejects_weakened_native_valid_safe_mode(self):
        for change in ({"num_attempts": 10}, {"boot_is_good_after": "120s"},
                       {"boot_is_good_on_shutdown": True}, {"reboot_timeout": "1min"}):
            with self.subTest(change=change):
                self.policy_rejects_native_valid(lambda c: c["safe_mode"].update(change),
                                                 "safe_mode_policy")

    def test_profile_policy_rejects_wifi_or_api_loss_reboots(self):
        for component in ("wifi", "api"):
            with self.subTest(component=component):
                self.policy_rejects_native_valid(lambda c: c[component].update(reboot_timeout="1min"),
                                                 "loss_reboots_forbidden")

    def test_profile_policy_rejects_wifi_loss_restart_automation(self):
        def mutate(config):
            config["button"][0]["id"] = "synthetic_recovery_button"
            config["wifi"]["on_disconnect"] = [
                {"then": [{"button.press": "synthetic_recovery_button"}]}]
        self.policy_rejects_native_valid(mutate, "automatic_actions_or_uart_passthrough_forbidden")

    def test_profile_policy_rejects_boot_actuator_lambda(self):
        def mutate(config):
            config["esphome"]["on_boot"] = [
                {"then": [{"lambda": "id(kettle).perform(esphome::ikettle_mcu::ButtonAction::START);"}]}]
        self.policy_rejects_native_valid(mutate, "automatic_actions_or_uart_passthrough_forbidden")

    def test_profile_policy_rejects_periodic_raw_uart_writer(self):
        def mutate(config):
            config["interval"] = [{"interval": "1s", "then": [
                {"uart.write": {"id": "mcu_uart", "data": "AT\r\n"}}]}]
        self.policy_rejects_native_valid(mutate, "automatic_actions_or_uart_passthrough_forbidden")

    def test_real_secret_loader_requires_ota_secret(self):
        with self.assertRaisesRegex(AssertionError, "ikettle_ota_password"):
            resolved_fixture(FIXTURE, omit_secret="ikettle_ota_password")


if __name__ == "__main__":
    import unittest
    unittest.main(verbosity=2)
