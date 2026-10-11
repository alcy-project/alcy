#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Prints the pinned versions config.toml carries, for CI steps.

A workflow reads a pin where the step that uses it runs, rather than
through a job that hands the value down: a workflow then cannot forget
to ask for a pin it uses, which is how a release workflow once built
with no LLVM version at all. The default output is `key=value` lines
for `>> "$GITHUB_OUTPUT"`, and `--get KEY` prints one value for a shell
substitution.

The keys are named here rather than read blindly, so a key removed from
config.toml fails this script instead of reaching a step as an empty
string.
"""

import argparse
import sys
import tomllib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

KEYS = (
    "llvm_toolchain_version",
    "llvm_fork_tag",
    "taplo_version",
    "tree_sitter_cli_version",
    "bun_version",
)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Print the pinned versions config.toml carries."
    )
    parser.add_argument("--get", choices=KEYS, help="print only this key's value")
    args = parser.parse_args()

    with open(REPO_ROOT / "config.toml", "rb") as f:
        config = tomllib.load(f)

    missing = [key for key in KEYS if key not in config]
    if missing:
        print(f"config.toml is missing: {', '.join(missing)}", file=sys.stderr)
        return 1

    if args.get is not None:
        print(config[args.get])
        return 0

    for key in KEYS:
        print(f"{key}={config[key]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
