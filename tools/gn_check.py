#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import json
import os
import subprocess
import sys
from pathlib import Path

from utils.source import source_extensions


def check_sources(
    out_dir: Path, source_root_dir: Path, excluded_dirs: list[str] | None = None
):
    excluded = frozenset(excluded_dirs or ())
    # Get list of all tracked source files from GN description
    cmd = ["gn", "desc", out_dir, "*", "sources", "--format=json"]
    result = subprocess.run(cmd, capture_output=True, text=True, check=True)
    data = json.loads(result.stdout)

    gn_sources = set()
    for _, target_info in data.items():
        sources = target_info.get("sources", [])
        for src in sources:
            # Normalize path representation
            normalized = src.replace("//", "")
            ignore = False
            if "third_party" in normalized:
                ignore = True

            if not ignore:
                # `gn desc` emits repo-relative paths; the disk side is
                # absolute, so the comparison has to be too, anchored at
                # the repository rather than at the caller's directory.
                repository = Path(__file__).resolve().parent.parent
                gn_sources.add(str((repository / normalized).resolve()))

    # Walk source directory to find unlisted source files
    disk_sources = set()
    exts = tuple(source_extensions)
    for root, dirs, files in os.walk(source_root_dir):
        dirs[:] = [d for d in dirs if d not in excluded]
        for file in files:
            if file.endswith(exts):
                disk_sources.add(os.path.abspath(os.path.join(root, file)))

    unadded = disk_sources - gn_sources
    if len(unadded) > 0:
        relative = [os.path.relpath(f, source_root_dir) for f in unadded]
        print(
            "Files on disk not added to any GN target:", *sorted(relative), sep="\n  "
        )
        return len(unadded)
    return 0


def main():
    from utils.paths import (
        compiler_dir,
        default_out_dir,
        excluded_source_dirs,
    )

    out_dir = default_out_dir
    return check_sources(out_dir, compiler_dir, excluded_source_dirs)


if __name__ == "__main__":
    sys.exit(main())
