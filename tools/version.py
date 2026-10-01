#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Computes the version string built into the compiler.

The string is baked into a single generated translation unit (see
`compiler/cli/BUILD.gn`), which is what keeps a new commit from rebuilding
every file in the cli. It is `project_version` on a clean release tag, and
`project_version-snapshot` carrying the build commit otherwise; a working
tree with uncommitted tracked changes marks the commit `-dirty`. Without a
git checkout the commit is unknown, so the string is the snapshot form
alone.
"""

import argparse
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]


def _git(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )


def format_version(
    base: str, commit: str | None, on_tag: bool, dirty: bool
) -> str:
    """The version string for one build state.

    `commit` is None when the source carries no git metadata, which is the
    one case that reports neither a tag nor a commit.
    """
    if commit is None:
        return f"{base}-snapshot"
    if on_tag and not dirty:
        return f"{base} ({commit})"
    marker = f"{commit}-dirty" if dirty else commit
    return f"{base}-snapshot ({marker})"


def resolve_version(base: str) -> str:
    try:
        head = _git(["rev-parse", "--short=12", "HEAD"])
    except FileNotFoundError:
        return format_version(base, None, False, False)
    if head.returncode != 0:
        return format_version(base, None, False, False)
    commit = head.stdout.strip()
    on_tag = (
        _git(["describe", "--exact-match", "--tags", "--match", "v*", "HEAD"])
        .returncode
        == 0
    )
    dirty = bool(
        _git(["status", "--porcelain", "--untracked-files=no"]).stdout.strip()
    )
    return format_version(base, commit, on_tag, dirty)


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
