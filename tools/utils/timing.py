#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Elapsed-time reporting shared by the check tools.

A tool that wants a --timings flag wraps its entry point; the flag is
stripped before argparse sees it, so no parser or return path changes.
The total is printed to stderr when the process exits, leaving the
check's own report on stdout.
"""

import atexit
import sys
import time
from collections.abc import Callable


def run_with_timings(entry: Callable[[], int]) -> int:
    """Runs `entry`, reporting the total at exit when --timings was given."""
    enabled = "--timings" in sys.argv
    if not enabled:
        return entry()
    sys.argv.remove("--timings")

    started = time.perf_counter()

    def report() -> None:
        print(
            f"{sys.argv[0]}: {time.perf_counter() - started:7.3f}s total",
            file=sys.stderr,
        )

    atexit.register(report)
    return entry()
