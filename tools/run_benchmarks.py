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

    uv run ./tools/run_benchmarks.py
    uv run ./tools/run_benchmarks.py --cases compile/executable
    uv run ./tools/run_benchmarks.py --compare before.jsonl after.jsonl
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
    case: dict,
    binary: Path,
    fixtures_dir: Path,
    build_subdir: str,
    build_mode: str,
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
    record.update(environment(binary, build_subdir, build_mode))
    return record


def environment(binary: Path, build_subdir: str, build_mode: str) -> dict:
    """What the numbers are only comparable within.

    A shared CI runner is not a stable measurement environment, so the
    comparison refuses records that disagree rather than averaging them.
    """
    return {
        "target": f"{platform.system().lower()}-{platform.machine()}",
        "build_subdir": build_subdir,
        "build_mode": build_mode,
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
    "build_mode",
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


# The microbenchmark driver, a separate binary: it links the compiler as
# a library and never the cli, so what it times has no process boundary
# and no argument parsing in it.
def infer_build_mode(build_subdir: str) -> str:
    """release or debug, as the build directory recorded it.

    The directory's name says nothing: the CI matrix builds both a debug
    and a release compiler into `out/build` and distinguishes them with
    `--mode`. `gn gen` wrote the flags that were actually used into
    `args.gn`, so that is what is asked. Guessing from the name got this
    wrong in the first CI run, and the run it got wrong measured a debug
    build's timings and called them a compiler's.
    """
    args = project_root_dir / "out" / build_subdir / "args.gn"
    try:
        text = args.read_text(encoding="utf-8")
    except OSError:
        return "unknown"
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("is_debug"):
            return "debug" if stripped.endswith("true") else "release"
    return "unknown"


def resolve_driver(build_subdir: str) -> Path:
    driver = project_root_dir / "out" / build_subdir / "benchmarks"
    if not driver.is_file():
        driver = driver.with_suffix(".exe")
    if not driver.is_file():
        raise SuiteError(f"benchmark driver not found in out/{build_subdir}/")
    return driver


def print_micro(record: dict) -> None:
    stats = record["stats"]
    print(
        f"{record['benchmark_id']}: p50 {stats['p50'] / 1e6:.3f}ms "
        f"p95 {stats['p95'] / 1e6:.3f}ms over {stats['count']} samples"
        f"{'' if record['confidence'] == 'ok' else ' (low confidence)'}"
    )


def run_micro(build_subdir: str, build_mode: str, output: str, cases: str) -> int:
    """Runs the engine over the cases it was compiled with.

    There is no suite file to read: a case is added to the driver, not to
    a manifest, because a micro case is a call into a module rather than
    a command line. The filter narrows a run without a rebuild.
    """
    try:
        driver = resolve_driver(build_subdir)
    except SuiteError as e:
        print(f"error: {e}")
        return -1

    target = Path(output) if output else None
    if target is not None:
        target.parent.mkdir(parents=True, exist_ok=True)
    argv = [str(driver), "--build-subdir", build_subdir, "--build-mode", build_mode]
    if cases:
        argv += ["--cases", cases]
    # The source is written out so a `check` run can be pointed at the
    # very input these figures came from, which is what the reconcile
    # step needs on both sides.
    if target is not None:
        argv += ["--emit-source", str(target.with_suffix(".al"))]
    proc = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr, file=sys.stderr)
        return -1

    records = [json.loads(line) for line in proc.stdout.splitlines() if line]
    if not records:
        print("error: the driver reported no cases")
        return -1
    if target is not None:
        with open(target, "a", encoding="utf-8") as f:
            for record in records:
                f.write(json.dumps(record) + "\n")
    for record in records:
        print_micro(record)
    if target is not None:
        print(f"wrote {len(records)} record(s) to {target}")
    return 0


# How far two runs of the same case may differ before the harness itself
# is suspect. A shared CI runner is noisy, so the band is wide; it exists
# to catch a broken clock, a missing warmup, or a batch that swallowed the
# work, not to measure the machine.
REPRODUCIBILITY_TOLERANCE = 0.25

# The width the engine runs at when nothing says otherwise. Large enough
# that a sample is far longer than the clock's own cost.
DEFAULT_WIDTH = 64


def phase_totals(trace_path: Path) -> dict[str, float]:
    """Each phase's total, in microseconds, as a Chromium trace records it.

    A trace event's `dur` is microseconds because that is the unit the
    format defines; the engine counts nanoseconds and is converted where
    the two meet, rather than either side being changed, so that the trace
    document stays pasteable into a viewer.
    """
    try:
        document = json.loads(trace_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise SuiteError(f"cannot read {trace_path}: {e}") from e
    totals: dict[str, float] = {}
    for event in document.get("traceEvents", []):
        totals[event["name"]] = totals.get(event["name"], 0.0) + event["dur"]
    return totals


def engine_run(
    driver: Path,
    build_subdir: str,
    build_mode: str,
    width: int,
    record_path: Path,
    source_path: Path,
) -> dict[str, dict]:
    """Runs the engine once and returns its cases by name."""
    done = subprocess.run(
        [
            str(driver),
            "--build-subdir",
            build_subdir,
            "--build-mode",
            build_mode,
            "--functions",
            str(width),
            "--output",
            str(record_path),
            "--emit-source",
            str(source_path),
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if done.returncode != 0:
        raise SuiteError(f"the engine failed: {done.stderr.strip()}")
    return {r["name"]: r for r in load_jsonl(record_path) if r["kind"] == "micro"}


def run_smoke(build_subdir: str) -> int:
    """Checks that the engine produced a full, well-formed result.

    This is the part of checking the harness that survives a machine
    nobody controls. It says nothing about how fast anything was: it
    looks at the shape of what came out, so a case that stopped being
    measured, a clock that read zero, a policy that collected nothing, or
    a generator that stopped producing a program the compiler accepts,
    all fail. A runner being noisy cannot make any of those true, and
    they cannot be told apart by a figure that is allowed to be wrong.

    The one judgement it makes is about the generated source, which is
    exact: the compiler has to accept it. A case measuring error
    recovery would otherwise look like a case measuring a parser.
    """
    try:
        driver = resolve_driver(build_subdir)
        binary = resolve_binary(build_subdir)
    except SuiteError as e:
        print(f"error: {e}")
        return -1

    with tempfile.TemporaryDirectory(prefix="alcy_smoke_") as scratch:
        work = Path(scratch)
        record_path = work / "smoke.jsonl"
        source_path = work / "bench.al"
        done = subprocess.run(
            [
                str(driver),
                "--build-subdir",
                build_subdir,
                "--functions",
                str(DEFAULT_WIDTH),
                "--output",
                str(record_path),
                "--emit-source",
                str(source_path),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        if done.returncode != 0:
            print(done.stderr, file=sys.stderr)
            return -1

        records = load_jsonl(record_path)
        if not records:
            print("error: the engine wrote no record")
            return -1

        problems = []
        seen = set()
        for record in records:
            name = record["benchmark_id"]
            seen.add(name)
            stats = record.get("stats", {})
            samples = record.get("samples_ns", [])
            if record.get("kind") != "micro":
                problems.append(f"{name}: not a micro record")
            if len(samples) != stats.get("count"):
                problems.append(
                    f"{name}: {len(samples)} samples but count {stats.get('count')}"
                )
            if not samples:
                problems.append(f"{name}: collected no sample")
            elif not stats.get("p50", 0) > 0:
                problems.append(f"{name}: p50 is {stats.get('p50')}")
            if not record.get("fixture_digest"):
                problems.append(f"{name}: no fixture digest")
            metadata = record.get("metadata", {})
            if not metadata.get("clock"):
                problems.append(f"{name}: no clock named")
            # Reported rather than required: how many samples a policy
            # gathers depends on how fast the host is, and a slow runner
            # is not a broken case.
            print(
                f"       {name}: p50 {stats.get('p50', 0) / 1e6:.3f}ms over "
                f"{len(samples)} samples, {record.get('confidence')} "
                f"confidence"
            )

        accepted = subprocess.run(
            [str(binary), "check", "--file", str(source_path), "--json"],
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        try:
            verdict = json.loads(accepted.stdout)
        except json.JSONDecodeError:
            problems.append("the compiler's verdict was not JSON")
        else:
            if verdict.get("status") != "ok" or verdict.get("diagnostics"):
                problems.append(
                    "the generated source does not compile: "
                    f"{verdict.get('diagnostics')}"
                )

    if problems:
        for problem in problems:
            print(f"BAD    {problem}")
        return 1
    print(f"ok     {len(seen)} case(s) reported and the fixture compiles")
    return 0


def run_reconcile(build_subdir: str, build_mode: str, tolerance: float) -> int:
    """Checks that the engine measures what it claims, and shows the rest.

    Two things are worth knowing and only one of them can be a gate.

    The engine's figure is the *steady-state* cost of a phase: the same
    bytes, run again and again, warm. A `--time-trace` run is a single
    invocation, and the phases that touch the file pay its first touch
    once. The two are therefore not expected to agree, and are not
    compared as though they should: for a small file the difference is an
    order of magnitude, and closing it would mean making the engine
    measure something less useful. They are printed side by side, so the
    gap is visible rather than assumed.

    What can be gated is the harness itself. Run twice on one machine and
    a case's median should land in the same place; if it does not, the
    clock, the warmup, or the batching is wrong, and every number the
    engine ever produced is suspect. That is the check CI runs.
    """
    try:
        driver = resolve_driver(build_subdir)
        binary = resolve_binary(build_subdir)
    except SuiteError as e:
        print(f"error: {e}")
        return -1

    if build_mode != "release":
        # A debug build carries assertions and sanitizers, so its timings
        # describe that build rather than the compiler. Refused rather
        # than warned about: the spread below would be read as a verdict
        # on the harness when it is really a verdict on the build.
        print(
            f"error: reconcile needs a release build; out/{build_subdir} is "
            f"{build_mode!r}"
        )
        print("       a debug build's figures are not a measurement of the compiler")
        return -1

    with tempfile.TemporaryDirectory(prefix="alcy_reconcile_") as scratch:
        work = Path(scratch)
        source_path = work / "bench.al"
        try:
            first = engine_run(
                driver,
                build_subdir,
                build_mode,
                DEFAULT_WIDTH,
                work / "first.jsonl",
                source_path,
            )
            second = engine_run(
                driver,
                build_subdir,
                build_mode,
                DEFAULT_WIDTH,
                work / "second.jsonl",
                source_path,
            )
        except SuiteError as e:
            print(f"error: {e}")
            return -1

        check = subprocess.run(
            [
                str(binary),
                "check",
                "--file",
                str(source_path),
                "--time-trace",
                "--json",
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        (work / "trace.json").write_text(check.stdout, encoding="utf-8")
        try:
            traces = phase_totals(work / "trace.json")
        except SuiteError as e:
            print(f"error: {e}")
            return -1

        print(f"engine, two runs at {DEFAULT_WIDTH} functions:")
        failures = 0
        rows = 0
        for name in sorted(set(first) & set(second)):
            one = first[name]["stats"]["p50"]
            two = second[name]["stats"]["p50"]
            smaller = min(one, two)
            spread = abs(one - two) / smaller if smaller > 0 else 0.0
            unstable = spread > tolerance
            if unstable:
                failures += 1
            rows += 1
            print(
                f"  {'BAD' if unstable else 'ok':4} {name}: "
                f"{one / 1000.0:.3f}ms then {two / 1000.0:.3f}ms "
                f"({spread * 100:.1f}% apart)"
            )

        print()
        print("against one compiler run, for scale rather than for agreement:")
        for name in sorted(set(first) & set(traces)):
            engine_us = first[name]["stats"]["p50"] / 1000.0
            traced_us = traces[name]
            if min(engine_us, traced_us) <= 0:
                continue
            print(
                f"       {name}: engine {engine_us:.3f}ms steady-state, "
                f"trace {traced_us:.3f}ms for the whole invocation"
            )

    if not rows:
        print("reconcile: the engine reported no case twice")
        return 1
    if failures:
        print(f"reconcile: {failures} case(s) did not reproduce")
        return 1
    return 0


def run_process(args) -> int:
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
            record = measure(
                case,
                binary,
                fixtures_dir,
                args.build_subdir,
                args.build_mode or infer_build_mode(args.build_subdir),
            )
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


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(
        description="Run the compiler's benchmarks, or read what they recorded."
    )
    sub = parser.add_subparsers(dest="command", required=True)

    process = sub.add_parser("process", help="time the alcy binary as a process")
    micro = sub.add_parser(
        "micro", help="time compiler phases in process, with no file in the way"
    )
    cmp_ = sub.add_parser("compare", help="compare two result files")
    smoke = sub.add_parser(
        "smoke",
        help="check the engine produced a full, well-formed result",
    )
    rec = sub.add_parser(
        "reconcile",
        help="check that the engine reproduces, and show it beside a run",
    )

    for target in (process, micro):
        target.add_argument(
            "--build-subdir",
            default="build",
            help="Subdirectory inside out/ holding the binary (default: build)",
        )
        target.add_argument(
            "--cases", default="", help="Comma-separated case ids to run"
        )
        target.add_argument("--output", default="", help="JSONL file to append to")
        target.add_argument(
            "--build-mode",
            default="",
            help="debug or release, recorded with the figures "
            "(default: read from --build-subdir)",
        )
    process.add_argument(
        "--suite",
        default="benchmarks/suites/cases.toml",
        help="Suite file, relative to the project root",
    )
    process.add_argument(
        "--fixtures",
        default="benchmarks/fixtures",
        help="Fixture root, relative to the project root",
    )
    cmp_.add_argument("before", help="Result file recorded earlier")
    cmp_.add_argument("after", help="Result file recorded now")
    for target in (smoke, rec):
        target.add_argument(
            "--build-subdir",
            default="build",
            help="Subdirectory inside out/ holding both binaries (default: build)",
        )
    rec.add_argument(
        "--build-subdir-unused",
        default="build",
        help="Subdirectory inside out/ holding both binaries (default: build)",
    )
    rec.add_argument(
        "--build-mode",
        default="",
        help="Build the figures came from (default: read from --build-subdir)",
    )
    rec.add_argument(
        "--tolerance",
        type=float,
        default=REPRODUCIBILITY_TOLERANCE,
        help="How far two runs of a case may differ (default %(default)s)",
    )

    args = parser.parse_args()
    if args.command == "compare":
        return compare(Path(args.before), Path(args.after))
    if args.command == "smoke":
        return run_smoke(args.build_subdir)
    if args.command == "reconcile":
        mode = args.build_mode or infer_build_mode(args.build_subdir)
        return run_reconcile(args.build_subdir, mode, args.tolerance)
    if args.command == "micro":
        mode = args.build_mode or infer_build_mode(args.build_subdir)
        return run_micro(args.build_subdir, mode, args.output, args.cases)
    return run_process(args)


if __name__ == "__main__":
    sys.exit(main())
