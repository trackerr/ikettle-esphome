"""Build and execute portable C++ tests, then ESPHome config-only tests. No device I/O."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, errors="replace", **kwargs)
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode:
        raise SystemExit(result.returncode)
    return result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-only", action="store_true")
    parser.add_argument("--protocol-only", action="store_true",
                        help="Run only existing protocol regressions; trace has its own result file")
    options = parser.parse_args()
    out = ROOT / "build" / "host"
    out.mkdir(parents=True, exist_ok=True)
    sources = [ROOT / "tests" / "test_main.cpp"] + [
        ROOT / "components" / "ikettle_mcu" / name for name in ("modem.cpp", "protocol.cpp")]
    include = ROOT / "components" / "ikettle_mcu"
    exe = out / ("ikettle-tests.exe" if os.name == "nt" else "ikettle-tests")
    if os.name == "nt":
        vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / (
            "Microsoft Visual Studio/Installer/vswhere.exe")
        if not vswhere.is_file():
            raise SystemExit("MSVC Build Tools/vswhere required; no compiler is downloaded automatically.")
        found = subprocess.check_output([
            str(vswhere), "-latest", "-products", "*", "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
        vcvars = Path(found) / "VC/Auxiliary/Build/vcvars64.bat"
        if not found or not vcvars.is_file():
            raise SystemExit("No suitable installed MSVC x64 environment found.")
        rsp = out / "host-build.rsp"
        flags = ["/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/utf-8",
                 f'/I"{include}"', f'/Fo"{out.as_posix()}/"', f'/Fe"{exe}"']
        rsp.write_text("\n".join(flags + [f'"{source}"' for source in sources]) + "\n", encoding="utf-8")
        script = out / "host-build.cmd"
        script.write_text(
            '@echo off\ncall "' + str(vcvars) + '" >nul\nif errorlevel 1 exit /b 1\n'
            'cl @"' + str(rsp) + '"\nif errorlevel 1 exit /b 1\n', encoding="utf-8")
        run(["cmd.exe", "/d", "/c", str(script)])
    else:
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise SystemExit("A C++17 host compiler is required; none is installed by this test runner.")
        run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(include),
             *map(str, sources), "-o", str(exe)])
    output = run([str(exe)])
    match = re.search(r"(\d+)/(\d+) cases passed", output)
    assert match and match[1] == match[2], "Missing complete native test summary"
    result = {"cpp_cases_passed": int(match[1]), "cpp_cases_total": int(match[2]), "device_io": False,
              "source_sha256": {str(source.relative_to(ROOT)): hashlib.sha256(source.read_bytes()).hexdigest()
                                for source in [*sources, include / "modem.h", include / "protocol.h",
                                               include / "uart_trace.h", include / "cycle_stats.h",
                                               include / "drink_temp.h"]}}
    if not options.protocol_only:
        run([sys.executable, "tests/run_trace_tests.py"])
        trace = json.loads((ROOT / "build/trace-native-results.json").read_text(encoding="utf-8-sig"))
        result["trace_native_result_file"] = "build/trace-native-results.json"
        result["trace_native_verified"] = True
    if not options.cpp_only and not options.protocol_only:
        schema_output = run([sys.executable, "-m", "unittest", "discover", "-s", "tests",
                             "-p", "test*config.py", "-v"])
        schema_match = re.search(r"Ran (\d+) tests", schema_output)
        assert schema_match and "\nOK" in schema_output, "Missing complete config/profile test summary"
        result["esphome_schema_and_profile_cases_passed"] = int(schema_match[1])
    (ROOT / "build" / "offline-results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
