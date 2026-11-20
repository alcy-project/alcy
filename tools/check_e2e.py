#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""File-based end-to-end acceptance runner.

Each case is a directory under e2e/cases/<name>/ holding either an
alcy.toml package (checked as `alcy check .` with the case directory
as cwd) or a single main.al file (checked as `alcy check --file main.al`).
An expect.toml file declares the outcome:

    exit = 0
    contains = ["checked: 1 file(s)"]
    not_contains = ["error"]

`exit` is required (integer value or "non-zero"); `contains` lines must
all appear in the combined output, `not_contains` lines must all be absent.
An optional `args` list is inserted after the selected subcommand, so a
case can name the subcommand's own flags as well as the global ones, and
an optional `env` table sets environment variables for the run. Each case
runs in its own copy under the repository's `out/`, so the artifacts and
the build cache the compiler leaves behind land there too and are removed
with that one directory.
"""

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

from utils import log
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

    contains = data.get("contains", [])
    not_contains = data.get("not_contains", [])
    args = data.get("args", [])
    if not isinstance(args, list) or not all(isinstance(arg, str) for arg in args):
        sys.exit(f"{path}: 'args' must be a list of strings")

    # Defaults to `check`; a case that needs another subcommand (one
    # whose diagnostics are only reported on that path) names it.
    subcommand = data.get("subcommand", "check")
    if not isinstance(subcommand, str):
        sys.exit(f"{path}: 'subcommand' must be a string")

    # Variables the run sets on top of the environment it inherits, so a
    # case can say what the compiler must ignore.
    env = data.get("env", {})
    if not isinstance(env, dict) or not all(
        isinstance(k, str) and isinstance(v, str) for k, v in env.items()
    ):
        sys.exit(f"{path}: 'env' must be a table of strings")

    return (
        expected_exit,
        contains,
        not_contains,
        args,
        subcommand,
        env,
    )


def staged_case(case_dir: Path) -> Path:
    """A private copy of a case under the repository's out/.

    Running from the copy keeps the compiler's own output placement and
    the case's relative paths intact, while every artifact it writes
    stays under out/. A case that times out or fails leaves its copy for
    inspection, and the next run starts from the checked-in source again.
    """
    target = project_root_dir / "out" / "e2e" / case_dir.name / "case"
    if target.exists():
        shutil.rmtree(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(case_dir, target)
    return target


def check_env(extra: dict[str, str]) -> dict[str, str]:
    """Environment for a case, with its temporary files under out/ too."""
    temp = project_root_dir / "out" / "tmp"
    temp.mkdir(parents=True, exist_ok=True)
    env = dict(extra)
    env.update({"TMPDIR": str(temp), "TMP": str(temp), "TEMP": str(temp)})
    return run_environment(env)


def run_case(alcy: Path, case_dir: Path):
    expect_path = case_dir / "expect.toml"
    if not expect_path.is_file():
        return False, "missing expect.toml"

    (
        expected_exit,
        contains,
        not_contains,
        extra_args,
        subcommand,
        env,
    ) = parse_expect(expect_path)
    # An unset variable in the table is the case's business, so the run
    # starts from the environment it inherits with the table applied.
    process_env = check_env(env)
    work = staged_case(case_dir)
    if (work / "alcy.toml").is_file():
        argv = [str(alcy), subcommand, *extra_args, "."]
    elif (work / "main.al").is_file():
        # Single files name their verb explicitly: `check` takes --file
        # and `build` is spelled `compile`.
        if subcommand == "build":
            argv = [str(alcy), "compile", *extra_args, "main.al"]
        else:
            argv = [str(alcy), subcommand, *extra_args, "--file", "main.al"]
    else:
        return False, "no alcy.toml or main.al found"

    proc = subprocess.run(
        argv,
        capture_output=True,
        text=True,
        encoding="utf-8",
        cwd=work,
        env=process_env,
    )
    return compare(proc, expected_exit, contains, not_contains)


def compare(proc, expected_exit, contains, not_contains):
    output = proc.stdout + proc.stderr
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
    # Program output is UTF-8 on every platform; report it as such
    # instead of the console locale, so a unicode mismatch prints
    # rather than crashing the runner.
    sys.stdout.reconfigure(encoding="utf-8")
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
    parser.add_argument(
        "--jobs",
        type=int,
        default=0,
        help=(
            "Cases to run concurrently (default: the runner's core count, "
            "capped at 8). A case is a compiler invocation of its own, so "
            "concurrent cases wait on their children rather than on each "
            "other."
        ),
    )
    log.add_verbose(parser)
    args = parser.parse_args()

    alcy = project_root_dir / "out" / args.build_subdir / "alcy"
    if not alcy.is_file():
        alcy = alcy.with_suffix(".exe")
    if not alcy.is_file():
        return log.die(f"alcy binary not found in out/{args.build_subdir}/")

    cases_root = project_root_dir / "e2e" / "cases"
    selected = (
        {name.strip() for name in args.cases.split(",") if name.strip()}
        if args.cases
        else None
    )
    case_dirs = sorted(p for p in cases_root.iterdir() if p.is_dir())
    if selected is not None:
        case_dirs = [p for p in case_dirs if p.name in selected]

    # A case is one compiler invocation of its own, so the pool waits on
    # subprocesses rather than holding the interpreter, and the results
    # are reported in the sorted order the cases were handed out in.
    jobs = args.jobs or min(os.cpu_count() or 2, 8)

    def checked(case_dir: Path):
        try:
            return run_case(alcy, case_dir)
        except Exception as e:  # noqa: BLE001 - a harness bug is a case failure
            return False, f"the harness raised: {e!r}"

    result = build_result(alcy, case_dirs, jobs)
    logger = log.Logger(args.verbose)
    for name, ok, detail in result.cases:
        logger.case(name, ok, detail)
    sys.stdout.flush()
    return logger.summary(result.label)


def build_result(alcy: Path, case_dirs, jobs: int) -> log.CheckResult:
    """Runs the cases and returns what happened, before any printing."""

    def checked(case_dir: Path):
        try:
            return run_case(alcy, case_dir)
        except Exception as e:  # noqa: BLE001 - a harness bug is a case failure
            return False, f"the harness raised: {e!r}"

    result = log.CheckResult("e2e")
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for case_dir, (ok, detail) in zip(
            case_dirs, pool.map(checked, case_dirs), strict=True
        ):
            result.record(case_dir.name, ok, detail)
    return result


if __name__ == "__main__":
    sys.exit(main())
