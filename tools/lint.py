#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

import format
import gn_check
from utils.command import run_commands_in_parallel
from utils.paths import (
    excluded_source_dirs,
    out_dir,
    project_root_dir,
    project_source_dirs,
)
from utils.source import (
    compile_unit_extensions,
    header_extensions,
    source_extensions,
)

from build import build


def target_files(target_dirs: list[Path]):
    files: list[str] = []
    comp_files: list[str] = []
    header_files: list[str] = []
    for d in target_dirs:
        assert d.is_dir()
        for f in d.rglob("*"):
            relative_path = str(f.relative_to(project_root_dir))
            if f.is_file():
                if f.suffix in source_extensions:
                    files.append(relative_path)
                if f.suffix in compile_unit_extensions:
                    comp_files.append(relative_path)
                if f.suffix in header_extensions:
                    header_files.append(relative_path)
    return files, comp_files, header_files


def check_ascii_only(files: list[str]) -> bool:
    """Ensure all target source files contain strictly ASCII characters."""
    passed = True
    for rel_path in files:
        filepath = project_root_dir / rel_path
        try:
            with open(filepath, encoding="ascii") as f:
                f.read()
        except UnicodeDecodeError:
            passed = False
            with open(filepath, encoding="utf-8", errors="replace") as f:
                for line_num, line in enumerate(f, start=1):
                    for col_num, char in enumerate(line, start=1):
                        if ord(char) > 127:
                            print(
                                f"Non-ASCII character error: "
                                f"{rel_path}:{line_num}:{col_num}: "
                                f"found '{char}' (U+{ord(char):04X}) in: {line.strip()}"
                            )
    return passed


# Headers whose translation unit is generated: the generator already
# occupies the sibling name, so no source .cc can exist. They stay
# outside the header gate until the generator is renamed. A stale
# entry fails the run instead of silently passing.
GENERATED_HEADER_EXCLUSIONS = [
    "compiler/pipeline/embedded_std.h",
    "compiler/cli/version.h",
]


def exclusion_errors(build_path: Path) -> list[str]:
    siblings = set()
    compdb = build_path / "compile_commands.json"
    if compdb.is_file():
        siblings = {
            (build_path / Path(e["file"])).resolve().with_suffix(".h")
            for e in json.loads(compdb.read_text())
        }
    errors = []
    for excluded in GENERATED_HEADER_EXCLUSIONS:
        if not (project_root_dir / excluded).is_file():
            errors.append(f"Excluded header no longer exists: {excluded}")
        elif (project_root_dir / excluded).resolve() in siblings:
            errors.append(f"Excluded header gained a translation unit: {excluded}")
    return errors


def analysable_headers(build_path: Path, header_files: list[str]) -> list[str]:
    """The headers that have a sibling translation unit in the database.

    Without one, clang-tidy synthesises a command that has no include paths,
    so the file does not parse and the check reports nothing rather than
    failing. Same directory as well as same stem: a stem alone matches an
    unrelated module's file, and that module's flags are not this header's.
    """
    compdb = build_path / "compile_commands.json"
    if not compdb.is_file():
        return []
    siblings = {
        (build_path / Path(e["file"])).resolve().with_suffix(".h")
        for e in json.loads(compdb.read_text())
    }
    return [h for h in header_files if (project_root_dir / h).resolve() in siblings]


def create_commands(
    files: list[str],
    comp_files: list[str],
    header_files: list[str],
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

    for f in comp_files + header_files:
        commands.append(base_clang_tidy_cmd + [f])

    # cpplint takes many files per run; spawning `uv run` once per file
    # costs more in interpreter startup than the check itself.
    if files:
        base_cpplint_cmd = ["uv", "run", "cpplint"]
        if not verbose:
            base_cpplint_cmd.append("--quiet")
        commands.append(base_cpplint_cmd + files)

    return commands


def lint_files(
    build_subdir: str,
    fix: bool,
    fix_errors: bool,
    verbose: bool,
):
    ret = build(
        target="all",
        mode="debug",
        is_clang="true",
        use_lld="true",
        build_subdir=build_subdir,
        target_os="",
        target_cpu="",
        gen_only=True,
        fast=False,
        gn_args_extra="",
    )
    if ret != 0:
        return -1

    failed = False
    build_dir = out_dir / build_subdir
    ret = subprocess.run(["ninja", "-C", build_dir, "setup_llvm"]).returncode
    if ret != 0:
        return -2

    target_dirs = []
    for d in project_source_dirs:
        if not d.is_dir():
            print(f"Directory not found: {d}")
            continue

        target_dirs.append(d)

    format.format_files(dry_run=True)

    compdb = build_dir / "compile_commands.json"
    if not os.path.isfile(compdb):
        print(f"Compilation database not found at: {compdb}")

    for src_dir in project_source_dirs:
        ret = gn_check.check_sources(build_dir, src_dir, excluded_source_dirs)
        if ret != 0:
            failed = True

    files, comp_files, header_files = target_files(target_dirs)

    # Check that all source and header files under project_source_dirs remain pure ASCII
    all_source_files = sorted(set(files + comp_files + header_files))
    if not check_ascii_only(all_source_files):
        failed = True

    for error in exclusion_errors(build_dir):
        print(error)
        failed = True
    header_files = [
        h
        for h in analysable_headers(build_dir, header_files)
        if h not in GENERATED_HEADER_EXCLUSIONS
    ]

    commands = create_commands(
        files, comp_files, header_files, build_dir, fix, fix_errors, verbose
    )
    if not run_commands_in_parallel(commands):
        failed = True

    if fix or fix_errors:
        format.format_files(dry_run=False)

    # A lint that lints nothing is not a pass: a source directory that no
    # longer exists leaves every check silently unapplied, which is exactly
    # what a tree reorganization looks like from here.
    if len(commands) == 0:
        print("None of the files were linted")
        failed = True

    if failed:
        print("lint failed")
        return -1
    else:
        print("lint passed")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Run lint checks on source files.")
    parser.add_argument(
        "--build-subdir",
        default="lint_compdb",
        help="Subdirectory inside out/ (default: lint_compdb)",
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

    return lint_files(args.build_subdir, fix, fix_errors, args.verbose)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted by user. Exiting immediately...", file=sys.stderr)
        os._exit(130)
