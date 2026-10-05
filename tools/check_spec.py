#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Keeps the specification independent of the compiler that implements it.

`docs/spec/` states what a program is written against. A rule belongs
there when a program can observe it, and a reader should not have to
know a module, a source file, or a symbol of this compiler to learn what
the language does. Those details drift as the compiler moves, and a
stale one is worse than no one, so the boundary is checked rather than
remembered.

What is rejected is an implementation *file* and an implementation
*symbol*. A link to a document is not: the specification cites the ADR
behind a rule, and the deferred list points at the plan for the rest.
"""

import re
import sys

from utils.paths import project_root_dir

SPEC_DIR = project_root_dir / "docs" / "spec"

# A path to a source file or a build tree. `docs/adr/0007-....md` and
# `compiler/docs/architecture.md` are documents and are left alone.
PATTERNS = {
    "a source or build path": re.compile(
        r"(?<![\w./-])(?:src|compiler|third_party|lib|tools|e2e|exe|samples|out)"
        r"/[\w.*{}<>-]*\.(?:cc|h|py|gn|gni|al|toml|ebnf|def)\b"
    ),
    "a source file name": re.compile(r"\b[\w./-]*\.(?:cc|h|def)\b"),
    "a GN label": re.compile(r"//[a-z_]+/"),
    "a runtime symbol": re.compile(
        r"\balcy_(?:print|println|panic|sys_write|alloc|dealloc|write_all"
        r"|runtime\.text)\b"
    ),
    # A diagnostic code says which component of the compiler answered,
    # which is not a program the specification is written against. The
    # rule belongs here and the spelling belongs to the component.
    "a diagnostic code": re.compile(r"\b[EWN][A-Z]\d{3}\b"),
}


def main() -> int:
    findings = 0
    for path in sorted(SPEC_DIR.rglob("*")):
        if not path.is_file() or path.suffix not in {".md", ".ebnf"}:
            continue
        relative = path.relative_to(project_root_dir)
        for number, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
            for description, pattern in PATTERNS.items():
                match = pattern.search(line)
                if match is None:
                    continue
                print(f"{relative}:{number}: {description}: {match.group(0)}")
                findings += 1
    if findings:
        print(
            f"the specification names the implementation in {findings} place(s); "
            "state the rule, and let the compiler's own docs carry the detail"
        )
        return 1
    print(
        "the specification names no implementation "
        f"({SPEC_DIR.relative_to(project_root_dir)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
