"""Offline config-only checks. Run with this project's .venv Python."""
from __future__ import annotations

import copy
import importlib.metadata
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import yaml


PROJECT = Path(__file__).resolve().parents[1]
FIXTURE = PROJECT / "validation" / "esp8266-lab.yaml"

# Each child has isolated ESPHome globals and blocks Python network operations.
# Only native config loading/validation runs: no code generation or compilation.
VALIDATOR = r'''
import json
from pathlib import Path
import socket
import sys

def denied(*args, **kwargs):
    raise RuntimeError("Network disabled for offline configuration validation")

for name in ("connect", "connect_ex", "sendto", "sendall"):
    setattr(socket.socket, name, denied)
for name in ("create_connection", "getaddrinfo", "gethostbyname", "gethostbyname_ex"):
    setattr(socket, name, denied)

from esphome.core import CORE, TimePeriod
from esphome import yaml_util
from esphome.components.packages import do_packages_pass, merge_packages
from esphome.components.substitutions import do_substitution_pass
from esphome.config import load_config

def plain(value):
    if isinstance(value, dict):
        return {str(k): plain(v) for k, v in value.items()}
    if isinstance(value, list):
        return [plain(v) for v in value]
    if isinstance(value, TimePeriod):
        return value.total_milliseconds
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    return str(value)

CORE.config_path = Path(sys.argv[1])
if sys.argv[2] == "expand":
    config = yaml_util.load_yaml(Path(CORE.config_path))
    if "packages" in config:
        config = do_packages_pass(config, command_line_substitutions={})
    config = do_substitution_pass(config, {})
    if "packages" in config:
        config = merge_packages(config)
    config.pop("substitutions", None)
    result = {"valid": True, "config": plain(config)}
else:
    config = load_config({}, skip_external_update=True)
    result = {"valid": not config.errors,
              "diagnostics": [str(error) for error in config.errors]}
    if not config.errors:
        result["config"] = plain(config)
print("IKETTLE_CONFIG_RESULT=" + json.dumps(result))
raise SystemExit(0 if result["valid"] else 1)
'''


def _run_validation(config_path, mode):
    directory = config_path.parent
    env = os.environ.copy()
    env.update(PLATFORMIO_CORE_DIR=str(directory / "platformio"),
               ESPHOME_DATA_DIR=str(directory / "esphome-data"),
               PYTHONDONTWRITEBYTECODE="1", NO_COLOR="1")
    result = subprocess.run(
        [sys.executable, "-B", "-c", VALIDATOR, str(config_path), mode],
        cwd=directory, env=env, capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=90, check=False,
    )
    marker = "IKETTLE_CONFIG_RESULT="
    records = [line[len(marker):] for line in result.stdout.splitlines() if line.startswith(marker)]
    if len(records) != 1:
        raise AssertionError("ESPHome validation did not return its result: " + result.stderr)
    payload = json.loads(records[0])
    return result.returncode, payload


def resolved_fixture(fixture, omit_secret=None):
    """Use the real package/substitution loader with temporary PUBLIC dummies."""
    with tempfile.TemporaryDirectory(prefix="fixture-config-", dir=PROJECT) as name:
        directory = Path(name)
        shutil.copyfile(fixture.parent / "recovery.yaml", directory / "recovery.yaml")
        secrets = yaml.safe_load((fixture.parent / "secrets.yaml.example").read_text(encoding="utf-8"))
        if omit_secret is not None:
            secrets.pop(omit_secret)
        (directory / "secrets.yaml").write_text(yaml.safe_dump(secrets), encoding="utf-8")
        target = directory / fixture.name
        shutil.copyfile(fixture, target)
        _, payload = _run_validation(target, "expand")
        config = payload["config"]
        config["external_components"][0]["source"]["path"] = (PROJECT / "components").as_posix()
        return config


class ConfigValidationSupport(unittest.TestCase):
    """Real ESPHome schema tests; no substitute schema or external downloads."""

    def validate(self, mutate=None, *, valid=False, diagnostic=""):
        config = copy.deepcopy(self.base)
        if mutate:
            mutate(config)
        with tempfile.TemporaryDirectory(prefix="config-test-", dir=PROJECT) as name:
            directory = Path(name)
            config["esphome"]["build_path"] = (directory / "build").as_posix()
            config_path = directory / "case.yaml"
            config_path.write_text(yaml.safe_dump(config, sort_keys=False), encoding="utf-8")
            code, payload = _run_validation(config_path, "validate")
            if valid:
                self.assertEqual(code, 0, "Valid config rejected: " + "; ".join(payload["diagnostics"]))
                return payload["config"]
            self.assertNotEqual(code, 0, "Invalid config accepted")
            self.assertIn(diagnostic.lower(), " ".join(payload["diagnostics"]).lower(),
                          "Rejected for an unrelated reason")
            return payload


class ConfigValidationTests(ConfigValidationSupport):
    @classmethod
    def setUpClass(cls):
        if importlib.metadata.version("esphome") != "2026.10.2":
            raise RuntimeError("Use the isolated ESPHome 2026.10.2 environment")
        cls.base = resolved_fixture(FIXTURE)

    def test_valid_lab_fixture(self):
        self.validate(valid=True)

    def test_wrong_uart_baud_rejected(self):
        self.validate(lambda c: c["uart"].update(baud_rate=9600), diagnostic="baud")

    def test_logger_on_modem_uart_rejected(self):
        self.validate(lambda c: c["logger"].update(baud_rate=115200), diagnostic="logger")

    def test_software_uart_pins_rejected(self):
        self.validate(lambda c: c["uart"].update(tx_pin="GPIO4", rx_pin="GPIO5"), diagnostic="hardware uart0")

    def test_uart_debug_rejected(self):
        self.validate(lambda c: c["uart"].update(debug={}), diagnostic="debug")

    def test_keep_warm_one_minute_rejected(self):
        self.validate(lambda c: c["ikettle_mcu"].update(initial_keep_warm=1), diagnostic="keep")

    def test_target_above_boiling_rejected(self):
        self.validate(lambda c: c["ikettle_mcu"].update(initial_target=101), diagnostic="initial_target")


if __name__ == "__main__":
    unittest.main(verbosity=2)
