#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

from pathlib import Path
from utils.paths import (
    project_root_dir,
    project_source_dirs,
    default_out_dir,
)
from utils.source import (
    source_extensions,
    compile_unit_extensions,
)
from utils.command import run_commands_in_parallel

import sys

import format
import gn_check


def create_commands(
    target_dirs: list[Path], fix: bool, fix_errors: bool
) -> list[list[str]]:
    commands: list[list[str]] = []
    files: list[str] = []
    comp_files: list[str] = []
    for d in target_dirs:
        assert d.is_dir()
        for f in d.rglob("*"):
            relative_path = str(f.relative_to(project_root_dir))
            if f.is_file():
                if f.suffix in source_extensions:
                    files.append(relative_path)
                if f.suffix in compile_unit_extensions:
                    comp_files.append(relative_path)

    base_clang_tidy_cmd = ["clang-tidy"]
    if fix_errors:
        base_clang_tidy_cmd.append("--fix-errors")
    elif fix:
        base_clang_tidy_cmd.append("--fix")

    for f in comp_files:
        commands.append(base_clang_tidy_cmd + [f])

    base_cpplint_cmd = ["uv", "run", "cpplint"]
    for f in files:
        commands.append(base_cpplint_cmd + [f])

    return commands


def lint_files(fix: bool, fix_errors: bool):
    failed = False

    target_dirs = []
    for d in project_source_dirs:
        if not d.is_dir():
            print(f"Directory not found: {d}")
            continue

        target_dirs.append(d)

    format.format_files(dry_run=True)

    for src_dir in project_source_dirs:
        ret = gn_check.check_sources(default_out_dir, src_dir)
        if ret != 0:
            failed = True

    commands = []
    # commands.append(["typos"])
    commands.extend(create_commands(target_dirs, fix, fix_errors))

    if not run_commands_in_parallel(commands):
        failed = True

    if len(commands) == 0:
        print("None of the files were linted")

    if failed:
        print("lint failed")
        return -1
    else:
        print("lint passed")
    return 0


def main():
    fix = False
    fix_errors = False
    if len(sys.argv) >= 2:
        if sys.argv[1] == "--fix":
            print("fix enabled")
            fix = True
        if sys.argv[1] == "--fix-errors":
            print("fix errors enabled")
            fix = True
            fix_errors = True

    return lint_files(fix, fix_errors)


if __name__ == "__main__":
    sys.exit(main())
