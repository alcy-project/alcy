#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Elapsed-time reporting shared by the check tools.

A tool adds the flag with `timing.add_argument(parser)` and calls
`timing.configure(args.timings)` after parsing. `mark()` names the time
since the previous mark (or the start), and the total is printed at exit.
Both go to stderr, leaving the check's report on stdout.
"""

import argparse
import atexit
import sys
import time

_enabled = False
_started = 0.0
_last = 0.0


def add_argument(parser: argparse.ArgumentParser) -> None:
    """Registers --timings so it appears in the tool's own help."""
    parser.add_argument(
        "--timings",
        action="store_true",
        help="report elapsed time for each phase to stderr",
    )


def configure(enabled: bool) -> None:
    """Begins timing when the caller's --timings flag was given."""
    global _enabled, _started, _last
    _enabled = enabled
    if not enabled:
        return
    _started = _last = time.perf_counter()
    atexit.register(_report_total)


def mark(label: str) -> None:
    """Reports the time since the last mark, or since start."""
    if not _enabled:
        return
    global _last
    now = time.perf_counter()
    print(f"{sys.argv[0]}: {now - _last:7.3f}s  {label}", file=sys.stderr)
    _last = now


def _report_total() -> None:
    print(
        f"total: {time.perf_counter() - _started:7.3f}s",
        file=sys.stderr,
    )
