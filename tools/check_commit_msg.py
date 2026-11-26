#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Checks a commit message against the convention in CONTRIBUTING.md.

The commit-msg hook passes the path to the message file. `--self-test`
runs the cases below against the validator without touching git.
"""

import argparse
import re
import sys
from pathlib import Path

from utils import timing

TYPES = (
    "feat",
    "fix",
    "refactor",
    "perf",
    "test",
    "docs",
    "build",
    "ci",
    "chore",
    "style",
)

SUBJECT = re.compile(rf"^({'|'.join(TYPES)})(\([a-z0-9_-]+\))?!?: (.+)$")

# Git writes these itself; a hook that rejects them breaks merges and
# autosquash rather than helping the message.
GENERATED = ("Merge ", "Revert ", "fixup! ", "squash! ")

TARGET = 72
CAP = 100


def message_lines(text: str) -> list[str]:
    """The lines Git keeps: no template comments, nothing after scissors."""
    lines: list[str] = []
    for line in text.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("#"):
            if ">8" in line:
                break
            continue
        lines.append(line)
    return lines


def check(text: str) -> list[str]:
    """Returns the problems with `text`, empty when it is acceptable."""
    lines = message_lines(text)
    if not lines or not lines[0].strip():
        return ["the message has no subject"]

    subject = lines[0]
    if subject.startswith(GENERATED):
        return []

    problems: list[str] = []
    match = SUBJECT.match(subject)
    if match is None:
        problems.append(
            f"subject must be '<type>(<scope>)!?: <subject>' with one of: "
            f"{', '.join(TYPES)}"
        )
        description = subject
    else:
        description = match.group(3)

    if description and description[0].isupper():
        problems.append("the subject starts with a capital; use lowercase")
    if subject.endswith("."):
        problems.append("the subject has a trailing period")
    if not subject.isascii():
        problems.append("commit messages are written in English (ASCII)")

    if len(subject) > CAP:
        problems.append(f"subject is {len(subject)} columns; the cap is {CAP}")
    elif len(subject) > TARGET:
        problems.append(f"subject is {len(subject)} columns; aim for {TARGET} or fewer")

    for index, line in enumerate(lines[1:], start=2):
        if len(line) > CAP:
            problems.append(f"line {index} is {len(line)} columns; the cap is {CAP}")
        elif len(line) > TARGET:
            problems.append(
                f"line {index} is {len(line)} columns; aim for {TARGET} or fewer"
            )
    return problems


SELF_TESTS = (
    ("fix(cli): report a missing entry point\n", 0),
    ("docs: say what the alcy command is\n\nOne binary, one version.\n", 0),
    ("Merge branch 'main' into local/ci\n", 0),
    ("Fixed the at_mut issue\n", 1),
    ("fix: trailing period.\n", 1),
    ("fix: Capitalised subject\n", 1),
    ("feat(census): " + "x" * 100 + "\n", 1),
    ("fix: " + "x" * 71 + "\n\n" + "y" * 101 + "\n", 1),
    ("feat!: drop the old flag\n", 0),
    ("feat(cli)!: change the interface\n", 0),
    ("feat!(cli): bad marker\n", 1),
    ("fix: keep it\n\n# " + "x" * 120 + "\n", 0),
    (
        "fix: keep it\n\n# ------------------------ >8 ------------------------\n"
        + "y" * 120
        + "\n",
        0,
    ),
    ("\n", 1),
)


def self_test() -> int:
    failures = 0
    for text, expected in SELF_TESTS:
        problems = check(text)
        got = 0 if not problems else 1
        if got != expected:
            failures += 1
            print(f"FAIL {text.splitlines()[0]!r}: {problems}")
    if failures:
        print(f"check_commit_msg: {failures} self-test(s) failed")
        return 1
    print(f"check_commit_msg: {len(SELF_TESTS)} self-test(s) passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("message", nargs="?", help="path to the commit message file")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        code = self_test()
        timing.mark("self-test")
        return code
    if not args.message:
        parser.error("give the commit message file, or --self-test")

    text = Path(args.message).read_text(encoding="utf-8", errors="replace")
    timing.mark("read")
    problems = check(text)
    timing.mark("check")
    if not problems:
        return 0
    for problem in problems:
        print(f"commit message: {problem}", file=sys.stderr)
    print("see CONTRIBUTING.md, 'Changes and review'", file=sys.stderr)
    return 1


if __name__ == "__main__":
    from utils.timing import run_with_timings

    sys.exit(run_with_timings(main))
