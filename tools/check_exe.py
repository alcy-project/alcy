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
`alcy compile` for a single file or `alcy build` for a package, executed,
and asserted. Directories holding alcy.toml build as packages.
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

from utils.env import run_environment
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


# Sanitizers for a generated program. Address is the one that finds
# memory bugs in compiler output; undefined is here because a lowering
# mistake often shows up as an integer or shift operation that is merely
# wrong rather than fatal.
SANITIZERS = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]

# The system compiler, not alcy's, is what instruments a generated
# program. Without it the sanitized run cannot happen, and asking for one
# that cannot happen is a refusal rather than a downgrade: see main().
SYSTEM_CLANG = shutil.which("clang")


def build_sanitized(alcy: Path, work: Path, is_package: bool, exe: Path):
    """Builds the case through textual IR so the sanitizers can reach it.

    alcy writes an object straight from an LLVM TargetMachine, with no
    external compiler in the path, so the code it generates cannot be
    instrumented after the fact. Textual IR can: the module is handed to
    clang, which both compiles and instruments it, runtime included.
    That is the reason --emit=llvm-ir exists, and this is what it is for.
    """
    ir = work / "main.ll"
    command = "build" if is_package else "compile"
    argv = [str(alcy), command, "." if is_package else "main.al",
            "--emit=llvm-ir", "-o", str(ir)]
    proc = subprocess.run(
        argv,
        capture_output=True,
        text=True,
        encoding="utf-8",
        cwd=work,
        env=run_environment(),
    )
    if proc.returncode != 0 or not ir.is_file():
        return proc, "alcy --emit=llvm-ir did not produce a module"

    link = [SYSTEM_CLANG, "-x", "ir", str(ir), *SANITIZERS, "-o", str(exe)]
    proc = subprocess.run(
        link, capture_output=True, text=True, encoding="utf-8", cwd=work
    )
    if proc.returncode != 0:
        return proc, "clang could not compile the generated IR"
    return proc, ""


def run_case(alcy: Path, case_dir: Path, sanitize: bool = False):
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
            target = "."
        else:
            shutil.copy(main_al, work / "main.al")
            target = "main.al"

        command = "build" if is_package else "compile"
        if sanitize:
            proc, why = build_sanitized(alcy, work, is_package, work / exe_name)
            if why:
                return False, f"{why}\n--- stderr ---\n{proc.stderr}"
        else:
            build_argv = [str(alcy), command, target, "-o", exe_name]
            proc = subprocess.run(
                build_argv,
                capture_output=True,
                text=True,
                encoding="utf-8",
                cwd=work,
                env=run_environment(),
            )
        if proc.returncode != 0:
            return False, (
                f"alcy {command} failed: exit={proc.returncode}\n"
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
                [str(exe_path)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                cwd=work,
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
            problems.append(
                f"stdout: got {proc.stdout!r}, want {expected_stdout!r}"
            )
        for needle in stdout_contains:
            if needle not in proc.stdout:
                problems.append(f"missing stdout: {needle!r}")
        for needle in stderr_contains:
            if needle not in proc.stderr:
                problems.append(f"missing stderr: {needle!r}")

        # A sanitizer writes its report to stderr and exits non-zero, which
        # the exit check may already have caught. This catches the case
        # where a case expects a non-zero exit, so a report would pass for
        # an ordinary failure.
        for marker in (
            "AddressSanitizer",
            "runtime error:",
            "LeakSanitizer",
            "SEGV on unknown address",
        ):
            if marker in proc.stderr:
                problems.append(f"sanitizer report: {marker}")

    if problems:
        detail = "; ".join(problems)
        detail += f"\n--- stdout ---\n{proc.stdout}\n--- stderr ---\n{proc.stderr}"
        return False, detail
    return True, ""


def main():
    # Program output is UTF-8 on every platform; report it as such
    # instead of the console locale, so a unicode mismatch prints
    # rather than crashing the runner.
    sys.stdout.reconfigure(encoding="utf-8")
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
    parser.add_argument(
        "--cases-root",
        default="exe/cases",
        help=(
            "Directory of case directories, relative to the project root. "
            "The samples use the same harness as the test cases, so that a "
            "program kept as an example stays compiled and run."
        ),
    )
    parser.add_argument(
        "--sanitize",
        action="store_true",
        help=(
            "Build each case through --emit=llvm-ir and compile it with "
            "AddressSanitizer and UndefinedBehaviorSanitizer, so a bug in "
            "the code alcy generates is reported rather than inherited."
        ),
    )
    args = parser.parse_args()

    if args.sanitize and SYSTEM_CLANG is None:
        # A run without the sanitizer would pass while testing none of what
        # was asked for, so this refuses rather than degrades. A gate that
        # cannot discriminate is worse than no gate: it is green for a
        # reason nobody reading it can see.
        print(
            "error: --sanitize needs clang on PATH, and it is not there",
            file=sys.stderr,
        )
        return 1

    alcy = project_root_dir / "out" / args.build_subdir / "alcy"
    if not alcy.is_file():
        alcy = alcy.with_suffix(".exe")
    if not alcy.is_file():
        print(f"alcy binary not found in out/{args.build_subdir}/")
        return -1

    cases_root = project_root_dir / args.cases_root
    if not cases_root.is_dir():
        print(f"cases root not found: {args.cases_root}")
        return -1
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
        ok, detail = run_case(alcy, case_dir, args.sanitize)
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
