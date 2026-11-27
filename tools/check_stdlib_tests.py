#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Check the toolchain standard library with its own tests.

Every package under `lib/std/` holds `#[test] fn`s beside the definitions
they cover, and `alcy test` runs them. This check asks each
package for the tests it names and then for its verdict:

  - a package names at least one test, because a package that runs
    nothing would pass here while checking nothing;
  - `--list` and the run agree on how many tests the package has;
  - the run exits 0 and reports no failure.

The names come from `--list --json` rather than from a second reading of
the sources, so the count is what the compiler discovered rather than what
a grep believes it would, and the counts of a run come from the document
it writes rather than from the sentences it prints for a reader.

The suite runs from a copy under the repository's `out/`: the packages
import their siblings by path, so the whole tree is staged at once and
everything the compiler leaves behind stays in one removable directory.
"""

import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from utils import log, timing
from utils.env import run_environment
from utils.paths import project_root_dir

STD_DIR = project_root_dir / "lib" / "std"

# The packages that name no test, each with the reason it names none.
# Every other member of the suite has to name at least one, so a package
# that runs nothing cannot pass here by saying nothing: adding a name to
# one of these means adding the test that covers it, and a new package
# means joining this list on purpose.
UNTESTED = {
    "arch": "declares no items yet",
    "atomic": "declares no items yet",
    "network": "declares no items yet",
    "simd": "declares no items yet",
    "sync": "declares no items yet",
    "thread": "declares no items yet",
    "time": "declares no items yet",
}


def alcy_binary(build_subdir: str):
    alcy = project_root_dir / "out" / build_subdir / "alcy"
    if not alcy.is_file():
        alcy = alcy.with_suffix(".exe")
    return alcy


def staged_std() -> Path:
    """A copy of the whole std tree under the repository's out/.

    The packages import their siblings by path, so one package cannot be
    staged alone. Running the suite from this copy keeps the compiler's
    own output placement, which then lands under out/ as well.
    """
    target = project_root_dir / "out" / "std" / "lib" / "std"
    if target.exists():
        shutil.rmtree(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(STD_DIR, target)
    return target


def packages(root: Path):
    """Every package of the suite, in the order the directory lists them."""
    return sorted(path.parent for path in root.glob("*/alcy.toml"))


def read_stream(result):
    """One invocation output as the failure report shows it."""
    return (result.stdout + result.stderr).strip()


def invoke(alcy, package, *options):
    """Run `alcy test` on one package and read the document it wrote."""
    temp = project_root_dir / "out" / "tmp"
    temp.mkdir(parents=True, exist_ok=True)
    env = run_environment(
        {
            "ALCY_STD_TEST_ENV": "alcy-std-test-value",
            "TMPDIR": str(temp),
            "TMP": str(temp),
            "TEMP": str(temp),
        }
    )
    tried = subprocess.run(
        [str(alcy), "test", *options, "--json", str(package)],
        env=env,
        capture_output=True,
        text=True,
    )
    try:
        return tried, json.loads(tried.stdout)
    except json.JSONDecodeError:
        return tried, None


def stats_of(document):
    """The figures a document reports, or an empty mapping when it has none."""
    figures = (document or {}).get("stats")
    return figures if isinstance(figures, dict) else {}


def check_package(alcy, package):
    listed, document = invoke(alcy, package, "--list")
    if listed.returncode != 0 or document is None:
        return (package, 0, None, "listing the tests failed\n" + read_stream(listed))
    names = document.get("test_labels") or []
    if not names:
        reason = UNTESTED.get(package.name)
        if reason is None:
            return (
                package,
                0,
                None,
                "names no test; the `#[test] fn`s of a package's modules are what "
                "this check runs, and a package with nothing to run belongs in "
                "UNTESTED with the reason it has nothing",
            )
        return (package, 0, [reason], None)
    ran, document = invoke(alcy, package)
    output = read_stream(ran)
    if document is None:
        return (package, 0, None, "the run wrote no result\n" + output)
    figures = stats_of(document)
    verdict = figures.get("tests")
    failed = figures.get("test_failures")
    if verdict != len(names):
        return (
            package,
            0,
            None,
            f"listed {len(names)} tests but reported {verdict}:\n{output}",
        )
    if ran.returncode != 0 or failed:
        return (
            package,
            0,
            None,
            f"the run exited {ran.returncode} with {failed} failures:\n{output}",
        )
    return (package, len(names), names, None)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="Run the stdlib tests.")
    parser.add_argument(
        "--build-subdir",
        default="build",
        help="Subdirectory inside out/ holding the alcy binary",
    )
    parser.add_argument(
        "--package",
        action="append",
        default=[],
        help="Package name to run (default: every member of the suite)",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=0,
        help="Packages to run concurrently (default: the core count)",
    )
    log.add_verbose(parser)
    timing.add_argument(parser)
    args = parser.parse_args()
    timing.configure(args.timings)

    alcy = alcy_binary(args.build_subdir)
    if not alcy.is_file():
        sys.exit(log.die(f"alcy binary not found in out/{args.build_subdir}/"))

    selected = packages(staged_std())
    if args.package:
        wanted = set(args.package)
        selected = [path for path in selected if path.name in wanted]
        missing = wanted - {path.name for path in selected}
        if missing:
            sys.exit("no such package: " + ", ".join(sorted(missing)))

    jobs = args.jobs or min(8, os.cpu_count() or 1)
    timing.mark("setup")
    results = run_packages(alcy, selected, jobs)
    timing.mark("run")

    logger = log.Logger(args.verbose)
    total = 0
    for package, count, names, problem in results:
        if problem is not None:
            logger.case(package.name, False, problem)
            continue
        # A package that names no test passes by declaring why, so the
        # reason rides the line the verbose report shows.
        logger.case(
            f"{package.name}: no test, and {names[0]}"
            if count == 0
            else f"{package.name}: {count} tests",
            True,
        )
        total += count
    # The suite's summary counts the tests the packages carry, not the
    # packages the check ran, so the line keeps its old shape.
    ran = logger.passed + logger.failed
    print(
        f"stdlib tests: {total} tests in {ran} packages passed"
        if logger.failed == 0
        else f"stdlib tests: {logger.passed}/{ran} packages passed"
    )
    return 1 if logger.failed else 0


def run_packages(alcy, selected, jobs: int):
    """Checks every package and returns the verdicts, before printing."""

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(check_package, alcy, path) for path in selected]
        return [future.result() for future in futures]


if __name__ == "__main__":
    sys.exit(main())
