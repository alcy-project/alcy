#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""End-to-end acceptance for the direct wasm backend.

Each case is a directory under `e2e/wasm/<name>/` holding a `main.al` and
an `expect.toml`:

    exit = 7
    stdout = "hello\\n"
    contains = ["..."]
    not_contains = ["error"]

The compiler builds the case for wasm with `--backend=direct-wasm`, and
`tools/wasm_run.mjs` runs the module under node's WASI. `exit` is
required; `stdout` is compared exactly when present, and `contains` and
`not_contains` are checked against the combined output. An optional
`args` list is inserted before `compile`.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

from utils.paths import project_root_dir

WASM_TARGET = "wasm32-unknown-emscripten"
WASM_BACKEND = "direct-wasm"


def parse_expect(path: Path):
    try:
        with open(path, "rb") as f:
            data = tomllib.load(f)
    except tomllib.TOMLDecodeError as e:
        sys.exit(f"{path}: invalid TOML format: {e}")

    expected_exit = data.get("exit")
    if expected_exit is None:
        sys.exit(f"{path}: missing required 'exit' key")

    stdout = data.get("stdout")
    contains = data.get("contains", [])
    not_contains = data.get("not_contains", [])
    extra_args = data.get("args", [])
    if stdout is not None and not isinstance(stdout, str):
        sys.exit(f"{path}: 'stdout' must be a string")
    for key, value in (("contains", contains), ("not_contains", not_contains)):
        if not isinstance(value, list) or not all(
            isinstance(item, str) for item in value
        ):
            sys.exit(f"{path}: '{key}' must be a list of strings")
    if not isinstance(extra_args, list) or not all(
        isinstance(arg, str) for arg in extra_args
    ):
        sys.exit(f"{path}: 'args' must be a list of strings")
    return expected_exit, stdout, contains, not_contains, extra_args


def compare(proc, expected_exit, stdout, contains, not_contains):
    problems = []

    if proc.returncode != expected_exit:
        problems.append(f"exit: got {proc.returncode}, want {expected_exit}")

    if stdout is not None and proc.stdout != stdout:
        problems.append(f"stdout: got {proc.stdout!r}, want {stdout!r}")

    output = proc.stdout + proc.stderr
    for needle in contains:
        if needle not in output:
            problems.append(f"missing output: {needle!r}")
    for needle in not_contains:
        if needle in output:
            problems.append(f"unexpected output: {needle!r}")

    if problems:
        return False, "; ".join(problems) + "\n--- output ---\n" + output
    return True, ""


def run_case(alcy: Path, shim: Path, case_dir: Path):
    expect_path = case_dir / "expect.toml"
    if not expect_path.is_file():
        return False, "missing expect.toml"
    source = case_dir / "main.al"
    if not source.is_file():
        return False, "missing main.al"

    (
        expected_exit,
        stdout,
        contains,
        not_contains,
        extra_args,
    ) = parse_expect(expect_path)

    with tempfile.TemporaryDirectory() as scratch:
        module_path = Path(scratch) / "case.wasm"
        compile_args = [
            str(alcy),
            *extra_args,
            "compile",
            str(source),
            f"--target={WASM_TARGET}",
            f"--backend={WASM_BACKEND}",
            "-o",
            str(module_path),
        ]
        compiled = subprocess.run(
            compile_args,
            capture_output=True,
            text=True,
            encoding="utf-8",
            cwd=project_root_dir,
        )
        if compiled.returncode != 0:
            return False, (
                "compile failed:\n" + compiled.stdout + compiled.stderr
            )
        # The warning the WASI module prints on load is the host's, not the
        # program's, and would otherwise read as program output.
        env = dict(os.environ)
        env["NODE_NO_WARNINGS"] = "1"
        ran = subprocess.run(
            ["node", str(shim), str(module_path)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            env=env,
            timeout=60,
        )
    return compare(ran, expected_exit, stdout, contains, not_contains)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(
        description="Run the wasm backend's e2e cases."
    )
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

    cases_root = project_root_dir / "e2e" / "wasm"
    if not cases_root.is_dir():
        print(f"wasm cases not found in {cases_root}")
        return -1
    selected = (
        {name.strip() for name in args.cases.split(",") if name.strip()}
        if args.cases
        else None
    )
    case_dirs = sorted(p for p in cases_root.iterdir() if p.is_dir())
    if selected is not None:
        case_dirs = [p for p in case_dirs if p.name in selected]

    shim = project_root_dir / "tools" / "wasm_run.mjs"
    failures = 0
    ran = 0
    for case_dir in case_dirs:
        ran += 1
        ok, detail = run_case(alcy, shim, case_dir)
        print(f"{'PASS' if ok else 'FAIL'} {case_dir.name}")
        if not ok:
            failures += 1
            print(detail)
    print(f"wasm: {ran - failures}/{ran} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
