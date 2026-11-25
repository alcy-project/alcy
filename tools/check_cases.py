#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Run the acceptance cases in `e2e/` and `samples/`.

A case is a directory with a `case.toml`: `[cli]` asks alcy for a verdict,
`[program]` asks the program it built for one. This runner selects the
steps and how a program step is executed; the case file says what each
answer should be.
"""

import argparse
import os
import sys
from pathlib import Path

from cases.runner import build_result, discover
from utils import log, timing
from utils.paths import project_root_dir


def alcy_binary(build_subdir: str) -> Path:
    binary = project_root_dir / "out" / build_subdir / "alcy"
    if not binary.is_file():
        binary = binary.with_suffix(".exe")
    return binary


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-subdir", default="build", help="out/ subdirectory holding alcy"
    )
    parser.add_argument(
        "--cases-root",
        action="append",
        default=[],
        help="case root, relative to the repository (default: e2e/cases)",
    )
    parser.add_argument(
        "--steps",
        default="cli,program",
        help="comma-separated steps to run: cli, program",
    )
    parser.add_argument(
        "--mode",
        choices=("native", "sanitized", "wasm", "qemu"),
        default="native",
        help="how a program step is built and executed",
    )
    parser.add_argument("--cases", default="", help="comma-separated case names")
    parser.add_argument(
        "--backend",
        default="",
        help="backend for native program builds (e.g. direct-x86_64)",
    )
    parser.add_argument("--jobs", type=int, default=0, help="parallel cases")
    log.add_verbose(parser)
    args = parser.parse_args()

    roots = [project_root_dir / root for root in args.cases_root] or [
        project_root_dir / "e2e" / "cases"
    ]
    for root in roots:
        if not root.is_dir():
            return log.die(f"case root not found: {root}")
    steps = [step.strip() for step in args.steps.split(",") if step.strip()]
    unknown = set(steps) - {"cli", "program"}
    if unknown:
        return log.die(f"unknown step(s): {', '.join(sorted(unknown))}")

    alcy = alcy_binary(args.build_subdir)
    if not alcy.is_file():
        return log.die(f"alcy binary not found in out/{args.build_subdir}/")
    if args.mode == "sanitized" and not shutil_which("clang"):
        return log.die("a sanitized run needs clang on PATH")
    if args.mode == "wasm" and not shutil_which("bun"):
        return log.die("a wasm run needs bun on PATH")
    if args.mode == "qemu" and not (
        shutil_which("qemu-riscv64") and shutil_which("clang")
    ):
        return log.die("a qemu run needs qemu-riscv64 and clang on PATH")

    names = {name.strip() for name in args.cases.split(",") if name.strip()} or None
    # A freestanding program owns its entry and links no C runtime.
    # Linux and Windows can provide one; macOS has libSystem as the only
    # sanctioned route, so those cases are not answers this host can give.
    # A wasm sweep already names its refusals in the case file.
    skip_freestanding = args.mode != "wasm" and sys.platform.startswith("darwin")
    selected = []
    skipped_freestanding = 0
    for root, case in discover(roots, names):
        if not (
            ("cli" in steps and case.cli is not None)
            or ("program" in steps and case.program is not None)
        ):
            continue
        if skip_freestanding and case.freestanding:
            skipped_freestanding += 1
            continue
        selected.append((root, case))
    if skipped_freestanding:
        print(
            f"note: {skipped_freestanding} freestanding case(s) skipped: "
            f"{sys.platform} is not a Linux syscall host"
        )
    jobs = args.jobs or min(os.cpu_count() or 2, 8)
    label = ",".join(root.name for root in roots)
    timing.mark("setup")
    result = build_result(alcy, selected, steps, args.mode, jobs, label, args.backend)
    timing.mark("run")
    if result.notes:
        print("\n".join(result.notes))
    logger = log.Logger(args.verbose)
    for name, ok, detail in result.cases:
        logger.case(name, ok, detail)
    return logger.summary(result.label, len(selected))


def shutil_which(name: str) -> bool:
    import shutil

    return shutil.which(name) is not None


if __name__ == "__main__":
    from utils.timing import run_with_timings

    sys.exit(run_with_timings(main))
