#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
from pathlib import Path
import sys
import os

import format
import gn_check
from utils.command import run_commands_in_parallel
from utils.paths import (
    default_out_dir,
    project_root_dir,
    project_source_dirs,
)
from utils.source import (
    compile_unit_extensions,
    source_extensions,
)


def target_files(target_dirs: list[Path]):
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
    return files, comp_files


def create_commands(
    files: list[str],
    comp_files: list[str],
    build_path: Path,
    fix: bool,
    fix_errors: bool,
    verbose: bool,
) -> list[list[str]]:
    commands: list[list[str]] = []

    # clang-tidy
    base_clang_tidy_cmd = ["clang-tidy", "-p", str(build_path)]
    if not verbose:
        base_clang_tidy_cmd.append("--quiet")

    if fix_errors:
        base_clang_tidy_cmd.append("--fix-errors")
    elif fix:
        base_clang_tidy_cmd.append("--fix")

    for f in comp_files:
        commands.append(base_clang_tidy_cmd + [f])

    # cpplint
    base_cpplint_cmd = ["uv", "run", "cpplint"]
    if not verbose:
        base_cpplint_cmd.append("--quiet")

    for f in files:
        commands.append(base_cpplint_cmd + [f])

    return commands


def lint_files(
    build_path: Path,
    fix: bool,
    fix_errors: bool,
    verbose: bool,
):
    failed = False

    target_dirs = []
    for d in project_source_dirs:
        if not d.is_dir():
            print(f"Directory not found: {d}")
            continue

        target_dirs.append(d)

    format.format_files(dry_run=True)

    compdb = build_path / "compile_commands.json"
    if not os.path.isfile(compdb):
        print(f"Compilation database not found at: {compdb}")

    for src_dir in project_source_dirs:
        ret = gn_check.check_sources(build_path, src_dir)
        if ret != 0:
            failed = True

    files, comp_files = target_files(target_dirs)

    commands = create_commands(files, comp_files, build_path, fix, fix_errors, verbose)
    if not run_commands_in_parallel(commands):
        failed = True

    if fix or fix_errors:
        format.format_files(dry_run=False)

    if len(commands) == 0:
        print("None of the files were linted")

    if failed:
        print("lint failed")
        return -1
    else:
        print("lint passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Run lint checks on source files.")
    parser.add_argument(
        "-p",
        "--build-path",
        type=Path,
        default=default_out_dir,
        help="Path to the build directory containing compile_commands.json",
    )
    parser.add_argument(
        "--fix", action="store_true", help="Automatically fix standard lint issues"
    )
    parser.add_argument(
        "--fix-errors", action="store_true", help="Automatically fix lint errors"
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Enable verbose output"
    )

    args = parser.parse_args()

    fix = args.fix or args.fix_errors
    fix_errors = args.fix_errors

    if args.verbose:
        import os

        if fix_errors:
            print(f"{os.path.basename(__file__)}: fix errors enabled")
        elif fix:
            print(f"{os.path.basename(__file__)}: fix enabled")

    return lint_files(args.build_path, fix, fix_errors, args.verbose)


if __name__ == "__main__":
    sys.exit(main())
