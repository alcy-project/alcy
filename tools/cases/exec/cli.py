#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""The `[cli]` step: ask alcy for a verdict and read what it says."""

from __future__ import annotations

import subprocess
from pathlib import Path

from cases.config import Case


def run(alcy: Path, case: Case, work: Path, env: dict[str, str]) -> tuple[bool, str]:
    step = case.cli
    if step is None:
        return True, ""
    if (work / "alcy.toml").is_file():
        argv = [str(alcy), step.subcommand, *step.args, "."]
    elif (work / "main.al").is_file():
        verb = "compile" if step.subcommand == "build" else step.subcommand
        flag = [] if verb == "compile" else ["--file"]
        argv = [str(alcy), verb, *step.args, *flag, "main.al"]
    else:
        return False, "no alcy.toml or main.al found"

    run_env = dict(env)
    run_env.update(step.env)
    proc = subprocess.run(
        argv,
        capture_output=True,
        text=True,
        encoding="utf-8",
        cwd=work,
        env=run_env,
    )
    output = proc.stdout + proc.stderr
    problems = []
    want = step.expect.exit if step.expect else 0
    if want == "non-zero":
        if proc.returncode == 0:
            problems.append("exit: got 0, want non-zero")
    elif proc.returncode != want:
        problems.append(f"exit: got {proc.returncode}, want {want}")
    if step.expect is not None:
        for needle in step.expect.contains:
            if needle not in output:
                problems.append(f"missing output: {needle!r}")
        for needle in step.expect.not_contains:
            if needle in output:
                problems.append(f"unexpected output: {needle!r}")
        for needle in step.expect.stderr_contains:
            if needle not in proc.stderr:
                problems.append(f"missing stderr: {needle!r}")
    detail = "; ".join(problems)
    if problems:
        detail += "\n--- output ---\n" + output
    return not problems, detail
