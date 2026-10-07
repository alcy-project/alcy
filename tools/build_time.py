#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Reports where a build's time went, from ninja's own log.

Ninja writes one line per finished edge to `out/<subdir>/.ninja_log`, so the
answer needs no rebuild and no wrapper around the build. An output keeps its
most recent entry, which is what "how long does this object take" means after
several incremental builds. Durations are per edge and edges run in parallel,
so the total is CPU time rather than wall time.

    uv run ./tools/build_time.py
    uv run ./tools/build_time.py --top 30
    uv run ./tools/build_time.py --build-subdir build_wasm

Like measure_gates.py this is a report and never a gate: a slow translation
unit is a prompt to split it, not a build failure.
"""

import argparse
import os
import sys
from collections import defaultdict
from pathlib import Path

from utils.paths import out_dir


def read_edges(log_path: Path) -> dict[str, int]:
    """The most recent duration, in milliseconds, of every output."""
    durations: dict[str, int] = {}
    for line in log_path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) < 4:
            continue
        try:
            start = int(fields[0])
            end = int(fields[1])
        except ValueError:
            continue
        durations[fields[3]] = max(0, end - start)
    return durations


def module_of(output: str) -> str:
    # `obj/compiler/analyzer/analyzer/types.o` names the analyzer module;
    # the second component is the target the object was built into.
    parts = Path(output).parts
    if len(parts) >= 3 and parts[0] == "obj":
        return f"{parts[1]}/{parts[2]}"
    return "other"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Report where a build's time went, from ninja's log."
    )
    parser.add_argument(
        "--build-subdir",
        default="build",
        help="Subdirectory inside out/ (default: build)",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=20,
        help="How many slowest translation units to list (default: 20)",
    )
    args = parser.parse_args()

    log_path = out_dir / args.build_subdir / ".ninja_log"
    if not log_path.is_file():
        print(f"No ninja log at {log_path}", file=sys.stderr)
        return 1

    edges = read_edges(log_path)
    if not edges:
        print(f"{log_path} holds no finished edges")
        return 0

    compiles = {out: ms for out, ms in edges.items() if out.endswith((".o", ".obj"))}
    other = {out: ms for out, ms in edges.items() if out not in compiles}

    print(f"build_time: {log_path} on {os.cpu_count() or '?'} cores")

    total = sum(compiles.values()) / 1000
    print(f"\ncompile edges: {len(compiles)}, CPU {total:.1f} s")
    slowest = sorted(compiles.items(), key=lambda item: item[1], reverse=True)
    for rank, (output, ms) in enumerate(slowest[: args.top], start=1):
        print(f"  {rank:3}. {ms / 1000:7.1f} s  {output}")

    modules: dict[str, int] = defaultdict(int)
    for output, ms in compiles.items():
        modules[module_of(output)] += ms
    print("\nby module (CPU):")
    for module, ms in sorted(modules.items(), key=lambda item: item[1], reverse=True):
        print(f"  {ms / 1000:7.1f} s  {module}")

    print(f"\nother edges: {len(other)}, CPU {sum(other.values()) / 1000:.1f} s")
    for output, ms in sorted(other.items(), key=lambda item: item[1], reverse=True)[:5]:
        print(f"  {ms / 1000:7.1f} s  {output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted by user. Exiting immediately...", file=sys.stderr)
        os._exit(130)
