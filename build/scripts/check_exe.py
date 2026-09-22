#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""End-to-end execution checks.

Each case is a directory under exe/cases/<name>/ holding main.al and
expect.txt:

    exit: 0
    stdout: hi\\n
    stdout-contains: hi
    stderr-contains: boom

`exit` is required (negative values match signal termination, e.g.
-6 for SIGABRT). `stdout` must match exactly when present;
`*contains` lines must all appear in the respective stream.

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
from pathlib import Path

from utils.paths import project_root_dir


def parse_expect(path: Path):
    expected_exit = None
    expected_stdout = None
    stdout_contains = []
    stderr_contains = []
    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, start=1):
            line = raw.rstrip("\n")
            if not line.strip() or line.strip().startswith("#"):
                continue
            key, sep, value = line.partition(":")
            if not sep:
                sys.exit(f"{path}:{lineno}: malformed line: {raw.strip()}")
            key = key.strip()
            value = value.strip()
            if key == "exit":
                expected_exit = int(value)
            elif key == "stdout":
                expected_stdout = value.encode().decode("unicode_escape")
            elif key == "stdout-contains":
                stdout_contains.append(value)
            elif key == "stderr-contains":
                stderr_contains.append(value)
            else:
                sys.exit(f"{path}:{lineno}: unknown key: {key}")
    if expected_exit is None:
        sys.exit(f"{path}: missing required 'exit:' line")
    return expected_exit, expected_stdout, stdout_contains, stderr_contains


def run_case(alcy: Path, case_dir: Path):
    is_package = (case_dir / "alcy.toml").is_file()
    main_al = case_dir / "main.al"
    expect_path = case_dir / "expect.txt"
    if not is_package and not main_al.is_file():
        return False, "no main.al found"
    if not expect_path.is_file():
        return False, "missing expect.txt"
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
                ignore=shutil.ignore_patterns("expect.txt"),
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
                f"--- output ---\n{proc.stdout + proc.stderr}"
            )
        proc = subprocess.run(
            [str(work / exe_name)], capture_output=True, text=True, cwd=work
        )
        if proc.returncode != expected_exit:
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
        sys.exit(f"alcy binary not found in out/{args.build_subdir}/")

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
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    if hasattr(signal, "SIGPIPE"):
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    main()
