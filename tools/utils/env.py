#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import os


def run_environment(
    overrides: dict[str, str] | None = None, instrumented: bool = False
) -> dict[str, str]:
    """Environment for running a build product.

    The prebuilt LLVM is not built with ASan, so the container members its
    libraries define carry no annotations while this build's copies do. A
    call that crosses the two can report a container overflow for a legal
    write. The check is off for these runs; an option already set, in the
    environment or in `overrides`, wins.

    `instrumented` is for a product the sanitizers compiled: their ceiling
    on a request they will attempt is lower than the runtime's, so a size
    the runtime panics on aborts the instrument instead, and the same
    program answers differently in the two builds. A null answer puts the
    runtime's panic back in both.
    """
    env = dict(os.environ)
    if overrides:
        env.update(overrides)
    options = "detect_container_overflow=0"
    if instrumented:
        options += ":allocator_may_return_null=1"
    env.setdefault("ASAN_OPTIONS", options)
    return env
