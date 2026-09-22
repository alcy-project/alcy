#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import os
import subprocess

from utils.paths import (
    project_root_dir,
)
from concurrent.futures import (
    ThreadPoolExecutor,
    as_completed,
)


def run_command(cmd: list[str]) -> bool:
    result = subprocess.run(cmd, cwd=project_root_dir)
    return result.returncode == 0


def run_commands_in_parallel(commands: list[list[str]], max_workers: int = 0) -> bool:
    if not commands:
        return True

    if max_workers == 0:
        max_workers = os.cpu_count() or 4

    all_success = True
    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        futures = [executor.submit(run_command, cmd) for cmd in commands]
        for future in as_completed(futures):
            if not future.result():
                all_success = False
    return all_success
