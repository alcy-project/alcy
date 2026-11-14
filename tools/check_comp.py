#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""The compile-time evaluator's census over the acceptance corpus.

Lowering hands its IR to the backends and to the second engine
(ADR-0054), and the corpus is where the two are asked to agree. Every
case that compiles is written in the binary IR form and handed to the
comp_census driver, which runs each function the engine can start and fails
while the engine reports a compiler bug or a refusal it has not
declared. A case the front end rejects is the corpus's
expected-rejection half and sits this check out, so a construct the
lowering emits and the engine cannot carry is caught here by the corpus
instead of by a reader of the engine's source.
"""

import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from utils.env import run_environment
from utils.paths import project_root_dir


def collect_cases() -> list[Path]:
    """Every acceptance case: the e2e cases and the wasm ones."""
    cases: list[Path] = []
    for root in ("e2e/cases", "e2e/wasm"):
        base = project_root_dir / root
        cases.extend(p for p in sorted(base.iterdir()) if p.is_dir())
    return cases


def emit_ir(alcy: Path, case_dir: Path, out_path: Path) -> Path | None:
    """Writes the case's package in the binary IR form, or None.

    A package builds as one; a single main.al compiles as one. A case
    the front end rejects is expected here, so its failure is a skip and
    not a finding.
    """
    if (case_dir / "alcy.toml").is_file():
        argv = [str(alcy), "build", ".", "--emit", "ir-bc", "-o", str(out_path)]
    elif (case_dir / "main.al").is_file():
        argv = [
            str(alcy),
            "compile",
            "main.al",
            "--emit",
            "ir-bc",
            "-o",
            str(out_path),
        ]
    else:
        return None
    proc = subprocess.run(
        argv,
        capture_output=True,
        text=True,
        encoding="utf-8",
        cwd=case_dir,
        env=run_environment(),
    )
    if proc.returncode != 0 or not out_path.is_file():
        return None
    return out_path


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-subdir",
        default="build",
        help="Subdirectory inside out/ holding the alcy and comp_census binaries",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=0,
        help=(
            "Cases to compile concurrently (default: the runner's core "
            "count, capped at 8). A case is a compiler invocation of its "
            "own, so concurrent cases wait on their children rather than "
            "on each other."
        ),
    )
    args = parser.parse_args()

    out = project_root_dir / "out" / args.build_subdir
    tools = {}
    for name in ("alcy", "comp_census"):
        binary = out / name
        if not binary.is_file():
            binary = binary.with_suffix(".exe")
        if not binary.is_file():
            print(
                f"{name} binary not found in out/{args.build_subdir}/; "
                "run tools/build.py --target=all --mode=debug "
                f"--build-subdir={args.build_subdir} first"
            )
            return -1
        tools[name] = binary

    case_dirs = collect_cases()
    jobs = args.jobs or min(os.cpu_count() or 2, 8)

    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)

        def emitted(case_dir: Path) -> tuple[Path, Path | None]:
            try:
                return case_dir, emit_ir(
                    tools["alcy"], case_dir, tmpdir / f"{case_dir.name}.irbc"
                )
            except Exception as e:  # noqa: BLE001 - a harness bug is a case failure
                sys.exit(f"{case_dir}: the harness raised: {e!r}")

        packages: list[Path] = []
        rejected = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            for _, path in pool.map(emitted, case_dirs):
                if path is None:
                    rejected += 1
                else:
                    packages.append(path)

        # One driver process reads every package, so the census is one
        # verdict rather than one per case, and a summary line per case
        # still names where a refusal lives.
        proc = subprocess.run(
            [str(tools["comp_census"]), *[str(p) for p in packages]],
            env=run_environment(),
        )
        print(
            f"check_comp: {len(case_dirs)} cases, {len(packages)} compiled, "
            f"{rejected} rejected by the front end"
        )
        return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
