#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""File-based end-to-end acceptance runner.

Each case is a directory under e2e/cases/<name>/ holding either an
alcy.toml package (checked as `alcy check .` with the case directory
as cwd) or a single main.al file (checked as `alcy check main.al`).
An expect.txt file declares the outcome:

    exit: 0
    contains: checked 1 file(s)
    not-contains: error

`exit` is required; `contains` lines must all appear in the combined
output, `not-contains` lines must all be absent. Cases double as the
demo/acceptance set; keep them minimal and intention-revealing.
"""

import argparse
import subprocess
import sys
from pathlib import Path

from utils.paths import project_root_dir


def parse_expect(path: Path):
    expected_exit = None
    contains = []
    not_contains = []
    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            key, sep, value = line.partition(":")
            if not sep:
                sys.exit(f"{path}:{lineno}: malformed line: {raw.strip()}")
            key = key.strip()
            value = value.strip()
            if key == "exit":
                expected_exit = int(value)
            elif key == "contains":
                contains.append(value)
            elif key == "not-contains":
                not_contains.append(value)
            else:
                sys.exit(f"{path}:{lineno}: unknown key: {key}")
    if expected_exit is None:
        sys.exit(f"{path}: missing required 'exit:' line")
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
    expect_path = case_dir / "expect.txt"
    if not expect_path.is_file():
        return False, "missing expect.txt"
    expected_exit, contains, not_contains = parse_expect(expect_path)
    problems = []
    if proc.returncode != expected_exit:
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
        sys.exit(f"alcy binary not found in out/{args.build_subdir}/")

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
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
