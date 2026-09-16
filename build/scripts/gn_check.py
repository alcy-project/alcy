#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

import subprocess
import json
import os
import sys

from pathlib import Path
from utils.source import source_extensions


def check_sources(out_dir: Path, source_root_dir: Path):
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
                gn_sources.add(os.path.abspath(normalized))

    # Walk source directory to find unlisted source files
    disk_sources = set()
    exts = tuple(source_extensions)
    for root, _, files in os.walk(source_root_dir):
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
        default_out_dir,
        src_dir,
    )

    out_dir = default_out_dir
    source_root_dir = src_dir
    return check_sources(out_dir, source_root_dir)


if __name__ == "__main__":
    sys.exit(main())
