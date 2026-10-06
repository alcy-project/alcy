#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Generates compiler corpora and measures a run against them.

A performance change is a claim about a number, and on a shared machine the
only number that repeats is the instruction count: it is what the program
asked the machine to do, so it does not move with load, caches or frequency.
Wall clock is the second question, and it is answered by running two binaries
against the same corpus in an interleaved order and taking the minimum, which
is what `ab` does. See `compiler/docs/performance.md` for the loop these
commands are steps in.

    uv run ./tools/perf.py corpus --shape plain --modules 300
    uv run ./tools/perf.py scale --shape plain --sizes 300 600 1200
    uv run ./tools/perf.py ab out/perf/plain1200 old-alcy new-alcy

The corpora are packages of generated modules with the shape of a real one.
They are written under `out/`, which is not tracked, and the tool reports the
directory it wrote so a command can point at it.

`corpus` is a generator, not a benchmark suite: its shapes exist to put
weight where a cost can hide. `plain` is the same module repeated, `generic`
gives every module a generic type and two instantiations of it, and `drop`
gives every module a destructor. A package with one module per file and a
`main` that names every one of them is what makes a cost proportional to the
package visible: a shape whose modules do not mention each other would not.

`scale` measures the same package at several sizes and reports the cost per
module. A cost that grows with the package shows there: per-module
instructions that rise with the module count are work no single module asked
for, because instructions do not grow from cache effects.

`profile` is the step that names the work: it records the same shape at two
sizes, reads each symbol's self time from `perf report`, and reports it per
module at both sizes. A symbol whose per-module cost rises is the one to read.
It needs a build with symbols, which `compiler/docs/performance.md` shows how
to make without committing the change.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from utils.paths import project_root_dir

REPO_ROOT = project_root_dir

DEFAULT_BINARY = "out/build_release/alcy"

# Where generated corpora and their build outputs go. Tracked nowhere.
WORK_DIR = "out/perf"

# The phases a run reports, in pipeline order. Their durations are summed per
# name over the trace, so a scope that nests inside another adds to both:
# compare a phase with itself across two runs, not with its neighbour.
PHASES = ("parse", "targets", "analyze", "lower", "borrow")

INSTRUCTIONS = re.compile(r"^(\d+),,.*instructions")

# One line of `perf report --no-children`, which is a share, a few columns, the
# object kind, and the symbol.
PROFILE_LINE = re.compile(r"^\s*([0-9]+\.[0-9]+)%\s+.*?\[([.k])\]\s+(.*)$")


def module_source(shape: str, index: int) -> str:
    """One module of a corpus, whose shape is the corpus's shape."""
    i = index
    if shape == "plain":
        return f"""// A module with the shape a real one has: named types, methods, a
// variant enum, a match, a loop, and string literals.
struct Point{i} {{
  x: i32,
  y: i32,
  label: str,
}}

impl Point{i} {{
  fn length_squared(self: Self) -> i32 {{
    ret self.x * self.x + self.y * self.y
  }}

  fn advance(mut self: &mut Self, dx: i32, dy: i32) -> i32 {{
    self.x = self.x + dx
    self.y = self.y + dy
    ret self.x * self.x + self.y * self.y
  }}
}}

enum Shape{i} {{
  Circle(i32),
  Rect(i32, i32),
  Empty,
}}

fn area{i}(s: Shape{i}) -> i32 {{
  match s {{
    Shape{i}::Circle(r) => {{ ret r * 3 * r }}
    Shape{i}::Rect(w, h) => {{ ret w * h }}
    Shape{i}::Empty => {{ ret 0 }}
  }}
}}

fn describe{i}(p: Point{i}) -> i32 {{
  mut total := 0
  mut n := p.x
  while n > 0 {{
    total = total + n
    n = n - 1
  }}
  ret total
}}

fn make{i}(k: i32) -> Point{i} {{
  ret Point{i} {{ x: k, y: k, label: "p" }}
}}
"""
    if shape == "generic":
        return f"""// A module whose generic box owns storage and drops it, so every
// instantiation mints a type with a destructor.
struct Box{i}<T> {{
  item: T,
  buf: &mut MaybeUninit<u8>,
}}

impl<T> Box{i}<T> {{
  fn drop(self: Box{i}<T>) {{
  }}

  fn peek(self: &Self) -> T {{
    ret self.item
  }}
}}

fn use{i}(k: i32) -> i32 {{
  big: Box{i}<i32> := Box{i} {{ item: k, buf: unsafe {{ alloc::<u8>(1) }} }}
  small: Box{i}<u8> := Box{i} {{ item: 3 as u8, buf: unsafe {{ alloc::<u8>(1) }} }}
  ret big.peek() + (small.peek() as i32)
}}
"""
    if shape == "drop":
        return f"""// A module whose point owns storage and drops it, so finding a
// destructor is part of checking every use.
struct Point{i} {{
  x: i32,
  y: i32,
  label: str,
  buf: &mut MaybeUninit<u8>,
}}

impl Point{i} {{
  fn drop(self: Point{i}) {{
  }}

  fn length_squared(self: Self) -> i32 {{
    ret self.x * self.x + self.y * self.y
  }}
}}

fn describe{i}(p: Point{i}) -> i32 {{
  mut total := 0
  mut n := p.x
  while n > 0 {{
    total = total + n
    n = n - 1
  }}
  ret total
}}

fn make{i}(k: i32) -> Point{i} {{
  ret Point{i} {{ x: k, y: k, label: "p", buf: unsafe {{ alloc::<u8>(1) }} }}
}}
"""
    raise SystemExit(f"unknown shape: {shape}")


def main_source(shape: str, modules: int, main_calls: int) -> str:
    """The entry module, which names every module the corpus has."""
    if shape == "plain":
        calls = "\n".join(
            f"  total = total + m{i % modules}::describe{i % modules}"
            f"(m{i % modules}::make{i % modules}({i % 7}))"
            for i in range(main_calls)
        )
    else:
        calls = "\n".join(
            f"  total = total + m{i % modules}::use{i % modules}({i % 7})"
            for i in range(main_calls)
        )
    return f"""fn main() -> i32 {{
  mut total := 0
{calls}
  ret total
}}
"""


def manifest_source(shape: str, modules: int) -> str:
    includes = ", ".join(f'"m{i}"' for i in range(modules))
    dependencies = ""
    if shape != "plain":
        dependencies = """
[dependencies]
"alcy/std/core" = {}
"alcy/std/alloc" = {}
"""
    return f"""[package]
name = "perf"
version = "0.1.0"

[modules]
include = ["main", {includes}]
{dependencies}
[[bin]]
name = "perf"
path = "main.al"
"""


def write_corpus(shape: str, modules: int, main_calls: int, out: Path) -> Path:
    """Writes one corpus and returns its directory."""
    out.mkdir(parents=True, exist_ok=True)
    for i in range(modules):
        (out / f"m{i}.al").write_text(module_source(shape, i))
    (out / "main.al").write_text(main_source(shape, modules, main_calls))
    (out / "alcy.toml").write_text(manifest_source(shape, modules))
    return out


def binary_for(binary: str) -> Path:
    path = Path(binary) if os.path.isabs(binary) else REPO_ROOT / binary
    if not path.is_file():
        raise SystemExit(f"no compiler at {path}; build one or pass --binary")
    return path


def instructions(binary: Path, corpus: Path, jobs: int) -> int:
    """Retired instructions for one check, which is the same number every run."""
    if shutil.which("perf") is None:
        raise SystemExit("perf is not on PATH; instruction counts need it")
    argv = [
        "perf",
        "stat",
        "-x,",
        "-e",
        "instructions",
        str(binary),
        "check",
        "-j",
        str(jobs),
        ".",
    ]
    done = subprocess.run(
        argv, cwd=corpus, capture_output=True, text=True, timeout=3600
    )
    # A hybrid CPU reports one line per PMU; a count of <not supported> on one
    # of them is a line without a number, so the numbers are summed.
    counts = [
        int(found.group(1))
        for line in done.stderr.splitlines()
        if (found := INSTRUCTIONS.match(line))
    ]
    if not counts:
        raise SystemExit(f"perf reported no instructions for {corpus}")
    return sum(counts)


def check_json(binary: Path, corpus: Path, jobs: int) -> dict:
    """One check as the compiler's own JSON report."""
    argv = [
        str(binary),
        "check",
        "-j",
        str(jobs),
        "--time-trace",
        "--json",
        ".",
    ]
    done = subprocess.run(
        argv, cwd=corpus, capture_output=True, text=True, timeout=3600
    )
    if done.returncode != 0:
        raise SystemExit(f"{binary} refused {corpus}: {done.stdout[:400]}")
    return json.loads(done.stdout)


def phase_totals(report: dict) -> dict:
    totals = dict.fromkeys(PHASES, 0.0)
    for event in report.get("traceEvents", []):
        name = event.get("name", "")
        if name in totals and "dur" in event:
            totals[name] += event["dur"] / 1000
    return totals


def human(count: float) -> str:
    for unit, scale in (("G", 1e9), ("M", 1e6), ("K", 1e3)):
        if abs(count) >= scale:
            return f"{count / scale:.1f}{unit}"
    return f"{count:.0f}"


def corpus_dir(text: str) -> Path:
    """A corpus directory, resolved from the repository root when relative."""
    path = Path(text)
    if not path.is_absolute():
        path = REPO_ROOT / path
    if not path.is_dir():
        raise SystemExit(f"no corpus at {path}")
    return path


def corpus_name(shape: str, modules: int, main_calls: int) -> str:
    """A directory name that tells the two main sizes apart."""
    name = f"{shape}{modules}"
    if main_calls:
        name += f"-main{main_calls}"
    return name


def cmd_corpus(args: argparse.Namespace) -> int:
    out = Path(args.out) if args.out else Path(WORK_DIR) / args.name
    if not out.is_absolute():
        out = REPO_ROOT / out
    modules = args.modules
    main_calls = args.main_calls if args.main_calls else modules
    write_corpus(args.shape, modules, main_calls, out)
    print(f"{out}: {modules} modules, {main_calls} calls in main")
    return 0


def cmd_profile(args: argparse.Namespace) -> int:
    binary = binary_for(args.binary)
    if shutil.which("perf") is None:
        raise SystemExit("perf is not on PATH; profiles need it")
    tagged = []
    for corpus, runs in ((args.small, args.runs_small), (args.large, args.runs_large)):
        path = corpus_dir(corpus)
        modules = len(list(path.glob("m*.al")))
        run = instructions(binary, path, args.jobs)
        totals = profile_self_time(binary, path, args.jobs, runs)
        tagged.append((Path(corpus), modules, run, totals))
    (small_path, small_modules, small_run, small) = tagged[0]
    (large_path, large_modules, large_run, large) = tagged[1]
    print(
        f"{small_path.name} ({small_modules} modules, {human(small_run)}"
        f" instructions) vs {large_path.name} ({large_modules} modules,"
        f" {human(large_run)})"
    )
    print(f"per-module self cost, largest growth first; binary={args.binary}")
    print(f"{'grow':>9} {'small':>9} {'large':>9}   symbol")
    rows = []
    for symbol, share in large.items():
        if share < args.floor:
            continue
        grew = share / 100 * large_run / large_modules
        before = small.get(symbol, 0.0) / 100 * small_run / small_modules
        rows.append((grew - before, before, grew, symbol))
    for grow, before, after, symbol in sorted(rows, reverse=True)[: args.limit]:
        print(f"{human(grow):>9} {human(before):>9} {human(after):>9}   {symbol[:64]}")
    return 0


def profile_self_time(binary: Path, corpus: Path, jobs: int, runs: int) -> dict:
    """Per-symbol self time as a share of a run, over `runs` runs of a corpus."""
    with tempfile.TemporaryDirectory() as scratch:
        recorded = Path(scratch) / "perf.data"
        inner = (
            f"for i in $(seq 1 {runs}); do {binary} check -j {jobs} ."
            f" >/dev/null 2>&1; done"
        )
        subprocess.run(
            [
                "perf",
                "record",
                "-F",
                "999",
                "-o",
                str(recorded),
                "bash",
                "-c",
                inner,
            ],
            cwd=corpus,
            capture_output=True,
            text=True,
            timeout=7200,
        )
        report = subprocess.run(
            [
                "perf",
                "report",
                "-i",
                str(recorded),
                "--stdio",
                "--no-children",
                "--percent-limit",
                "0",
            ],
            capture_output=True,
            text=True,
            timeout=3600,
        )
    totals = {}
    for line in report.stdout.splitlines():
        found = PROFILE_LINE.match(line)
        if not found or found.group(2) == "k":
            continue
        symbol = found.group(3).strip()
        if symbol.startswith("0x"):
            continue
        totals[symbol] = totals.get(symbol, 0.0) + float(found.group(1))
    return totals


def cmd_scale(args: argparse.Namespace) -> int:
    binary = binary_for(args.binary)
    rows = []
    for modules in args.sizes:
        corpus = write_corpus(
            args.shape,
            modules,
            args.main_calls if args.main_calls else modules,
            Path(WORK_DIR) / corpus_name(args.shape, modules, args.main_calls),
        )
        count = instructions(binary, corpus, args.jobs)
        rows.append((modules, count))
    calls = "fixed at " + str(args.main_calls) if args.main_calls else "one per module"
    print(
        f"shape={args.shape}  binary={args.binary}  jobs={args.jobs}"
        f"  main calls={calls}"
    )
    print(f"{'modules':>9} {'instructions':>13} {'per module':>11} {'growth':>8}")
    growth = float("nan")
    for i, (modules, count) in enumerate(rows):
        if i:
            before = rows[i - 1][1] / rows[i - 1][0]
            growth = count / modules / before
        shown = f"{growth:.3f}" if i else "-"
        print(
            f"{modules:>9} {human(count):>13} {human(count / modules):>11} {shown:>8}"
        )
    if len(rows) >= 2:
        first, last = rows[0], rows[-1]
        per_module = [count / modules for modules, count in (first, last)]
        slope = (per_module[1] - per_module[0]) / (last[0] - first[0])
        intercept = per_module[0] - slope * first[0]
        grown = slope * last[0]
        share = grown / per_module[1] if per_module[1] else 0.0
        print(
            f"per module = {human(intercept)} + {human(slope)} x modules;"
            f" at {last[0]} modules {share:.1%} of the run grows with the package"
        )
        print(f"corpora under {REPO_ROOT / WORK_DIR}")
    return 0


def cmd_ab(args: argparse.Namespace) -> int:
    corpus = corpus_dir(args.corpus)
    binaries = [binary_for(name) for name in args.binaries]
    jobs = [int(job) for job in args.jobs]
    best = {}
    for _ in range(args.reps):
        for binary in binaries:
            for job in jobs:
                report = check_json(binary, corpus, job)
                stats = report.get("stats", {})
                wall = stats.get("wall_ns", 0) / 1e6
                phases = phase_totals(report)
                key = (str(binary), job)
                if key not in best or wall < best[key][0]:
                    best[key] = (wall, phases, stats.get("peak_memory_bytes", 0))
    print(f"{corpus.name}, min of {args.reps}, interleaved")
    print(
        f"{'binary':<16} {'jobs':>4} {'total':>9} "
        + " ".join(f"{phase:>9}" for phase in PHASES)
        + f" {'peak':>8}"
    )
    for binary in binaries:
        for job in jobs:
            wall, phases, peak = best[(str(binary), job)]
            cells = " ".join(f"{phases[p]:>9.1f}" for p in PHASES)
            print(f"{binary.name:<16} {job:>4} {wall:>8.1f}m {cells} {human(peak):>8}")
    return 0


def cmd_determinism(args: argparse.Namespace) -> int:
    corpus = corpus_dir(args.corpus)
    binary = binary_for(args.binary)
    settings = [(job, []) for job in args.jobs if job != "default"]
    settings += [("default", [])] if "default" in args.jobs else []
    hashes = {}
    with tempfile.TemporaryDirectory() as scratch:
        for job, _ in settings:
            for _ in range(args.reps):
                out = Path(scratch) / "ir.ll"
                argv = [str(binary), "build"]
                if job != "default":
                    argv += ["-j", str(job)]
                argv += ["--emit", "llvm-ir", "-o", str(out), str(corpus)]
                done = subprocess.run(
                    argv,
                    cwd=REPO_ROOT,
                    capture_output=True,
                    text=True,
                    timeout=3600,
                )
                if done.returncode != 0 or not out.exists():
                    raise SystemExit(
                        f"{binary.name} refused {corpus}: {done.stderr[:400]}"
                    )
                digest = hashlib.sha256(out.read_bytes()).hexdigest()[:16]
                hashes.setdefault(digest, []).append(str(job))
    jobs_text = ", ".join(str(job) for job, _ in settings)
    if len(hashes) == 1:
        digest = next(iter(hashes))
        print(
            f"{corpus}: one hash over jobs {jobs_text} x {args.reps} repeats  {digest}"
        )
        return 0
    print(f"{corpus}: {len(hashes)} different hashes")
    for digest, seen in hashes.items():
        print(f"  {digest}  {', '.join(seen)}")
    return 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    # The compiler travels with every subcommand, before or after it: the
    # default here is the fallback, and SUPPRESS in the shared parser keeps a
    # subcommand that was not given one from overwriting it.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument(
        "--binary",
        default=argparse.SUPPRESS,
        help=f"compiler to measure (default: {DEFAULT_BINARY})",
    )
    parser.add_argument(
        "--binary",
        default=DEFAULT_BINARY,
        help=f"compiler to measure (default: {DEFAULT_BINARY})",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    corpus = subparsers.add_parser("corpus", parents=[common], help="write one corpus")
    corpus.add_argument(
        "--shape", default="plain", choices=["plain", "generic", "drop"]
    )
    corpus.add_argument("--modules", type=int, default=300)
    corpus.add_argument(
        "--main-calls",
        type=int,
        default=0,
        help="calls in main (default: one per module, so main grows with it)",
    )
    corpus.add_argument(
        "--out", help=f"directory to write (default: {WORK_DIR}/<name>)"
    )
    corpus.add_argument(
        "--name", default="", help="directory name under the work directory"
    )
    corpus.set_defaults(func=cmd_corpus)

    scale = subparsers.add_parser(
        "scale", parents=[common], help="instructions per module at several sizes"
    )
    scale.add_argument("--shape", default="plain", choices=["plain", "generic", "drop"])
    scale.add_argument("--sizes", type=int, nargs="+", default=[300, 600, 1200])
    scale.add_argument(
        "--main-calls",
        type=int,
        default=0,
        help="fix main's size, to separate per-item work from package scale",
    )
    scale.add_argument("--jobs", type=int, default=1)
    scale.set_defaults(func=cmd_scale)

    profile = subparsers.add_parser(
        "profile",
        parents=[common],
        help="self cost per module of one shape at two sizes, from perf",
    )
    profile.add_argument("small")
    profile.add_argument("large")
    profile.add_argument("--runs-small", type=int, default=30)
    profile.add_argument("--runs-large", type=int, default=8)
    profile.add_argument("--jobs", type=int, default=1)
    profile.add_argument(
        "--floor", type=float, default=0.12, help="smallest share to report"
    )
    profile.add_argument("--limit", type=int, default=14, help="symbols to show")
    profile.set_defaults(func=cmd_profile)

    ab = subparsers.add_parser(
        "ab", parents=[common], help="interleaved wall-clock A/B of binaries"
    )
    ab.add_argument("corpus")
    ab.add_argument("binaries", nargs="+")
    ab.add_argument("--jobs", nargs="+", default=["1", "4"])
    ab.add_argument("--reps", type=int, default=5)
    ab.set_defaults(func=cmd_ab)

    determinism = subparsers.add_parser(
        "determinism",
        parents=[common],
        help="one emitted-IR hash across job counts and repeats",
    )
    determinism.add_argument("corpus")
    determinism.add_argument("--jobs", nargs="+", default=["1", "4", "8", "default"])
    determinism.add_argument("--reps", type=int, default=3)
    determinism.set_defaults(func=cmd_determinism)

    args = parser.parse_args()
    # --name only exists for corpus, and a missing one derives a directory name.
    if args.command == "corpus" and not args.name:
        args.name = corpus_name(args.shape, args.modules, args.main_calls)
    return args


def main() -> int:
    args = parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
