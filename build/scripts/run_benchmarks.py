#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Process integration benchmarks over the real alcy binary.

Times `alcy` as a process, so a number here includes startup, argument
parsing, the filesystem, and the linker. That is what a user waits for,
which is the reason this runner is separate from the in-process
microbenchmark engine: the two answer different questions and their
numbers must not be averaged together.

Cases are declared in benchmarks/suites/cases.toml and run a fixture
directory under benchmarks/fixtures/. The command is an argument vector,
never a shell string.

Results are written as JSONL, one record per case, keeping the raw
samples: an aggregate whose samples were discarded cannot be checked
against a re-run.

    uv run ./build/scripts/run_benchmarks.py
    uv run ./build/scripts/run_benchmarks.py --cases compile/executable
    uv run ./build/scripts/run_benchmarks.py --compare before.jsonl after.jsonl
"""

import argparse
import hashlib
import json
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import tomllib
from pathlib import Path

from utils.paths import project_root_dir

SCHEMA_VERSION = 1


class SuiteError(Exception):
    """A suite file that does not describe a runnable case set."""


def load_suite(path: Path) -> list[dict]:
    with open(path, "rb") as f:
        data = tomllib.load(f)
    version = data.get("schema_version")
    if version != SCHEMA_VERSION:
        raise SuiteError(f"{path}: schema_version {version!r} is not {SCHEMA_VERSION}")
    cases = data.get("case")
    if not isinstance(cases, list) or not cases:
        raise SuiteError(f"{path}: no cases declared")
    for case in cases:
        for key in ("id", "fixture", "argv", "expected_exit"):
            if key not in case:
                raise SuiteError(f"{path}: case is missing {key!r}: {case}")
        if not isinstance(case["argv"], list) or not all(
            isinstance(a, str) for a in case["argv"]
        ):
            raise SuiteError(f"{path}: {case['id']}: argv must be a list of strings")
    return cases


def fixture_digest(fixture_dir: Path) -> str:
    """Digest of every file's path and bytes, in a stable order.

    Recorded with the result so a comparison can refuse to line up two
    runs whose inputs were not the same inputs.
    """
    digest = hashlib.sha256()
    for path in sorted(p for p in fixture_dir.rglob("*") if p.is_file()):
        digest.update(path.relative_to(fixture_dir).as_posix().encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()[:16]


def resolve_binary(build_subdir: str) -> Path:
    binary = project_root_dir / "out" / build_subdir / "alcy"
    if not binary.is_file():
        binary = binary.with_suffix(".exe")
    if not binary.is_file():
        raise SuiteError(f"alcy binary not found in out/{build_subdir}/")
    return binary


def prepare_workspace(case: dict, fixtures_dir: Path, work_dir: Path) -> Path:
    """A private copy of the fixture, so nothing leaks between runs.

    A case that writes its output into the fixture would otherwise leave
    a file that changes what the next repetition measures.
    """
    source = fixtures_dir / case["fixture"]
    if not source.is_dir():
        raise SuiteError(f"fixture not found: {source}")
    target = work_dir / case["id"].replace("/", "_")
    shutil.copytree(source, target)
    (target / "out").mkdir(exist_ok=True)
    return target


def run_once(argv: list[str], cwd: Path) -> tuple[float, int]:
    start = time.perf_counter_ns()
    proc = subprocess.run(
        argv, cwd=cwd, capture_output=True, text=True, encoding="utf-8"
    )
    elapsed = time.perf_counter_ns() - start
    return elapsed, proc.returncode


def summarize(samples_ns: list[int]) -> dict:
    ordered = sorted(samples_ns)
    return {
        "count": len(ordered),
        "min": ordered[0],
        "p50": int(statistics.median(ordered)),
        "mean": round(statistics.fmean(ordered), 1),
        "max": ordered[-1],
        "stddev": round(statistics.stdev(ordered), 1) if len(ordered) > 1 else 0.0,
    }


def measure(
    case: dict, binary: Path, fixtures_dir: Path, build_subdir: str
) -> dict:
    argv = [str(binary), *case["argv"]]
    repetitions = int(case.get("repetitions", 10))
    warmup = int(case.get("warmup", 2))

    with tempfile.TemporaryDirectory(prefix="alcy_bench_") as scratch:
        work_dir = Path(scratch)
        # The digest is of the checked-in fixture, not of the scratch copy:
        # it identifies the input, and the copy is byte-identical to it.
        digest = fixture_digest(fixtures_dir / case["fixture"])
        cwd = prepare_workspace(case, fixtures_dir, work_dir)

        for _ in range(warmup):
            run_once(argv, cwd)

        samples_ns: list[int] = []
        for _ in range(repetitions):
            elapsed, code = run_once(argv, cwd)
            if code != case["expected_exit"]:
                raise SuiteError(
                    f"{case['id']}: exit {code}, want {case['expected_exit']}"
                )
            samples_ns.append(elapsed)

    record = {
        "schema_version": SCHEMA_VERSION,
        "benchmark_id": case["id"],
        "kind": "process",
        # The arguments, not the path of the binary that ran them: two
        # builds of the same tree differ in that path, and comparing
        # them is the point.
        "command": list(case["argv"]),
        "fixture_digest": digest,
        "repetitions": repetitions,
        "warmup": warmup,
        "samples_ns": samples_ns,
        "stats": summarize(samples_ns),
    }
    record.update(environment(binary, build_subdir))
    return record


def environment(binary: Path, build_subdir: str) -> dict:
    """What the numbers are only comparable within.

    A shared CI runner is not a stable measurement environment, so the
    comparison refuses records that disagree rather than averaging them.
    """
    return {
        "target": f"{platform.system().lower()}-{platform.machine()}",
        "build_subdir": build_subdir,
        "binary_bytes": binary.stat().st_size,
    }


def load_jsonl(path: Path) -> list[dict]:
    records = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                records.append(json.loads(line))
    return records


# The fields that must agree before two numbers describe the same work.
# Anything not listed here (samples, statistics) is what is compared.
# The build directory is one of them: a debug binary and a release binary
# are two different programs, so their timings are not a before and after.
COMPARABLE = (
    "benchmark_id",
    "fixture_digest",
    "command",
    "target",
    "build_subdir",
)


def compare(before_path: Path, after_path: Path) -> int:
    before = {r["benchmark_id"]: r for r in load_jsonl(before_path)}
    after = {r["benchmark_id"]: r for r in load_jsonl(after_path)}

    removed = sorted(set(before) - set(after))
    added = sorted(set(after) - set(before))
    for name in removed:
        print(f"REMOVED {name}")
    for name in added:
        print(f"ADDED   {name}")

    rows = 0
    for name in sorted(set(before) & set(after)):
        old, new = before[name], after[name]
        mismatch = [f for f in COMPARABLE if old.get(f) != new.get(f)]
        if mismatch:
            print(f"SKIP    {name}: differs in {', '.join(mismatch)}")
            continue
        old_p50 = old["stats"]["p50"]
        new_p50 = new["stats"]["p50"]
        delta = 100.0 * (new_p50 - old_p50) / old_p50 if old_p50 else 0.0
        print(
            f"{name}: p50 {old_p50 / 1e6:.2f}ms -> {new_p50 / 1e6:.2f}ms "
            f"({delta:+.1f}%)"
        )
        rows += 1

    if not rows:
        print("compare: no comparable cases")
        return 1
    return 0


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(
        description="Run process integration benchmarks over the alcy binary."
    )
    parser.add_argument(
        "--build-subdir",
        default="build",
        help="Subdirectory inside out/ holding the alcy binary (default: build)",
    )
    parser.add_argument(
        "--suite",
        default="benchmarks/suites/cases.toml",
        help="Suite file, relative to the project root",
    )
    parser.add_argument(
        "--fixtures",
        default="benchmarks/fixtures",
        help="Fixture root, relative to the project root",
    )
    parser.add_argument(
        "--cases",
        default="",
        help="Comma-separated case ids to run (default: all)",
    )
    parser.add_argument("--output", default="", help="JSONL file to append to")
    parser.add_argument(
        "--compare",
        nargs=2,
        metavar=("BEFORE", "AFTER"),
        help="Compare two result files instead of running cases",
    )
    args = parser.parse_args()

    if args.compare:
        return compare(Path(args.compare[0]), Path(args.compare[1]))

    try:
        suite = load_suite(project_root_dir / args.suite)
        binary = resolve_binary(args.build_subdir)
    except SuiteError as e:
        print(f"error: {e}")
        return -1

    selected = (
        {name.strip() for name in args.cases.split(",") if name.strip()}
        if args.cases
        else None
    )
    cases = [c for c in suite if selected is None or c["id"] in selected]
    if not cases:
        print(f"error: no case matched {args.cases!r}")
        return -1

    fixtures_dir = project_root_dir / args.fixtures
    records = []
    for case in cases:
        try:
            record = measure(case, binary, fixtures_dir, args.build_subdir)
        except SuiteError as e:
            print(f"error: {e}")
            return -1
        records.append(record)
        stats = record["stats"]
        print(
            f"{record['benchmark_id']}: p50 {stats['p50'] / 1e6:.2f}ms "
            f"min {stats['min'] / 1e6:.2f}ms over {stats['count']} runs"
        )

    if args.output:
        path = Path(args.output)
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "a", encoding="utf-8") as f:
            for record in records:
                f.write(json.dumps(record) + "\n")
        print(f"wrote {len(records)} record(s) to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
