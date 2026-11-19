#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import os
import re
import subprocess
import sys
from concurrent.futures import (
    ThreadPoolExecutor,
    as_completed,
)

from utils.paths import (
    project_root_dir,
)


def run_command(cmd: list[str]) -> bool:
    result = subprocess.run(cmd, cwd=project_root_dir)
    return result.returncode == 0


# clang-tidy announces each entry of the compilation database it works
# on, whether or not what it found came to anything. Every entry pays
# its own banner; in a quiet run that is the only noise left.
_BANNER_RE = re.compile(r"^\[\d+/\d+\] \(\d+/\d+\) Processing file ")


def run_clang_tidy(cmd: list[str]) -> bool:
    result = subprocess.run(cmd, cwd=project_root_dir, capture_output=True, text=True)
    for line in result.stderr.splitlines(keepends=True):
        if _BANNER_RE.match(line):
            continue
        sys.stderr.write(line)
    if result.stdout:
        sys.stdout.write(result.stdout)
    return result.returncode == 0


def run_commands_in_parallel(
    commands: list[list[str]],
    max_workers: int = 0,
    verbose: bool = False,
) -> bool:
    if not commands:
        return True

    if max_workers == 0:
        max_workers = os.cpu_count() or 4

    def runner(cmd: list[str]) -> bool:
        if not verbose and cmd[0] == "clang-tidy":
            return run_clang_tidy(cmd)
        return run_command(cmd)

    all_success = True
    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        futures = [executor.submit(runner, cmd) for cmd in commands]
        for future in as_completed(futures):
            if not future.result():
                all_success = False
    return all_success
