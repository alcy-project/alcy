#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Lists source files under the given paths, for GN action inputs.

The version action needs a file list at generate time and a content
digest when it runs. Keeping the two walks in one script means they
cannot disagree about what makes the compiler what it is.
"""

import sys

from utils.paths import project_root_dir

SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".gn", ".gni", ".h", ".hpp", ".py"})


def main() -> int:
    for root in sys.argv[1:]:
        base = project_root_dir / root
        if not base.exists():
            continue
        paths = [base] if base.is_file() else base.rglob("*")
        for path in sorted(p for p in paths if p.is_file()):
            if path.suffix in SOURCE_SUFFIXES:
                print("//" + path.relative_to(project_root_dir).as_posix())
    return 0


if __name__ == "__main__":
    sys.exit(main())
