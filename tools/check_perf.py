#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Checks a working tree against a base revision for regressions.

The check builds the compiler at the base revision in its own
worktree, builds the working tree, generates one set of corpora both
binaries measure, and compares retired instructions per corpus.
Instruction counts are the number that repeats on a shared machine
(ADR-0021); wall clock is not consulted.

Two axes are checked:

- Per corpus: the working tree count against the base, past the
  allowance tools/check_perf.toml grants that row, if any.
- Growth: the per-module cost of the plain and generic shapes at 300
  and 600 modules. Linear work holds the ratio of the two at one; a
  cost that grows with the package pushes it up, which is how the
  shapes a fixed-size corpus undersamples are caught.

A regression a change accepts on purpose is recorded by adding an
allow entry to tools/check_perf.toml, naming the report row, the
delta it tolerates, and the decision. The entry is the record; the
commit that adds it carries the numbers.

Exit codes: 0 the tree passes, 1 a regression or a growth shift, 2
the check could not run.

    uv run tools/check_perf.py --base main
    uv run tools/check_perf.py --base HEAD~5
"""

import argparse
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import perf  # noqa: E402

REPO_ROOT = perf.REPO_ROOT
GATE_DIR = REPO_ROOT / "out" / "check-perf"
CONFIG_PATH = Path(__file__).resolve().parent / "check_perf.toml"

# The corpora the gate measures: (shape, modules, width, commands).
# The module shapes cover the pipeline stages; the diagnostics shapes
# cover the finding paths a clean module corpus never stresses. A
# width applies to the shapes that take one.
CORPORA = [
    ("plain", 300, None, ["check", "lint", "fmt --check"]),
    ("generic", 300, None, ["check"]),
    ("drop", 300, None, ["check"]),
    ("closure", 300, None, ["check"]),
    ("adapt", 300, None, ["check"]),
    ("comp", 300, None, ["check"]),
    ("print", 300, None, ["check"]),
    ("args", 1, 300, ["lint"]),
    ("find", 1, 300, ["lint"]),
]

# The shapes whose per-module cost is compared across a size step.
GROWTH_SHAPES = ["plain", "generic"]
GROWTH_SIZES = (300, 600)


def git(*args: str) -> str:
    """Runs git from the repository root, exiting the check on error."""
    done = subprocess.run(["git", *args], cwd=REPO_ROOT, capture_output=True, text=True)
    if done.returncode != 0:
        print(f"git failed: {done.stderr.strip()}", file=sys.stderr)
        sys.exit(2)
    return done.stdout.strip()


def build_tree(tree: Path) -> Path:
    """Builds the release compiler in the tree and returns its binary."""
    done = subprocess.run(
        [
            "uv",
            "run",
            "tools/build.py",
            "--mode=release",
            "--target=alcy",
            "--build-subdir=opt",
        ],
        cwd=tree,
    )
    binary = tree / "out" / "opt" / "alcy"
    if done.returncode != 0 or not binary.is_file():
        print(f"the build in {tree} failed; the check cannot continue", file=sys.stderr)
        sys.exit(2)
    return binary


def submodules_into(tree: Path) -> None:
    """Copies the checked-out submodule sources a build reads.

    The base worktree checks out a revision whose gitlinks may name
    commits no local checkout holds, and the sources live beside this
    tree; the build reads them in place.
    """
    for line in git("ls-files", "-s", "third_party").splitlines():
        meta, _, path = line.partition("\t")
        fields = meta.split()
        if not fields or fields[0] != "160000":
            continue
        source = REPO_ROOT / path
        if not source.is_dir():
            continue
        destination = tree / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(source, destination)


def prepare_base(base_ref: str, keep: bool) -> Path:
    """Builds the base revision in a worktree and returns its binary.

    The binary is copied out of the worktree before the worktree goes:
    a removed worktree takes its build directory with it.
    """
    tree = GATE_DIR / "base"
    subprocess.run(
        ["git", "worktree", "remove", "--force", str(tree)],
        cwd=REPO_ROOT,
        capture_output=True,
    )
    shutil.rmtree(tree, ignore_errors=True)
    subprocess.run(["git", "worktree", "prune"], cwd=REPO_ROOT, capture_output=True)
    git("worktree", "add", "--detach", str(tree), base_ref)
    submodules_into(tree)
    binary = build_tree(tree)
    copied = GATE_DIR / "base-alcy"
    GATE_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, copied)
    if not keep:
        subprocess.run(
            ["git", "worktree", "remove", "--force", str(tree)],
            cwd=REPO_ROOT,
            capture_output=True,
        )
        shutil.rmtree(tree, ignore_errors=True)
        subprocess.run(["git", "worktree", "prune"], cwd=REPO_ROOT, capture_output=True)
    return copied


def corpus_name(shape: str, modules: int, width: int | None) -> str:
    name = f"{shape}{modules}"
    if width is not None:
        name += f"-w{width}"
    return name


def corpus_dir(root: Path, shape: str, modules: int, width: int | None) -> Path:
    return root / corpus_name(shape, modules, width)


def generate_corpora(root: Path) -> dict[str, Path]:
    """Generates every corpus the gate measures, from this tree tool."""
    if root.exists():
        shutil.rmtree(root)
    corpora = {}
    for shape, modules, width, _ in CORPORA:
        out = corpus_dir(root, shape, modules, width)
        perf.write_corpus(shape, modules, modules, out, width)
        corpora[corpus_name(shape, modules, width)] = out
    for shape in GROWTH_SHAPES:
        out = corpus_dir(root, shape, GROWTH_SIZES[1], None)
        perf.write_corpus(shape, GROWTH_SIZES[1], GROWTH_SIZES[1], out, None)
        corpora[corpus_name(shape, GROWTH_SIZES[1], None)] = out
    return corpora


def measure(binary: Path, corpus: Path, command: str, reps: int) -> int:
    """The median of the reps instruction counts for one run."""
    counts = [perf.instructions(binary, corpus, 1, command) for _ in range(reps)]
    return sorted(counts)[len(counts) // 2]


def load_allows() -> dict[str, float]:
    """The per-row allowances tools/check_perf.toml grants."""
    if not CONFIG_PATH.is_file():
        return {}
    with CONFIG_PATH.open("rb") as handle:
        data = tomllib.load(handle)
    return {
        entry["row"]: float(entry["max_delta_percent"])
        for entry in data.get("allow", [])
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--base",
        default="",
        help="revision the working tree is compared against"
        " (default: the merge-base with main)",
    )
    parser.add_argument(
        "--fail-percent",
        type=float,
        default=2.0,
        help="delta that fails a row (default: 2.0)",
    )
    parser.add_argument(
        "--warn-percent",
        type=float,
        default=0.5,
        help="delta that warns on a row (default: 0.5)",
    )
    parser.add_argument(
        "--growth-percent",
        type=float,
        default=5.0,
        help="per-module cost shift across the size step the"
        " growth check tolerates past the base own ratio, in"
        " points (default: 5.0)",
    )
    parser.add_argument(
        "--reps",
        type=int,
        default=3,
        help="measurements per row, keeping the median (default: 3)",
    )
    parser.add_argument(
        "--keep-base",
        action="store_true",
        help="keep the base worktree after the check",
    )
    args = parser.parse_args()

    base_ref = args.base or git("merge-base", "HEAD", "main")
    base_ref = git("rev-parse", base_ref)
    if base_ref == git("rev-parse", "HEAD"):
        print(
            "the base revision is the working tree own commit; pass"
            " --base <ref> naming what to compare against (for example"
            " --base HEAD~5)",
            file=sys.stderr,
        )
        return 2

    allows = load_allows()
    corpora = generate_corpora(GATE_DIR / "corpora")
    head_binary = build_tree(REPO_ROOT)
    base_binary = prepare_base(base_ref, args.keep_base)

    # (row label, corpus, command), in the order the report reads.
    rows = []
    for shape, modules, width, commands in CORPORA:
        name = corpus_name(shape, modules, width)
        for command in commands:
            rows.append((f"{command} {name}", corpora[name], command))

    measured: dict[tuple[str, str], tuple[int, int]] = {}
    for label, corpus, command in rows:
        before = measure(base_binary, corpus, command, args.reps)
        after = measure(head_binary, corpus, command, args.reps)
        measured[(label, command)] = (before, after)

    # A suspect is measured again before it fails: a count that does
    # not reproduce was a glitched counter, not a regression.
    suspects = [
        (label, command)
        for label, corpus, command in rows
        if (measured[(label, command)][1] - measured[(label, command)][0])
        / measured[(label, command)][0]
        * 100
        > allows.get(label, args.fail_percent)
    ]
    for label, command in suspects:
        corpus = next(row[1] for row in rows if row[0] == label)
        before = measure(base_binary, corpus, command, args.reps)
        after = measure(head_binary, corpus, command, args.reps)
        held = measured[(label, command)]
        measured[(label, command)] = (min(before, held[0]), min(after, held[1]))

    print()
    header = f"{'row':32} {'base':>15} {'head':>15} {'delta':>9}  status"
    print(header)
    failed = []
    warned = []
    improved = []
    for label, _corpus, command in rows:
        before, after = measured[(label, command)]
        delta = (after - before) / before * 100
        limit = allows.get(label, args.fail_percent)
        if delta > limit:
            status = f"FAIL (allow {limit:+.1f}%)"
            failed.append(label)
        elif delta > args.warn_percent:
            status = "warn"
            warned.append(label)
        elif delta < -args.warn_percent:
            status = "improved"
            improved.append(label)
        else:
            status = "ok"
        line = f"{label:32} {before:>15,} {after:>15,} {delta:>+8.2f}%  {status}"
        print(line)

    for shape in GROWTH_SHAPES:
        per_module = {}
        for size in GROWTH_SIZES:
            name = f"{shape}{size}"
            held = measured.get((f"check {name}", "check"))
            if held is None:
                corpus = corpora[name]
                held = (
                    measure(base_binary, corpus, "check", args.reps),
                    measure(head_binary, corpus, "check", args.reps),
                )
            per_module[size] = (held[0] / size, held[1] / size)
        base_ratio = per_module[GROWTH_SIZES[1]][0] / per_module[GROWTH_SIZES[0]][0]
        head_ratio = per_module[GROWTH_SIZES[1]][1] / per_module[GROWTH_SIZES[0]][1]
        shift = head_ratio - base_ratio
        status = "ok"
        if shift * 100 > args.growth_percent:
            status = "FAIL"
            failed.append(f"growth {shape}")
        line = (
            f"{'growth ' + shape:32} {base_ratio:>15.3f}"
            f" {head_ratio:>15.3f} {shift:>+8.3f}  {status}"
        )
        print(line)

    if failed:
        joined = ", ".join(failed)
        print(f"check_perf: FAIL ({joined})")
        return 1
    print(f"check_perf: pass ({len(improved)} improved, {len(warned)} warned)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
