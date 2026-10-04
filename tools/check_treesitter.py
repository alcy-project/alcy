#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Keeps the tree-sitter grammar and the compiler in step.

The grammar in `treesitter/` is a second reading of the language, written
against `docs/spec/` rather than against the compiler. A second reading is
only worth having if it is checked against the first, so this runs three
checks, each naming its own oracle:

- Generated sources are current. `tree-sitter generate` rewrites them and
  `git diff` says whether the committed copies match, so a change to
  `grammar.js` cannot land without the tables it produces.
- The corpus passes. Every construct in the specification has a case, and
  a case that cannot parse is the failure.
- The grammar and the compiler agree on the tree in this repository. Every
  `.al` file the compiler accepts must parse here without an `ERROR` or a
  `MISSING` node, and every file listed in `test/parse_errors.txt` must not.
  The compiler's verdict is the oracle: it is the implementation the
  specification is written against.

Nothing here needs a build except the differential check, which reads the
exit code of `alcy check` on each file.
"""

import argparse
import shutil
import subprocess
import sys
import tomllib

from pathlib import Path

from utils.paths import project_root_dir

GRAMMAR_DIR = project_root_dir / "treesitter"
GENERATED_DIR = GRAMMAR_DIR / "src"
PARSE_ERRORS = GRAMMAR_DIR / "test" / "parse_errors.txt"
QUERIES = [
    GRAMMAR_DIR / "queries" / "alcy" / "highlights.scm",
    GRAMMAR_DIR / "queries" / "alcy" / "tags.scm",
]
# Every directory in the repository that holds alcy sources. A file the
# compiler accepts is one whose tree this grammar has to agree with.
SOURCE_ROOTS = ["lib", "e2e", "exe", "samples", "benchmarks"]


def grammar_cli_version(cli: str) -> str:
    result = subprocess.run([cli, "--version"], capture_output=True, text=True)
    if result.returncode != 0:
        return ""
    return result.stdout.split()[-1]


def recorded_cli_version() -> str:
    with open(project_root_dir / "config.toml", "rb") as handle:
        return tomllib.load(handle)["tree_sitter_cli_version"]


def run(command: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        command, cwd=cwd or project_root_dir, capture_output=True, text=True
    )


def report(step: str, result: subprocess.CompletedProcess) -> bool:
    if result.returncode == 0:
        print(f"{step}: ok")
        return True
    output = (result.stdout + result.stderr).strip()
    print(f"{step}: FAILED")
    for line in output.split("\n"):
        print(f"  {line}")
    return False


def series(version: str) -> str:
    return ".".join(version.split(".")[:2])


def check_version(cli: str) -> bool:
    """`grammar.json` is written by whichever CLI generated it, so a CLI of
    another series leaves sources formatted by a tool this repository does
    not name. The series is the granularity because that is what a patch
    release is not expected to change; a patch that does change the output is
    caught a moment later by the drift check, with a diff that says so,
    rather than by a version string that would refuse to explain itself."""
    recorded = recorded_cli_version()
    found = grammar_cli_version(cli)
    if found and series(found) == series(recorded):
        print(f"version: ok ({recorded} recorded, {found} found)")
        return True
    print(
        f"version: FAILED\n"
        f"  config.toml records {recorded}, the CLI reports {found or 'nothing'}\n"
        f"  install the recorded version, or regenerate and commit what {found} writes"
    )
    return False


def check_generated() -> bool:
    """A change to `grammar.js` that leaves `src/` alone is a change whose
    effect nobody committed."""
    generated = run(["tree-sitter", "generate"], cwd=GRAMMAR_DIR)
    if not report("generate", generated):
        return False
    relative = str(GENERATED_DIR.relative_to(project_root_dir))
    return report(
        "generated sources", run(["git", "diff", "--exit-code", "--", relative])
    )


def check_corpus() -> bool:
    return report("corpus", run(["tree-sitter", "test"], cwd=GRAMMAR_DIR))


def check_queries(sample: Path) -> bool:
    """`tree-sitter test` does not read the queries, so a typo in one is
    invisible until an editor loads it."""
    ok = True
    for query in QUERIES:
        result = run(["tree-sitter", "query", str(query), str(sample)], cwd=GRAMMAR_DIR)
        ok = report(f"query {query.name}", result) and ok
    return ok


def alcy_sources() -> list[Path]:
    return sorted(
        path
        for root in SOURCE_ROOTS
        for path in (project_root_dir / root).rglob("*.al")
    )


def unread(paths: list[Path]) -> set[Path]:
    """The sources among `paths` this grammar did not read whole.

    `--quiet` prints only the files that failed, and names them, so one run
    over the whole set is both the check and its report.
    """
    if not paths:
        return set()
    result = run(["tree-sitter", "parse", "--quiet", *map(str, paths)], cwd=GRAMMAR_DIR)
    if result.returncode != 0 and not result.stdout.strip():
        # A crashed or missing parser prints nothing, which would read as
        # "every file parsed"; name them all so the caller fails loudly.
        print(f"tree-sitter parse failed: {result.stderr.strip()}", file=sys.stderr)
        return set(paths)
    # The names are printed relative to where the parser runs, which is the
    # grammar directory, and padded into a column, so the name has to be
    # trimmed before it is a path.
    named = {
        (GRAMMAR_DIR / line.split("\t")[0].strip()).resolve()
        for line in result.stdout.split("\n")
        if line.strip()
    }
    return {path for path in paths if path.resolve() in named}


def compiler_verdicts(alcy: Path) -> dict[Path, bool]:
    """What `alcy check` says about each source, one run per case.

    A directory holding an `alcy.toml` is a package, and its files name each
    other, so the package is what has an accept or a reject; a lone file is
    checked on its own. The verdict goes to every file of the case rather
    than to one of them, which errs towards calling a file accepted and
    therefore towards asking more of the grammar.
    """
    verdicts: dict[Path, bool] = {}
    for path in alcy_sources():
        case = path.parent
        if case in verdicts:
            continue
        if (case / "alcy.toml").is_file():
            result = run([str(alcy), "check", "."], cwd=case)
            for member in case.rglob("*.al"):
                verdicts[member] = result.returncode == 0
        else:
            verdicts[path] = (
                run([str(alcy), "check", "--file", str(path)]).returncode == 0
            )
    return verdicts


def expected_parse_errors() -> set[str]:
    """The cases whose failure is a failure to parse rather than a failure
    to type-check. Each line names a directory under `e2e/cases/`."""
    if not PARSE_ERRORS.is_file():
        return set()
    return {
        line.strip()
        for line in PARSE_ERRORS.read_text(encoding="utf-8").split("\n")
        if line.strip() and not line.lstrip().startswith("#")
    }


def check_differential(alcy: Path) -> bool:
    verdicts = compiler_verdicts(alcy)
    accepted = sorted(path for path, ok in verdicts.items() if ok)
    rejected = sorted(path for path, ok in verdicts.items() if not ok)
    print(
        f"differential: the compiler accepts {len(accepted)} of "
        f"{len(verdicts)} sources"
    )

    problems = [
        f"the compiler accepts {path.relative_to(project_root_dir)}, "
        "the grammar does not"
        for path in unread(accepted)
    ]

    # The list names cases by directory, which is how e2e/cases/ is
    # addressed everywhere else in this repository.
    cases = project_root_dir / "e2e" / "cases"
    must_fail = expected_parse_errors()
    listed = [
        path
        for path in rejected
        if path.is_relative_to(cases) and path.parent.name in must_fail
    ]
    problems += [
        f"{path.relative_to(project_root_dir)} is listed as a parse error "
        "and parsed cleanly"
        for path in listed
        if path not in unread(listed)
    ]
    for name in sorted(must_fail):
        case = cases / name
        if not case.is_dir():
            problems.append(f"test/parse_errors.txt names {name}, which is not a case")
        elif all(verdicts[path] for path in verdicts if path.is_relative_to(case)):
            problems.append(
                f"test/parse_errors.txt names {name}, which the compiler accepts"
            )

    for problem in problems:
        print(f"  {problem}")
    if problems:
        print(f"differential: FAILED ({len(problems)} disagreement(s))")
        return False
    print(f"differential: ok ({len(listed)} parse error(s) confirmed)")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=str(__doc__).split("\n")[0])
    parser.add_argument(
        "--grammar",
        action="store_true",
        help="Check the generated sources, the corpus, and the queries (default)",
    )
    parser.add_argument(
        "--differential",
        action="store_true",
        help="Also compare the grammar against the compiler on every .al file",
    )
    parser.add_argument(
        "--build-subdir",
        default="build",
        help="Subdirectory inside out/ holding the alcy binary (default: build)",
    )
    args = parser.parse_args()

    cli = "tree-sitter"
    if shutil.which(cli) is None:
        print(
            f"error: {cli} not found on PATH; it builds and tests the grammar, "
            "so nothing else here can run without it",
            file=sys.stderr,
        )
        return 1

    if not args.differential:
        args.grammar = True

    sample = project_root_dir / "samples" / "primes" / "main.al"
    checks = [check_version(cli)]
    if args.grammar:
        checks.append(check_generated())
        checks.append(check_corpus())
        checks.append(check_queries(sample))

    if args.differential:
        alcy = project_root_dir / "out" / args.build_subdir / "alcy"
        if not alcy.is_file():
            alcy = alcy.with_suffix(".exe")
        if not alcy.is_file():
            print(
                f"error: alcy binary not found in out/{args.build_subdir}/; "
                "build it first, or drop --differential",
                file=sys.stderr,
            )
            return 1
        checks.append(check_differential(alcy))

    return 0 if all(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
