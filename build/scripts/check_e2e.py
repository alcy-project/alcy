#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""File-based end-to-end acceptance runner.

Each case is a directory under e2e/cases/<name>/ holding either an
alcy.toml package (checked as `alcy check .` with the case directory
as cwd) or a single main.al file (checked as `alcy check main.al`).
An expect.toml file declares the outcome:

    exit = 0
    contains = ["checked 1 file(s)"]
    not_contains = ["error"]

`exit` is required (integer value or "non-zero"); `contains` lines must
all appear in the combined output, `not_contains` lines must all be absent.
"""

import argparse
import subprocess
import sys
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

    contains = data.get("contains", [])
    not_contains = data.get("not_contains", [])

    return expected_exit, contains, not_contains


def run_case(alcy: Path, case_dir: Path):
    if (case_dir / "alcy.toml").is_file():
        argv = [str(alcy), "check", "."]
        cwd = case_dir
    elif (case_dir / "main.al").is_file():
        argv = [str(alcy), "check", str(case_dir / "main.al")]
        cwd = project_root_dir
    else:
        return False, "no alcy.toml or main.al found"

    proc = subprocess.run(argv, capture_output=True, text=True, cwd=cwd)
    output = proc.stdout + proc.stderr
    expect_path = case_dir / "expect.toml"

    if not expect_path.is_file():
        return False, "missing expect.toml"

    expected_exit, contains, not_contains = parse_expect(expect_path)
    problems = []

    if isinstance(expected_exit, str) and expected_exit.lower() in (
        "non-zero",
        "nonzero",
        "!0",
    ):
        if proc.returncode == 0:
            problems.append(f"exit: got {proc.returncode}, want non-zero")
    elif proc.returncode != expected_exit:
        problems.append(f"exit: got {proc.returncode}, want {expected_exit}")

    for needle in contains:
        if needle not in output:
            problems.append(f"missing output: {needle!r}")
    for needle in not_contains:
        if needle in output:
            problems.append(f"unexpected output: {needle!r}")

    if problems:
        return False, "; ".join(problems) + "\n--- output ---\n" + output
    return True, ""


def main():
    parser = argparse.ArgumentParser(description="Run file-based e2e cases.")
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

    cases_root = project_root_dir / "e2e" / "cases"
    selected = (
        {name.strip() for name in args.cases.split(",") if name.strip()}
        if args.cases
        else None
    )
    case_dirs = sorted(p for p in cases_root.iterdir() if p.is_dir())
    if selected is not None:
        case_dirs = [p for p in case_dirs if p.name in selected]

    failures = 0
    ran = 0
    for case_dir in case_dirs:
        ran += 1
        ok, detail = run_case(alcy, case_dir)
        print(f"{'PASS' if ok else 'FAIL'} {case_dir.name}")
        if not ok:
            failures += 1
            print(detail)
    print(f"e2e: {ran - failures}/{ran} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
