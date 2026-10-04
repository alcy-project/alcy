#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import os


def run_environment(overrides: dict[str, str] | None = None) -> dict[str, str]:
    """Environment for running a build product.

    The prebuilt LLVM is not built with ASan, so the container members its
    libraries define carry no annotations while this build's copies do. A
    call that crosses the two can report a container overflow for a legal
    write. The check is off for these runs; an option already set, in the
    environment or in `overrides`, wins.
    """
    env = dict(os.environ)
    if overrides:
        env.update(overrides)
    env.setdefault("ASAN_OPTIONS", "detect_container_overflow=0")
    return env
