#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Elapsed-time reporting shared by the check tools.

A tool that wants a --timings flag wraps its entry point; the flag is
stripped before argparse sees it, so no parser or return path changes.
`mark()` names the time since the previous mark (or the start), and the
total is printed at exit. Both go to stderr, leaving the check's report
on stdout.
"""

import atexit
import sys
import time
from collections.abc import Callable

_enabled = False
_started = 0.0
_last = 0.0


def start() -> None:
    """Begins timing when --timings was given; otherwise a no-op."""
    global _enabled, _started, _last
    _enabled = "--timings" in sys.argv
    if not _enabled:
        return
    sys.argv.remove("--timings")
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
        f"{sys.argv[0]}: {time.perf_counter() - _started:7.3f}s  total",
        file=sys.stderr,
    )


def run_with_timings(entry: Callable[[], int]) -> int:
    """Runs `entry`, reporting phase and total times when --timings was given."""
    start()
    return entry()
