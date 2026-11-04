"""Compile/run UART trace host tests with installed tools; no download/device I/O."""

import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def main():
    out = ROOT / "build" / "uart-trace-host"
    out.mkdir(parents=True, exist_ok=True)
    include = ROOT / "components" / "ikettle_mcu"
    sources = [ROOT / "tests/test_uart_trace.cpp", include / "modem.cpp", include / "uart_trace.cpp"]
    tracked = [ROOT / "tests/test_uart_trace.cpp", Path(__file__),
               *sorted(include.glob("*.cpp")), *sorted(include.glob("*.h")), include / "__init__.py"]
    source_hashes = {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in tracked}
    exe = out / ("uart-trace-tests.exe" if os.name == "nt" else "uart-trace-tests")
    transcript = []
    commands = []

    def run(command):
        commands.append(command)
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, errors="replace")
        text = result.stdout + result.stderr
        transcript.append(text)
        print(text, end="", flush=True)
        return result.returncode, text

    if os.name == "nt":
        vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / (
            "Microsoft Visual Studio/Installer/vswhere.exe")
        if not vswhere.is_file():
            raise SystemExit("Installed MSVC Build Tools/vswhere required; no downloads.")
        found = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
        vcvars = Path(found) / "VC/Auxiliary/Build/vcvars64.bat"
        if not found or not vcvars.is_file():
            raise SystemExit("Installed MSVC C++ toolset and SDK required.")
        rsp = out / "trace-build.rsp"
        flags = ["/nologo", "/std:c++17", "/EHsc", "/W4", "/WX", "/utf-8",
                 f'/I"{include}"', f'/Fo"{out.as_posix()}/"', f'/Fe"{exe}"']
        rsp.write_text("\n".join(flags + [f'"{path}"' for path in sources]) + "\n", encoding="utf-8")
        script = out / "trace-build.cmd"
        script.write_text('@echo off\ncall "' + str(vcvars) + '" >nul\nif errorlevel 1 exit /b 1\n'
                          'cl @"' + str(rsp) + '"\nif errorlevel 1 exit /b 1\n', encoding="utf-8")
        compile_code, _ = run(["cmd.exe", "/d", "/c", str(script)])
    else:
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise SystemExit("Installed C++17 host compiler required; no downloads.")
        compile_code, _ = run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(include),
                               *map(str, sources), "-o", str(exe)])
    test_code, output = (None, "") if compile_code else run([str(exe)])
    match = re.search(r"(\d+)/(\d+) uart trace groups passed", output)
    passed, total = (int(match[1]), int(match[2])) if match else (0, 0)
    success = compile_code == 0 and test_code == 0 and total > 0 and passed == total
    source_after = {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in tracked}
    record = {
        "recorded_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "command": [sys.executable, str(Path(__file__))], "commands_executed": commands,
        "compile_exit_code": compile_code, "test_exit_code": test_code,
        "trace_groups_passed": passed, "trace_groups_total": total,
        "cases_passed": passed, "cases_total": total,
        "failures": total - passed if match else int(not success), "exit_code": 0 if success else 1,
        "source_sha256": source_hashes, "source_sha256_after": source_after,
        "source_unchanged_during_run": source_hashes == source_after,
        "compiled_sources": [path.relative_to(ROOT).as_posix() for path in sources],
        "device_io": False, "hardware_io": False, "serial_io": False, "network_io": False, "target_compile": False,
        "scope": "Native modem/trace callbacks and fake clock/UART only; no hardware or radio/timing proof.",
    }
    (ROOT / "build/trace-native-results.log").write_text("".join(transcript), encoding="utf-8")
    (ROOT / "build/trace-native-results.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    if not record["source_unchanged_during_run"]:
        raise SystemExit("Source changed during host tests; result invalid.")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
