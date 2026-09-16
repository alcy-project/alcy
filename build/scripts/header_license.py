#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

import re
from datetime import datetime
from pathlib import Path
from re import Pattern
from utils.paths import (
    project_source_dirs,
    scripts_dir,
)
from utils.source import (
    source_extensions,
    script_extensions,
)

holder = "The Alcy Project Authors"
license_text_c = f"""// Copyright {datetime.now().year} {holder}
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.
"""

license_text_py = f"""# Copyright {datetime.now().year} {holder}
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.
"""

license_pattern_c = re.compile(r"//\s*Copyright\s+\d{4}\s+" + re.escape(holder))
license_pattern_py = re.compile(r"#\s*Copyright\s+\d{4}\s+" + re.escape(holder))


def apply_license(
    file_path: Path, license_regex: Pattern[str], license_text: str, dry_run: bool
):
    assert file_path.is_file()
    try:
        content = file_path.read_text(encoding="utf-8")

        if license_regex.search(content):
            return
        else:
            new_content = license_text + "\n" + content
            if dry_run:
                print(f"License header not found: {file_path} (dry run)")
                pass
            else:
                print(f"Applying license to: {file_path}")
                file_path.write_text(new_content.lstrip(), encoding="utf-8")

    except Exception as e:
        print(f"Error processing {file_path}: {e}")


def apply_to_files(dry_run: bool):
    target_dirs: list[Path] = project_source_dirs

    applied: bool = False
    for target in target_dirs:
        if not target.is_dir():
            print(f"Directory not found: {target}")
            continue

        for file_path in target.rglob("*"):
            if file_path.is_file():
                if file_path.suffix in source_extensions:
                    apply_license(file_path, license_pattern_c, license_text_c, dry_run)
                    applied = True

    for file_path in scripts_dir.rglob("*"):
        if file_path.is_file():
            if file_path.suffix in script_extensions:
                apply_license(file_path, license_pattern_py, license_text_py, dry_run)
                applied = True

    if not applied:
        print("None of the files were applied header license")


def main():
    import sys

    dry_run = False
    if len(sys.argv) == 2 and sys.argv[1] == "--dry-run":
        print("dry run enabled")
        dry_run = True
    apply_to_files(dry_run)


if __name__ == "__main__":
    main()
