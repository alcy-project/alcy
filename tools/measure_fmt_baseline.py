#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Record and compare the fmt pilot's baseline measurements.

The C++ conversion lane retires only after the tests, code size,
compile time, runtime performance, wasm parity, and digit accuracy
have been measured. This tool takes those measurements before the
conversion moves, and compares a later run against the record.

The process timings come from `run_benchmarks.py`, which keeps raw
samples in JSONL. Produced-image size comes from the release compile
of the same fixture, and the digit output comes from the exact
formatting case. Verdicts for wasm and the sanitized exe cases are
optional (`--full`) because they take much longer; the comparison
always checks them when both records carry them.

Wall-clock and size figures are machine-bound; the portability is in
the digit digest and the verdicts. `--compare` never judges a timing
on a different machine, and says so when the machine fingerprint
differs.

    uv run ./tools/measure_fmt_baseline.py --label before
    uv run ./tools/measure_fmt_baseline.py --label after \
        --compare out/fmt-baseline/before/summary.json --full
"""

import argparse
import hashlib
import json
import platform
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path

from utils.env import run_environment
from utils.paths import project_root_dir

ROOT = project_root_dir
BENCH_CASES = "check/fmt-heavy,compile/fmt-heavy"
FIXTURE = ROOT / "benchmarks" / "fixtures" / "fmt-heavy" / "main.al"
DIGIT_CASE = ROOT / "exe" / "cases" / "exe-fmt-exact" / "main.al"
EXE_CASES = (
    "exe-fmt,exe-fmt-carry,exe-fmt-exact,exe-fmt-exponent,exe-fmt-float,exe-fmt-min"
)


def run(argv: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        argv,
        cwd=cwd,
        env=run_environment(),
        capture_output=True,
        text=True,
        encoding="utf-8",
    )


def tool(*argv: str) -> list[str]:
    return ["uv", "run", "./tools/" + argv[0], *argv[1:]]


def machine() -> dict:
    return {
        "system": platform.system(),
        "machine": platform.machine(),
        "python": platform.python_version(),
    }


def git_head() -> str:
    result = run(["git", "rev-parse", "HEAD"], cwd=ROOT)
    return result.stdout.strip() if result.returncode == 0 else "unknown"


def benchmark_records(path: Path) -> list[dict]:
    return [
        json.loads(line)
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]


def record(scratch: Path, full: bool) -> dict:
    alcy = ROOT / "out" / "build" / "alcy"
    if not alcy.is_file():
        sys.exit("error: build out/build/alcy before taking a baseline")

    jsonl = scratch / "benchmarks.jsonl"
    proc = run(
        tool(
            "run_benchmarks.py",
            "process",
            "--build-subdir",
            "build",
            "--cases",
            BENCH_CASES,
            "--output",
            str(jsonl),
        )
    )
    if proc.returncode != 0 or not jsonl.is_file():
        sys.exit(f"error: process benchmarks failed\n{proc.stderr}")

    image = scratch / "fmt-heavy"
    compiled = run([str(alcy), "compile", str(FIXTURE), "--release", "-o", str(image)])
    if compiled.returncode != 0 or not image.is_file():
        sys.exit(f"error: release compile failed\n{compiled.stderr}")

    digit_dir = scratch / "digits"
    digit_dir.mkdir(parents=True, exist_ok=True)
    digit_exe = digit_dir / "case"
    built = run([str(alcy), "compile", str(DIGIT_CASE), "-o", str(digit_exe)])
    if built.returncode != 0 or not digit_exe.is_file():
        sys.exit(f"error: digit case did not compile\n{built.stderr}")
    digits = run([str(digit_exe)], cwd=digit_dir)
    (scratch / "digits.txt").write_text(digits.stdout, encoding="utf-8")

    result = {
        "recorded_utc": datetime.now(UTC).isoformat(),
        "machine": machine(),
        "git_head": git_head(),
        "digit_sha256": hashlib.sha256(digits.stdout.encode("utf-8")).hexdigest(),
        "digit_exit": digits.returncode,
        "image_bytes": image.stat().st_size,
        "benchmarks": benchmark_records(jsonl),
        "verdicts": {},
    }
    if full:
        checks = {
            "sanitize": tool(
                "check_exe.py",
                "--build-subdir=build",
                "--sanitize",
                "--cases",
                EXE_CASES,
            ),
            "wasm": tool("check_wasm.py", "--build-subdir=build"),
        }
        for name, argv in checks.items():
            proc = run(argv)
            result["verdicts"][name] = {
                "exit": proc.returncode,
                "last": proc.stdout.strip().splitlines()[-1]
                if proc.stdout.strip()
                else "",
            }
    return result


def median_ns(record: dict) -> float | None:
    return record.get("stats", {}).get("p50")


def compare(before: dict, after: dict) -> int:
    if before["machine"] != after["machine"]:
        print(
            "note: machine fingerprints differ; timing and size deltas are "
            "not comparable across runs"
        )
    problems = []
    if before["digit_sha256"] != after["digit_sha256"]:
        problems.append("digit output changed")
    if before["digit_exit"] != 0 or after["digit_exit"] != 0:
        problems.append("the digit case exited nonzero")
    for name, old in before["verdicts"].items():
        new = after["verdicts"].get(name)
        if new is None:
            continue
        if old["exit"] != new["exit"]:
            problems.append(f"{name} verdict changed: {old['exit']} -> {new['exit']}")
    old_times = {r["benchmark_id"]: median_ns(r) for r in before["benchmarks"]}
    new_times = {r["benchmark_id"]: median_ns(r) for r in after["benchmarks"]}
    for name in sorted(old_times.keys() & new_times.keys()):
        old, new = old_times[name], new_times[name]
        if old and new:
            print(
                f"{name}: p50 {old / 1e6:.3f}ms -> {new / 1e6:.3f}ms "
                f"({(new / old - 1) * 100:+.1f}%)"
            )
    print(f"image bytes: {before['image_bytes']} -> {after['image_bytes']}")
    print(
        f"digit digest: {before['digit_sha256'][:12]} -> {after['digit_sha256'][:12]}"
    )
    for name in sorted(before["verdicts"]):
        print(
            f"{name}: {before['verdicts'][name]['last']} | "
            f"{after['verdicts'].get(name, {}).get('last', 'not recorded')}"
        )
    if problems:
        print("FAILED: " + "; ".join(problems))
        return 1
    print("parity checks held")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default=datetime.now(UTC).strftime("%Y%m%d-%H%M%S"))
    parser.add_argument(
        "--output-dir", type=Path, default=ROOT / "out" / "fmt-baseline"
    )
    parser.add_argument(
        "--full",
        action="store_true",
        help="also run the sanitized exe cases and the wasm check",
    )
    parser.add_argument(
        "--compare", type=Path, help="a previous summary.json to compare against"
    )
    args = parser.parse_args()

    scratch = args.output_dir / args.label
    scratch.mkdir(parents=True, exist_ok=True)
    result = record(scratch, args.full)
    summary = scratch / "summary.json"
    summary.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"recorded {summary}")
    if args.compare is None:
        return 0
    return compare(json.loads(args.compare.read_text(encoding="utf-8")), result)


if __name__ == "__main__":
    sys.exit(main())
