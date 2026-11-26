#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""The check tools' one report channel.

Every check tool answers the same three questions - what passed, what
failed, and the count - and this module answers them in one shape, so
the tools read alike and a change to the shape lands in one place:

- quiet by default: a passing case is a line under --verbose and
  nothing otherwise, while a failing case always reports and carries
  its detail with it;
- one summary line at the end, in the form the CI logs already carry
  (`e2e: 517/517 passed`), whatever the verbosity;
- errors that are not a case's verdict (a missing binary, a bad
  invocation) go through `die`, which reports and stops.

The tools keep their own argparse and their own exit codes; this
module owns only what a reader sees. A future tool written in Alcy
reads the same three answers off a struct, which is why the methods
take the pieces rather than preformatted lines.
"""

from __future__ import annotations

import argparse
import sys


def add_verbose(parser: argparse.ArgumentParser) -> None:
    """Adds the -v/--verbose flag the report channel answers to."""
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="report every case as it passes, not only the failures",
    )


class Logger:
    """The one channel a check tool reports through."""

    def __init__(self, verbose: bool = False) -> None:
        self.verbose = verbose
        self.passed = 0
        self.failed = 0

    def case(self, name: str, ok: bool, detail: str = "") -> None:
        """Reports one case's verdict, loudly only when it failed."""
        if ok:
            self.passed += 1
            if self.verbose:
                print(f"PASS {name}")
            return
        self.failed += 1
        print(f"FAIL {name}")
        if detail:
            print(detail)

    def summary(self, label: str, total: int | None = None) -> int:
        """Prints the one line the CI log reads, and returns the exit code."""
        ran = self.passed + self.failed
        if total is None:
            total = ran
        print(f"{label}: {self.passed}/{total} passed")
        return 1 if self.failed else 0


def die(message: str) -> int:
    """Reports a failure that is not a case's verdict, and stops."""
    print(message, file=sys.stderr)
    return -1


class CheckResult:
    """What a check ran and how it went, before anything is printed.

    A tool's main builds one of these from its work and hands it to a
    Logger (or a future Alcy runner) to report; the shape, not the
    printing, is what an Alcy rewrite carries over.
    """

    def __init__(self, label: str) -> None:
        self.label = label
        self.cases: list[tuple[str, bool, str]] = []
        self.notes: list[str] = []

    def record(self, name: str, ok: bool, detail: str = "") -> None:
        self.cases.append((name, ok, detail))

    def note(self, text: str) -> None:
        self.notes.append(text)
