#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Computes the version string built into the compiler.

The string is baked into a single generated translation unit (see
`src/cli/BUILD.gn`), which is what keeps a new commit from rebuilding
every file in the cli. It is `project_version` on a clean release tag, and
`project_version-snapshot` carrying the build commit otherwise; a working
tree with uncommitted tracked changes marks the commit `-dirty` and adds
a digest of those changes, so that two dirty trees are two compilers and
not one. Without a git checkout the commit is unknown, so the string is
the snapshot form alone.
"""

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

# The paths whose content makes the compiler what it is. A change
# anywhere else - a test, a sample - does not make a new compiler, and
# naming it one would cost a cache miss for nothing.
COMPILER_PATHS = ["src", "build", "tools", "third_party/fpag"]


def _git(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


def dirty_digest() -> str | None:
    """A digest of the tracked changes, or None when there are none.

    A dirty tree is a compiler of its own, and two dirty trees are two
    compilers: the commit alone cannot tell them apart, and anything
    that keys on the compiler - the build cache, a summary's identity -
    would answer for the wrong one. The digest is over the changes to
    the files that make the compiler, so a change elsewhere does not
    make a new compiler.
    """
    diff = _git(["diff", "HEAD", "--", *COMPILER_PATHS])
    if diff.returncode != 0:
        return None
    if not diff.stdout.strip():
        return None
    return hashlib.sha256(diff.stdout.encode("utf-8", "replace")).hexdigest()[:12]


def format_version(
    base: str, commit: str | None, on_tag: bool, digest: str | None
) -> str:
    """The version string for one build state.

    `commit` is None when the source carries no git metadata, which is the
    one case that reports neither a tag nor a commit. `digest` is the
    content of the tracked changes, and it is what tells two dirty trees
    apart.
    """
    if commit is None:
        return f"{base}-snapshot"
    if on_tag and digest is None:
        return f"{base} ({commit})"
    marker = commit if digest is None else f"{commit}-dirty.{digest}"
    return f"{base}-snapshot ({marker})"


def resolve_version(base: str) -> str:
    try:
        head = _git(["rev-parse", "--short=12", "HEAD"])
    except FileNotFoundError:
        return format_version(base, None, False, None)
    if head.returncode != 0:
        return format_version(base, None, False, None)
    commit = head.stdout.strip()
    on_tag = (
        _git(
            ["describe", "--exact-match", "--tags", "--match", "v*", "HEAD"]
        ).returncode
        == 0
    )
    return format_version(base, commit, on_tag, dirty_digest())


def gn_string(value: str) -> str:
    """`value` as a GN string literal, for exec_script's value conversion."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Print the build version as a GN string."
    )
    parser.add_argument("base", help="the hand-maintained version, e.g. 0.1.0")
    args = parser.parse_args()
    print(gn_string(resolve_version(args.base)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted by user. Exiting immediately...", file=sys.stderr)
        sys.exit(130)
