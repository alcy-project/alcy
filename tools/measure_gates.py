#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Times every gate in the repository and reports each as a multiple of lint.

A repository that says in prose which check is slow is a repository whose
prose is wrong within a release. Two claims in this tree were: ADR-0017
called the coverage ratchet the slowest gate in `check.sh`, and
`CONTRIBUTING.md` repeated it. Lint is about 1.7x the ratchet, so both
were wrong by the largest factor they had a number for, and nothing
noticed because nothing measured the pair.

This measures the pair. It is a report and never a gate: ADR-0021 refuses
to gate on a timing because a shared runner is not a quiet machine, and
that reasoning applies to how long a gate took at least as much as it
applies to how fast the compiler is. A machine that reported a gate as
too slow would be a machine whose opinion changes with its load.

    uv run ./tools/measure_gates.py                  time every gate
    uv run ./tools/measure_gates.py --only lint     time one
    uv run ./tools/measure_gates.py --json           machine-readable

The figures depend on the machine, so the machine is part of the output
and the numbers are comparable to each other and to nothing else. The
ratios are the portable part, which is why the report is in multiples of
lint: lint is the largest gate, so it is the one that sets the scale.
"""

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import time

from utils.paths import project_root_dir

REPO_ROOT = project_root_dir

# The reference gate. Everything is reported against it because it is the
# largest, and a budget expressed as a multiple of the largest thing is
# the only form that survives being re-measured on another machine.
BASELINE = "lint"

# A gate is worth a CI job if it is no slower than this multiple of the
# reference. It was chosen when every gate in the tree measured well under
# one, so nothing is excluded by it today; it is here to be a decision
# rather than an accident if that ever stops being true.
BUDGET = 1.2

# (label, argv) in the order `check.sh` runs them, so the report reads as
# the gate's own ordering rather than as a sorted table. `fuzz` is not
# here: it is a development tool (ADR-0017), corpus-dependent, and would
# neither compare nor finish in a gate's time.
GATES = [
    ("typos", ["typos"]),
    ("format", ["uv", "run", "./tools/format.py", "--dry-run"]),
    ("check_spec", ["uv", "run", "./tools/check_spec.py"]),
    ("grammar", ["uv", "run", "./tools/check_treesitter.py", "--grammar"]),
    ("lint", ["uv", "run", "./tools/lint.py"]),
    (
        "tests",
        ["uv", "run", "./tools/run.py", "--target=tests", "--build-subdir=build"],
    ),
    ("e2e", ["uv", "run", "./tools/check_e2e.py", "--build-subdir=build"]),
    (
        "borrow_rules",
        ["uv", "run", "./tools/check_borrow_rules.py", "--build-subdir=build"],
    ),
    ("exe", ["uv", "run", "./tools/check_exe.py", "--build-subdir=build"]),
    (
        "exe samples",
        [
            "uv",
            "run",
            "./tools/check_exe.py",
            "--build-subdir=build",
            "--cases-root=samples",
        ],
    ),
    (
        "exe sanitized",
        ["uv", "run", "./tools/check_exe.py", "--build-subdir=build", "--sanitize"],
    ),
    (
        "static_linkage",
        ["uv", "run", "./tools/verify_static_linkage.py", "--build-dir=out/build"],
    ),
    (
        "grammar vs compiler",
        [
            "uv",
            "run",
            "./tools/check_treesitter.py",
            "--differential",
            "--build-subdir=build",
        ],
    ),
    ("coverage", ["uv", "run", "./tools/check_coverage.py"]),
    (
        "benchmark smoke",
        ["uv", "run", "./tools/run_benchmarks.py", "smoke", "--build-subdir=build"],
    ),
]


def machine() -> dict:
    return {
        "os": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        # The only number here that predicts a figure on another machine,
        # and even then only roughly.
        "cpus": os.cpu_count(),
    }


# Label to the tool whose absence stops it, and what to say about it.
# A gate that cannot run is reported rather than timed as zero: `typos` and
# the fuzzer's clang runtime are the two often missing, and a fast zero
# would read as a fast gate rather than as no gate.
NEEDS_TOOL = {
    "typos": ("typos", "typos is not on PATH"),
    "fuzz": ("clang", "clang is not on PATH, and the fuzz targets need it"),
    "coverage": ("llvm-cov", "llvm-cov is not on PATH"),
    "exe sanitized": (
        "clang",
        "clang is not on PATH, and the sanitized run refuses without it",
    ),
    "grammar": (
        "tree-sitter",
        "tree-sitter is not on PATH, and the grammar check refuses without it",
    ),
    "grammar vs compiler": (
        "tree-sitter",
        "tree-sitter is not on PATH, and the grammar check refuses without it",
    ),
}


def unrunnable(label: str) -> str:
    """Why a gate cannot run here, or an empty string if it can."""
    if label not in NEEDS_TOOL:
        return ""

    tool, why = NEEDS_TOOL[label]
    return why if shutil.which(tool) is None else ""


def time_one(label: str, argv: list[str]) -> dict:
    start = time.monotonic()
    proc = subprocess.run(argv, cwd=REPO_ROOT, capture_output=True, text=True)
    seconds = time.monotonic() - start

    # A gate that failed still took the time it took, and that is worth
    # knowing; but its figure does not describe the gate, so it is kept
    # out of the ratios.
    return {
        "label": label,
        "argv": argv,
        "seconds": round(seconds, 1),
        "returncode": proc.returncode,
        "ok": proc.returncode == 0,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--only", help="time one gate by label")
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit the report as JSON",
    )
    parser.add_argument(
        "--build",
        action="store_true",
        help="build first, so the figures include a cold tree rather than "
        "the last one's warmth",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    gates = [g for g in GATES if args.only is None or g[0] == args.only]
    if not gates:
        print(f"error: no gate named {args.only!r}", file=sys.stderr)
        return 1

    if args.build:
        subprocess.run(
            [
                "uv",
                "run",
                "./tools/build.py",
                "--target=all",
                "--mode=debug",
                "--build-subdir=build",
            ],
            cwd=REPO_ROOT,
            check=True,
        )

    results = []
    for label, argv in gates:
        why = unrunnable(label)
        if why:
            results.append({"label": label, "skipped": why})
            continue
        results.append(time_one(label, argv))

    # `--only` on something else than the reference leaves no baseline to
    # divide by, so the table prints figures without ratios rather than
    # inventing one.
    baseline = next(
        (
            r["seconds"]
            for r in results
            if r.get("label") == BASELINE and "seconds" in r
        ),
        None,
    )

    if args.json:
        print(
            json.dumps(
                {
                    "machine": machine(),
                    "budget": BUDGET,
                    "reference": BASELINE,
                    "gates": results,
                },
                indent=2,
            )
        )
        return 0

    info = machine()
    print(
        f"{info['os']} {info['machine']}, {info['cpus']} cpus. "
        f"Figures are this machine's; the ratios are the portable part.\n"
    )
    width = max(len(r["label"]) for r in results)
    print(f"{'gate'.ljust(width)}  {'seconds':>9}  {'x lint':>7}  verdict")
    for r in results:
        label = r["label"].ljust(width)
        if "skipped" in r:
            print(f"{label}  {'-':>9}  {'-':>7}  skipped: {r['skipped']}")
            continue
        note = "" if r["ok"] else "  FAILED, figure not comparable"
        if baseline is None:
            print(f"{label}  {r['seconds']:>9.1f}  {'-':>7}{note}")
            continue
        ratio = r["seconds"] / baseline
        verdict = "over budget" if ratio > BUDGET else "fits a job"
        if r["label"] == BASELINE:
            verdict = "the reference"
        print(f"{label}  {r['seconds']:>9.1f}  {ratio:>7.2f}  {verdict}{note}")

    if baseline is not None:
        print(
            f"\nThe reference is {BASELINE} at {baseline:.1f}s, so the "
            f"budget is {BUDGET * baseline:.0f}s."
        )
    print(
        "Nothing here is a gate. A timing that fails is a machine's opinion, "
        "and docs/adr/0021-benchmark-measurement.md is why a machine's "
        "opinion is not evidence."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
