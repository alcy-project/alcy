#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Ratchets source coverage of the compiler's own code.

A fixed threshold is the wrong shape for this repository: a module that
is half covered because it is mostly error paths is not a defect, and a
percentage target would push effort away from the code that matters. So
the gate is a ratchet. `build/coverage_baseline.json` records the line
coverage the tree has reached, a run that lands below it fails, and the
report names the least covered modules so the next test goes where the
gap is.

All three suites contribute, because measuring the unit tests alone
understates the tree badly: the lowering pass and the emitter are
exercised end to end by the exe cases rather than by unit tests, and a
report that called them untested would send the next test to the wrong
place.

    ./tools/check_coverage.py            build, run, gate
    ./tools/check_coverage.py --no-build reuse the last build
    ./tools/check_coverage.py --report   print the report, no gate
    ./tools/check_coverage.py --update   record the current numbers

Only `compiler/` counts. The vendored dependencies are compiled into the test
binary and would otherwise dominate the total, and a number that mostly
measures toml++ says nothing about this compiler.

A ratchet that anyone can move with --update is not a ratchet, so
--update is a deliberate act: it is the commit that says coverage is not
allowed to go back down from here.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from utils.paths import project_root_dir

REPO_ROOT = project_root_dir
TOOLS = REPO_ROOT / "tools"
BASELINE = REPO_ROOT / "build" / "coverage_baseline.json"
BUILD_SUBDIR = "coverage"
OUT_DIR = REPO_ROOT / "out" / BUILD_SUBDIR
TEST_BINARY = OUT_DIR / "tests"
ALCY_BINARY = OUT_DIR / "alcy"

# The suites that run the compiler, in the order they are reported.
SUITES = (
    ("unit tests", [str(TEST_BINARY)]),
    (
        "e2e cases",
        ["uv", "run", str(TOOLS / "check_e2e.py"), f"--build-subdir={BUILD_SUBDIR}"],
    ),
    (
        "exe cases",
        ["uv", "run", str(TOOLS / "check_exe.py"), f"--build-subdir={BUILD_SUBDIR}"],
    ),
)

# Third-party headers and the build tree are compiled into the binary but
# are not this project's code.
IGNORED = ("third_party/", "/out/", "/lib/", "/usr/")

# Line coverage moves by less than this between runs on the same code;
# the counts come from a counter merge, so they are not bit-reproducible.
TOLERANCE = 0.05


def build() -> int:
    """Builds instrumented copies of the tests and the compiler.

    The flags are clang-only and slow the build down, so the instrumented
    binaries live beside the normal ones rather than replacing them. The
    default target covers both: `tests` for the unit suites, `alcy` for
    the cases that drive the compiler.
    """
    command = [
        "uv",
        "run",
        str(TOOLS / "build.py"),
        f"--build-subdir={BUILD_SUBDIR}",
        "--gn-arg=is_coverage=true",
    ]
    # Both targets, in the order they are measured: the default target is
    # the compiler alone, so asking for it does not build the tests and a
    # stale test binary would silently report the old numbers.
    for target in ("tests", "default"):
        step = command + [f"--target={target}"]
        if subprocess.run(step, cwd=REPO_ROOT).returncode != 0:
            return 1
    return 0


def measure(verbose: bool = False) -> dict:
    """Runs every suite and returns line coverage for `compiler/`, by module."""
    for binary in (TEST_BINARY, ALCY_BINARY):
        if not binary.is_file():
            sys.exit(f"{binary} is missing; run without --no-build first")

    with tempfile.TemporaryDirectory(prefix="alcy_coverage_") as scratch:
        # %p keeps a suite that forks from overwriting another's profile.
        profile = str(Path(scratch) / "tests-%p.profraw")
        merged = Path(scratch) / "merged.profdata"
        exported = Path(scratch) / "coverage.json"
        env = dict(os.environ, LLVM_PROFILE_FILE=profile)

        failed_suites = []
        for name, command in SUITES:
            if verbose:
                print(f"==> {name}")
            # A failing case still produced a profile, and the profile is
            # the point here, so a suite's own failure is left to the suite.
            # A process that dies before writing its counters is a different
            # fault: its profile is corrupt, and the merge below then fails
            # naming llvm-profdata instead of the process that caused it.
            result = subprocess.run(
                command,
                cwd=REPO_ROOT,
                env=env,
                stdout=None if verbose else subprocess.DEVNULL,
                stderr=subprocess.STDOUT if not verbose else None,
            )
            if result.returncode != 0:
                failed_suites.append(f"{name} (exit {result.returncode})")
        if failed_suites:
            print(
                "warning: suites exited non-zero: " + ", ".join(failed_suites),
                file=sys.stderr,
            )

        raw = sorted(Path(scratch).glob("*.profraw"))
        if not raw:
            sys.exit("the suites produced no profile; was the build instrumented?")
        if verbose:
            print(f"merged {len(raw)} profile(s)")

        merge = subprocess.run(
            [
                "llvm-profdata",
                "merge",
                "-sparse",
                *(str(path) for path in raw),
                "-o",
                str(merged),
            ],
            capture_output=True,
            text=True,
            cwd=REPO_ROOT,
        )
        if merge.returncode != 0:
            sys.exit(
                f"llvm-profdata could not merge {len(raw)} profile(s).\n"
                + (merge.stderr.strip() or merge.stdout.strip())
                + "\n\nA process that died while writing its profile leaves one "
                "with no counters, and reusing an instrumented build directory "
                "is how a binary starts doing that: rebuild it with "
                f"`rm -rf {OUT_DIR.relative_to(REPO_ROOT)}` and run again."
                + (
                    "\nSuites that exited non-zero: " + ", ".join(failed_suites)
                    if failed_suites
                    else ""
                )
            )
        with exported.open("wb") as out:
            subprocess.run(
                [
                    "llvm-cov",
                    "export",
                    str(TEST_BINARY),
                    f"-instr-profile={merged}",
                    f"-ignore-filename-regex={'|'.join(IGNORED)}",
                ],
                check=True,
                cwd=REPO_ROOT,
                stdout=out,
            )
        return summarize(json.loads(exported.read_text()))


def summarize(export: dict) -> dict:
    """Reduces the export to the numbers the gate and the report need."""
    files = export["data"][0]["files"]
    modules: dict[str, dict[str, int]] = {}
    total_count = 0
    total_covered = 0
    for entry in files:
        name = relative(entry["filename"])
        if name is None:
            continue
        lines = entry["summary"]["lines"]
        module = name.split("/")[1] if name.count("/") > 1 else name
        bucket = modules.setdefault(module, {"count": 0, "covered": 0})
        bucket["count"] += lines["count"]
        bucket["covered"] += lines["covered"]
        total_count += lines["count"]
        total_covered += lines["covered"]
    if total_count == 0:
        sys.exit("the export contained no source files under compiler/")
    return {
        "lines": {
            "count": total_count,
            "covered": total_covered,
            "percent": 100.0 * total_covered / total_count,
        },
        "modules": modules,
    }


def relative(filename: str) -> str | None:
    """Path relative to the repository, or None if it is not ours."""
    path = Path(filename)
    if not path.is_absolute():
        path = REPO_ROOT / path
    try:
        name = str(path.relative_to(REPO_ROOT))
    except ValueError:
        return None
    if not name.startswith("compiler/"):
        return None
    return name


def percent(counts: dict[str, int]) -> float:
    if counts["count"] == 0:
        return 100.0
    return 100.0 * counts["covered"] / counts["count"]


def report(current: dict, limit: int) -> None:
    """Prints the total, then the modules furthest from covered."""
    print(
        f"line coverage: {current['lines']['covered']}/"
        f"{current['lines']['count']} "
        f"({current['lines']['percent']:.2f}%)"
    )
    modules = sorted(current["modules"].items(), key=lambda kv: percent(kv[1]))
    width = max(len(name) for name, _ in modules)
    print("\nleast covered modules:")
    for name, counts in modules[:limit]:
        print(
            f"  {name:<{width}}  {counts['covered']:>5}/{counts['count']:<5}"
            f" {percent(counts):>6.2f}%"
        )


def gate(current: dict) -> int:
    """Fails when coverage lands below the recorded baseline."""
    if not BASELINE.is_file():
        sys.exit(f"{BASELINE} is missing. Record the current numbers with --update.")
    baseline = json.loads(BASELINE.read_text())
    was = baseline["lines"]["percent"]
    now = current["lines"]["percent"]
    report(current, limit=8)
    if now < was - TOLERANCE:
        print(
            f"\ncoverage fell from {was:.2f}% to {now:.2f}%, "
            f"{was - now:.2f} points below the baseline.",
            file=sys.stderr,
        )
        print(
            "Either cover the code that lost coverage, or record the new "
            "number with --update if the loss is deliberate.",
            file=sys.stderr,
        )
        return 1
    print(f"\nat or above the {was:.2f}% baseline")
    return 0


def write_baseline(current: dict) -> None:
    payload = {
        "comment": [
            "Recorded by tools/check_coverage.py --update.",
            "A ratchet: check_coverage.py fails when the measured line",
            "coverage of compiler/ drops below this. Move it only on purpose.",
        ],
        "lines": current["lines"],
        "modules": current["modules"],
    }
    BASELINE.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    print(f"wrote {BASELINE.relative_to(REPO_ROOT)}")
    report(current, limit=8)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="reuse the last instrumented build",
    )
    parser.add_argument(
        "--report",
        action="store_true",
        help="print the report without gating on the baseline",
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="record the measured coverage as the new baseline",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=8,
        help="modules to list in the report (default: 8)",
    )
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if shutil.which("llvm-profdata") is None or shutil.which("llvm-cov") is None:
        sys.exit("llvm-cov and llvm-profdata must be on PATH")

    if not args.no_build and build() != 0:
        sys.exit("build failed")

    current = measure(args.verbose)
    if args.update:
        write_baseline(current)
        return 0
    if args.report:
        report(current, args.limit)
        return 0
    return gate(current)


if __name__ == "__main__":
    sys.exit(main())
