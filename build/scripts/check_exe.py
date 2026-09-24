#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""End-to-end execution checks.

Each case is a directory under exe/cases/<name>/ holding main.al and
expect.toml:

    exit = 0
    stdout = "hi\n"
    stdout_contains = ["hi"]
    stderr_contains = ["boom"]

`exit` is required (integer value or "non-zero"). `stdout` must match
exactly when present; `*_contains` lists must all appear in the
respective stream.

Cases are copied to a scratch directory, built to executables with
`alcy build` (which links the embedded runtime), executed, and
asserted. Directories holding alcy.toml build as packages.
"""

import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

from utils.paths import project_root_dir


def parse_expect(path: Path):
    try:
        with open(path, "rb") as f:
            data = tomllib.load(f)
    except tomllib.TOMLDecodeError as e:
        sys.exit(f"{path}: invalid TOML format: {e}")

    expected_exit = data.get("exit")
    if expected_exit is None:
        sys.exit(f"{path}: missing required 'exit' key")

    expected_stdout = data.get("stdout")
    stdout_contains = data.get("stdout_contains", [])
    stderr_contains = data.get("stderr_contains", [])

    return expected_exit, expected_stdout, stdout_contains, stderr_contains


def run_case(alcy: Path, case_dir: Path):
    is_package = (case_dir / "alcy.toml").is_file()
    main_al = case_dir / "main.al"
    expect_path = case_dir / "expect.toml"

    if not is_package and not main_al.is_file():
        return False, "no main.al found"
    if not expect_path.is_file():
        return False, "missing expect.toml"

    expected_exit, expected_stdout, stdout_contains, stderr_contains = parse_expect(
        expect_path
    )
    problems = []

    with tempfile.TemporaryDirectory(prefix="alcy_exe_case") as tmp:
        work = Path(tmp)
        exe_name = "main_exe.exe" if os.name == "nt" else "main_exe"

        if is_package:
            shutil.copytree(
                case_dir,
                work,
                dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("expect.toml"),
            )
            build_argv = [str(alcy), "build", ".", "-o", exe_name]
        else:
            shutil.copy(main_al, work / "main.al")
            build_argv = [str(alcy), "build", "main.al", "-o", exe_name]

        proc = subprocess.run(
            build_argv,
            capture_output=True,
            text=True,
            cwd=work,
        )
        if proc.returncode != 0:
            return False, (
                f"alcy build failed: exit={proc.returncode}\n"
                f"--- build stdout ---\n{proc.stdout}"
                f"--- build stderr ---\n{proc.stderr}"
            )

        exe_path = work / exe_name
        if not exe_path.is_file():
            return False, (
                f"alcy build succeeded but '{exe_name}' is missing\n"
                f"--- build stdout ---\n{proc.stdout}"
                f"--- build stderr ---\n{proc.stderr}"
            )

        try:
            proc = subprocess.run(
                [str(exe_path)], capture_output=True, text=True, cwd=work
            )
        except OSError as e:
            return False, f"cannot execute '{exe_name}': {e}"

        if isinstance(expected_exit, str) and expected_exit.lower() in (
            "non-zero",
            "nonzero",
            "!0",
        ):
            if proc.returncode == 0:
                problems.append(f"exit: got {proc.returncode}, want non-zero")
        elif proc.returncode != expected_exit:
            problems.append(f"exit: got {proc.returncode}, want {expected_exit}")

        if expected_stdout is not None and proc.stdout != expected_stdout:
            problems.append(f"stdout: got {proc.stdout!r}")
        for needle in stdout_contains:
            if needle not in proc.stdout:
                problems.append(f"missing stdout: {needle!r}")
        for needle in stderr_contains:
            if needle not in proc.stderr:
                problems.append(f"missing stderr: {needle!r}")

    if problems:
        detail = "; ".join(problems)
        detail += f"\n--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}"
        return False, detail
    return True, ""


def main():
    parser = argparse.ArgumentParser(description="Run execution cases.")
    parser.add_argument(
        "--build-subdir",
        default="build",
        help="Subdirectory inside out/ holding the alcy binary",
    )
    parser.add_argument(
        "--cases",
        default="",
        help="Comma-separated case names to run (default: all)",
    )
    args = parser.parse_args()

    alcy = project_root_dir / "out" / args.build_subdir / "alcy"
    if not alcy.is_file():
        alcy = alcy.with_suffix(".exe")
    if not alcy.is_file():
        print(f"alcy binary not found in out/{args.build_subdir}/")
        return -1

    cases_root = project_root_dir / "exe" / "cases"
    selected = (
        {name.strip() for name in args.cases.split(",") if name.strip()}
        if args.cases
        else None
    )
    failures = 0
    ran = 0
    for case_dir in sorted(cases_root.iterdir()):
        if not case_dir.is_dir():
            continue
        if selected is not None and case_dir.name not in selected:
            continue
        ran += 1
        ok, detail = run_case(alcy, case_dir)
        print(f"{'PASS' if ok else 'FAIL'} {case_dir.name}")
        if not ok:
            failures += 1
            print(detail)
    print(f"exe: {ran - failures}/{ran} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    if hasattr(signal, "SIGPIPE"):
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    sys.exit(main())
