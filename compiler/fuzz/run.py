#!/usr/bin/env python3
# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Builds and drives the libFuzzer targets under //fuzz.

The targets themselves carry no CLI: each one is a plain libFuzzer
binary. This wraps them so the common cases need no recall of libFuzzer
flags and so a crash is classified against fuzz/known before it is
reported.

    ./fuzz/run.py                      build, then 30s per target
    ./fuzz/run.py --seconds 300        a longer soak
    ./fuzz/run.py fuzz_render          one target
    ./fuzz/run.py --replay <path>...   run inputs or corpus dirs once
    ./fuzz/run.py --list               the targets and their entry points

A target's own exit status is not the signal: libFuzzer exits 0 after a
run that found a crash it already knows how to minimise. The signal is
the artifact set, which is why artifacts go to a scratch directory and
are classified by content.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_SUBDIR = "fuzz"
# libFuzzer grows this without bound; it is a cache for the next run, not
# an artifact.
CORPUS_DIR = REPO_ROOT / "fuzz" / "corpus"
KNOWN_DIR = REPO_ROOT / "fuzz" / "known"

# Kept in step with //fuzz/BUILD.gn. Each entry names the byte-level
# boundary a target drives; see the comment at the top of that file for
# what the oracle is in each case.
TARGETS = {
    "fuzz_lexer": "bytes -> lex -> verify_token_stream",
    "fuzz_parser": "bytes -> lex -> parse -> verify_file",
    "fuzz_path": "two path spellings -> from_native / join agreement",
    "fuzz_manifest": "bytes -> toml++ -> parse_manifest -> verify_manifest",
    "fuzz_render": "span -> render -> header and caret column agree",
    "fuzz_pipeline": "bytes -> the whole check pipeline",
}


def build(verbose: bool = False) -> int:
    """Builds every target into its own out/ subdirectory.

    Fuzz targets carry -fsanitize=fuzzer, which nothing else in the tree
    wants, so they get a separate GN output directory rather than
    disturbing the compiler build.
    """
    script = REPO_ROOT / "build" / "scripts" / "build.py"
    command = [
        "uv",
        "run",
        str(script),
        "--target=fuzz",
        f"--build-subdir={BUILD_SUBDIR}",
        "--gn-arg=is_fuzz=true",
    ]
    if verbose:
        command.append("--verbose")
    return subprocess.run(command, cwd=REPO_ROOT).returncode


def binary(name: str) -> Path:
    """Path to a built target, or an error naming how to get one."""
    out = REPO_ROOT / "out" / BUILD_SUBDIR / name
    if not out.is_file():
        sys.exit(f"{out} is missing; run ./fuzz/run.py --build first")
    return out


def load_known() -> tuple[dict[str, list[dict]], dict[str, dict]]:
    """Known failures, as (inputs keyed by hash, targets by name).

    An input entry is keyed on the bytes alone rather than on the target:
    the same input failing twice is the same defect, and a hash is what an
    artifact filename gives you to work with. A target entry covers a
    whole class of inputs, which is the honest way to record a
    dependency that fails on many inputs.
    """
    manifest = KNOWN_DIR / "crashes.json"
    if not manifest.is_file():
        return {}, {}
    data = json.loads(manifest.read_text())
    inputs: dict[str, list[dict]] = {}
    for entry in data.get("inputs", []):
        inputs.setdefault(entry["input"], []).append(entry)
    return inputs, {entry["target"]: entry for entry in data.get("targets", [])}


def classify(directory: Path, known: dict[str, list[dict]]) -> tuple[list, list]:
    """Splits libFuzzer artifacts into expected and unexpected.

    An artifact is named crash-<hash> by libFuzzer, but the content is
    what decides: the name is its own hash function, and the allowlist is
    keyed on sha256.
    """
    expected: list[tuple[Path, list[dict]]] = []
    unexpected: list[Path] = []
    for artifact in sorted(directory.glob("crash-*")):
        digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        if digest in known:
            expected.append((artifact, known[digest]))
        else:
            unexpected.append(artifact)
    return expected, unexpected


def describe(entry: dict) -> str:
    where = entry.get("target", "any target")
    return f"{where}: {entry['summary']}"


def run_fuzzing(
    names: list[str], seconds: int, rss_mb: int, jobs: int, verbose: bool
) -> int:
    """Runs each target for a time budget and classifies what it found."""
    known, disabled = load_known()
    failures = 0
    for name in names:
        if name in disabled:
            # Reported, not counted and not passed: a gate that cannot
            # discriminate is worse than no gate, and quietly turning it
            # green would hide that.
            print(f"==> {name} DISABLED: {disabled[name]['summary']}")
            print(
                f"    {name} cannot be a gate until this is fixed; see "
                "fuzz/known/README.md"
            )
            continue
        corpus = CORPUS_DIR / name
        corpus.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="alcy_fuzz_") as scratch:
            artifacts = Path(scratch)
            command = [
                str(binary(name)),
                f"-max_total_time={seconds}",
                f"-rss_limit_mb={rss_mb}",
                f"-artifact_prefix={artifacts}/",
                f"-jobs={jobs}",
                f"-workers={jobs}",
                str(corpus.resolve()),
            ]
            if not verbose:
                command.append("-print_final_stats=0")
            print(f"==> {name} ({seconds}s, {jobs} job(s))", flush=True)
            completed = subprocess.run(
                command,
                # libFuzzer writes fuzz-<job>.log into the working
                # directory under -jobs, so the run belongs in the
                # scratch directory rather than the repository.
                cwd=artifacts,
                stdout=subprocess.DEVNULL if not verbose else None,
                stderr=subprocess.STDOUT if not verbose else None,
            )
            expected, unexpected = classify(artifacts, known)
            for artifact, entries in expected:
                print(f"    known failure, not counted: {describe(entries[0])}")
                print(f"      {artifact.name}")
            if unexpected:
                failures += len(unexpected)
                for artifact in unexpected:
                    print(f"    UNEXPECTED CRASH: {artifact}", file=sys.stderr)
                    print(
                        f"      bytes: {artifact.read_bytes()!r}", file=sys.stderr
                    )
                print(
                    f"    reproduce: {binary(name)} {unexpected[0]}",
                    file=sys.stderr,
                )
                print(
                    "    a crash in compiler/ is a bug: reduce it to a unit test "
                    "under compiler/tests. See compiler/fuzz/known/README.md.",
                    file=sys.stderr,
                )
            else:
                print("    no unexpected crashes")
        if completed.returncode != 0 and not unexpected:
            # A non-zero status with no artifact is a target that could
            # not start, or an out-of-memory kill.
            failures += 1
            print(
                f"    target exited {completed.returncode} without an "
                "artifact",
                file=sys.stderr,
            )
    return failures


def replay(paths: list[str]) -> int:
    """Runs inputs or corpus directories once each, through every target.

    A checked-in reproducer should be replayable without a fuzzing run,
    and replaying the unit-test sources is the cheapest way to see
    whether a fix actually fixed something.
    """
    failures = 0
    for name in TARGETS:
        for target_path in paths:
            print(f"==> {name} {target_path}", flush=True)
            completed = subprocess.run(
                [str(binary(name)), str(target_path)],
                cwd=REPO_ROOT,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.STDOUT,
            )
            if completed.returncode != 0:
                failures += 1
                print(
                    f"    exited {completed.returncode}", file=sys.stderr
                )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "targets",
        nargs="*",
        metavar="TARGET",
        help="targets to run (default: all)",
    )
    parser.add_argument(
        "--build", action="store_true", help="build the targets and exit"
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="skip the build step (default: build first)",
    )
    parser.add_argument(
        "--seconds",
        type=int,
        default=30,
        help="seconds per target (default: 30)",
    )
    parser.add_argument(
        "--rss-mb", type=int, default=4096, help="RSS limit (default: 4096)"
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=max(1, (os.cpu_count() or 2) // 2),
        help="parallel jobs per target (default: half the cores)",
    )
    parser.add_argument(
        "--replay",
        nargs="+",
        metavar="PATH",
        help="replay inputs or corpus directories once, then exit",
    )
    parser.add_argument(
        "--list", action="store_true", help="list the targets and exit"
    )
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if args.list:
        width = max(len(name) for name in TARGETS)
        for name, boundary in sorted(TARGETS.items()):
            print(f"{name:<{width}}  {boundary}")
        return 0

    unknown = [name for name in args.targets if name not in TARGETS]
    if unknown:
        sys.exit(f"unknown target(s): {', '.join(unknown)}")

    if args.build:
        return build(args.verbose)

    if not args.no_build and build(args.verbose) != 0:
        sys.exit("build failed")

    if args.replay:
        return 1 if replay(args.replay) else 0

    names = args.targets or list(TARGETS)
    if os.environ.get("ALCY_FUZZ_RESET"):
        shutil.rmtree(CORPUS_DIR, ignore_errors=True)
    failures = run_fuzzing(
        names, args.seconds, args.rss_mb, args.jobs, args.verbose
    )
    if failures:
        print(f"\n{failures} unexpected failure(s)", file=sys.stderr)
        return 1
    print("\nno unexpected crashes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
