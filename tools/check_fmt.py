#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Two questions about the formatter, asked of every source in the tree.

The first is the gate a formatter owes its tree: every .al file is
already in its canonical layout, so a diff after alcy fmt is a review
decision rather than a formatting accident. alcy fmt --check answers
it, and one run covers many files.

The second is the property the formatter is built around, asked of the
whole corpus rather than of a sample. Formatting formatted output
changes nothing, and the compiler's verdict about a file survives its
formatting: a formatter that broke either would turn a no-op run into a
review, or worse, a program that stopped checking. Idempotence is
asked with --check over the formatted copies, which is the same
comparison and names every file that fails it; the verdict is asked by
checking each file before and after.

The corpus is the one the grammar's differential check reads, so the
two halves of the tree agree about what a source file is.
"""

import argparse
import concurrent.futures
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from utils.env import run_environment
from utils.paths import project_root_dir

SOURCE_ROOTS = ["lib", "e2e", "exe", "samples", "benchmarks"]

# One command line per this many paths: the corpus is a few hundred
# files, and a command line carries only so much of it at once.
CHUNK = 64

FMT_NOT_CLEAN = 8


def alcy_binary(build_subdir: str) -> Path:
    alcy = project_root_dir / "out" / build_subdir / "alcy"
    if not alcy.is_file():
        alcy = alcy.with_suffix(".exe")
    return alcy


def sources() -> list[Path]:
    return sorted(
        path
        for root in SOURCE_ROOTS
        for path in (project_root_dir / root).rglob("*.al")
    )


def run(
    alcy: Path, *args: str, stdin: bytes | None = None
) -> subprocess.CompletedProcess:
    return subprocess.run(
        [str(alcy), *args],
        capture_output=True,
        input=stdin,
        env=run_environment(),
    )


def unformatted(alcy: Path, paths: list[Path]) -> list[str]:
    """The paths whose layout would change, in as few runs as possible.

    --check reports every file it was handed, so the corpus is asked in
    chunks rather than one file at a time.
    """
    names: list[str] = []
    for start in range(0, len(paths), CHUNK):
        chunk = paths[start : start + CHUNK]
        result = run(
            alcy,
            "fmt",
            "--check",
            *[flag for path in chunk for flag in ("--file", str(path))],
        )
        if result.returncode not in (0, FMT_NOT_CLEAN):
            print(result.stderr.decode("utf-8", "replace"), file=sys.stderr)
            sys.exit("error: alcy fmt --check failed on the corpus")
        names.extend(
            line
            for line in result.stderr.decode("utf-8", "replace").splitlines()
            if "needs formatting" in line
        )
    return names


def verdict(alcy: Path, path: Path) -> int:
    return run(alcy, "check", "--file", str(path)).returncode


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check the formatter over every source in the tree."
    )
    parser.add_argument(
        "--build-subdir",
        default="build",
        help="Subdirectory inside out/ holding the alcy binary",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Name every file as it is checked, not only the failures",
    )
    args = parser.parse_args()

    alcy = alcy_binary(args.build_subdir)
    if not alcy.is_file():
        print(f"alcy binary not found in out/{args.build_subdir}/")
        return 1
    corpus = sources()
    if not corpus:
        print("no sources found under " + ", ".join(SOURCE_ROOTS))
        return 1

    # The layout gate: the tree as it stands is what fmt would write.
    needs_formatting = unformatted(alcy, corpus)

    # The property sweep: format every file once, then ask --check of
    # the copies (idempotence) and the compiler of both sides (the
    # verdict). The copies mirror the tree so a failure names the file
    # the way the tree spells it. A file's work is its own three
    # compiler invocations, so the pool waits on subprocesses rather
    # than holding the interpreter, and the results are read in the
    # corpus order the files were handed out in.
    jobs = min(os.cpu_count() or 2, 8)

    def swept(path: Path):
        formatted = run(alcy, "fmt", "--stdin", stdin=path.read_bytes())
        if formatted.returncode != 0:
            sys.exit(
                f"error: alcy fmt failed on {path}\n"
                + formatted.stderr.decode("utf-8", "replace")
            )
        return formatted.stdout

    def checked(path: Path, copy: Path):
        return verdict(alcy, path), verdict(alcy, copy)

    moved = []
    verdict_changed = []
    with tempfile.TemporaryDirectory(prefix="alcy_fmt_sweep_") as tmp:
        mirror = Path(tmp)
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            formatted = list(pool.map(swept, corpus))
        for path, text in zip(corpus, formatted, strict=True):
            copy = mirror / path.relative_to(project_root_dir)
            copy.parent.mkdir(parents=True, exist_ok=True)
            copy.write_bytes(text)
            if args.verbose:
                print(f"formatted {path.relative_to(project_root_dir)}")
        copies = [mirror / path.relative_to(project_root_dir) for path in corpus]
        moved = unformatted(alcy, copies)
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            verdicts = list(pool.map(checked, corpus, copies))
        for path, (before, after) in zip(corpus, verdicts, strict=True):
            if before != after:
                verdict_changed.append(
                    f"{path.relative_to(project_root_dir)}: "
                    f"verdict {before} became {after}"
                )
            if args.verbose:
                print(f"checked {path.relative_to(project_root_dir)}")

    failed = False
    if needs_formatting:
        print("\nthe tree is not formatted; alcy fmt writes these:")
        for line in needs_formatting:
            print(f"  - {line}")
        failed = True
    if moved:
        print("\nformatting is not idempotent; a second pass moves these:")
        for line in moved:
            print(f"  - {line}")
        failed = True
    if verdict_changed:
        print("\nformatting changed what the compiler says about these:")
        for line in verdict_changed:
            print(f"  - {line}")
        failed = True
    if failed:
        return 1
    print(f"fmt: {len(corpus)} sources formatted, idempotent, unchanged in verdict")
    return 0


if __name__ == "__main__":
    sys.exit(main())
