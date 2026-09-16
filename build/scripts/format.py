#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

from pathlib import Path
from utils.paths import (
    project_root_dir,
    project_source_dirs,
)
from utils.source import (
    source_extensions,
    gn_extensions,
)
import header_license
import subprocess


def create_commands(target_dirs: list[Path], dry_run: bool) -> list[list[str]]:
    commands: list[list[str]] = []
    files: list[str] = []
    gn_files: list[str] = []
    for d in target_dirs:
        assert d.is_dir()
        for f in d.rglob("*"):
            if f.is_file():
                relative_path = str(f.relative_to(project_root_dir))
                if f.suffix in source_extensions:
                    files.append(relative_path)
                if f.suffix in gn_extensions:
                    gn_files.append(relative_path)

    if len(files) > 0:
        # print("target files:", *files, sep="\n  ")
        base_command = [
            "clang-format",
            "--fail-on-incomplete-format",
            "--ferror-limit=1",
            "--sort-includes",
            "-i",
        ]
        if dry_run:
            base_command.append("--dry-run")
        commands.append(base_command + files)
    if len(gn_files) > 0:
        # print("target gn files:", *gn_files, sep="\n  ")
        base_command = ["gn", "format"]
        if dry_run:
            base_command.append("--dry-run")
        commands.append(base_command + gn_files)
    return commands


def format_files(dry_run: bool):
    header_license.apply_to_files(dry_run)
    target_dirs = []
    for d in project_source_dirs:
        if not d.is_dir():
            print(f"Directory not found: {d}")
            continue

        target_dirs.append(d)

    commands = create_commands(target_dirs, dry_run)
    [subprocess.run(cmd, cwd=project_root_dir) for cmd in commands]

    if len(commands) == 0:
        print("None of the files were formatted")


def main():
    import sys

    dry_run = False
    if len(sys.argv) >= 2 and sys.argv[1] == "--dry-run":
        print("dry run enabled")
        dry_run = True

    format_files(dry_run)


if __name__ == "__main__":
    main()
