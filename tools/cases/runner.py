#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Shared orchestration for every case sweep.

Discovery, staging, the subprocess environment, parallelism and the
report are the same whatever a step does. Executors answer `(ok,
detail)`, or `(None, reason)` when a sweep does not apply to a case.
"""

from __future__ import annotations

import concurrent.futures
import shutil
from pathlib import Path

from utils import log
from utils.env import run_environment
from utils.paths import project_root_dir

from .config import Case, read_case
from .exec import cli, program


def discover(roots: list[Path], names: set[str] | None) -> list[tuple[str, Case]]:
    found = []
    for root in roots:
        for path in sorted(root.iterdir()):
            if not path.is_dir() or not (path / "case.toml").is_file():
                continue
            if names is not None and path.name not in names:
                continue
            found.append((root.name, read_case(path)))
    return found


def case_environment() -> dict[str, str]:
    temp = project_root_dir / "out" / "tmp"
    temp.mkdir(parents=True, exist_ok=True)
    return run_environment({"TMPDIR": str(temp), "TMP": str(temp), "TEMP": str(temp)})


def stage(label: str, case: Case) -> Path:
    target = project_root_dir / "out" / label / case.name / "case"
    if target.exists():
        shutil.rmtree(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(case.path, target)
    return target


def build_result(
    alcy: Path,
    selected: list[tuple[str, Case]],
    steps: list[str],
    mode: str,
    jobs: int,
    label: str,
    backend: str = "",
) -> log.CheckResult:
    result = log.CheckResult(label)
    env = case_environment()

    def checked(item: tuple[str, Case]):
        root_label, case = item
        work = stage(root_label, case)
        outcomes = []
        if "cli" in steps and case.cli is not None:
            outcomes.append(("cli", *cli.run(alcy, case, work, env)))
        if "program" in steps and case.program is not None:
            outcomes.append(
                ("program", *program.run(alcy, case, work, env, mode, backend))
            )
        return case.name, outcomes

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        rows = list(pool.map(checked, selected))

    for name, outcomes in rows:
        if not outcomes:
            result.record(name, True, "no selected step")
            continue
        reasons = [detail for _, ok, detail in outcomes if ok is None]
        failures = [f"{step}: {detail}" for step, ok, detail in outcomes if ok is False]
        if reasons:
            result.note(f"{name}: skipped ({'; '.join(reasons)})")
        result.record(name, not failures, "\n".join(failures))
    return result
