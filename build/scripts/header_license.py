#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import re
import sys
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
spdx_identifier = "SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception"
copyright_message = f"Copyright {datetime.now().year} {holder}"

license_text_c = f"// {copyright_message}\n// {spdx_identifier}\n"
license_text_py = f"# {copyright_message}\n# {spdx_identifier}\n"

license_pattern_c = re.compile(
    r"//\s*Copyright\s+\d{4}\s+"
    + re.escape(holder)
    + r"\n//\s*"
    + re.escape(spdx_identifier)
)
license_pattern_py = re.compile(
    r"#\s*Copyright\s+\d{4}\s+"
    + re.escape(holder)
    + r"\n#\s*"
    + re.escape(spdx_identifier)
)

any_copyright_pattern_c = re.compile(r"//\s*Copyright")
any_copyright_pattern_py = re.compile(r"#\s*Copyright")


def apply_license(
    file_path: Path,
    valid_regex: Pattern[str],
    any_copyright_regex: Pattern[str],
    license_text: str,
    dry_run: bool,
) -> bool:
    assert file_path.is_file()
    try:
        content = file_path.read_text(encoding="utf-8")

        if valid_regex.search(content):
            return True

        if any_copyright_regex.search(content):
            print(
                f"Error: Invalid license header found in '{file_path}'. "
                f"Please update it to correct format.",
                file=sys.stderr,
            )
            return False

        if content.startswith("#!"):
            parts = content.split("\n", 1)
            shebang = parts[0]
            rest = parts[1] if len(parts) > 1 else ""
            new_content = f"{shebang}\n\n{license_text}\n{rest.lstrip()}"
        else:
            new_content = (license_text + "\n" + content).lstrip()

        if dry_run:
            print(f"License header not found: {file_path} (dry run)")
        else:
            print(f"Applying license to: {file_path}")
            file_path.write_text(new_content, encoding="utf-8")

        return True

    except Exception as e:
        print(f"Error processing {file_path}: {e}", file=sys.stderr)
        return False


def apply_to_files(dry_run: bool) -> bool:
    target_dirs: list[Path] = project_source_dirs

    applied_any: bool = False
    has_error: bool = False

    for target in target_dirs:
        if not target.is_dir():
            print(f"Directory not found: {target}")
            continue

        for file_path in target.rglob("*"):
            if file_path.is_file() and file_path.suffix in source_extensions:
                success = apply_license(
                    file_path,
                    license_pattern_c,
                    any_copyright_pattern_c,
                    license_text_c,
                    dry_run,
                )
                if not success:
                    has_error = True
                applied_any = True

    for file_path in scripts_dir.rglob("*"):
        if file_path.is_file() and file_path.suffix in script_extensions:
            success = apply_license(
                file_path,
                license_pattern_py,
                any_copyright_pattern_py,
                license_text_py,
                dry_run,
            )
            if not success:
                has_error = True
            applied_any = True

    if not applied_any:
        print("No eligible files were found.")

    return not has_error


def main():
    dry_run = False
    if len(sys.argv) == 2 and sys.argv[1] == "--dry-run":
        print("dry run enabled")
        dry_run = True

    success = apply_to_files(dry_run)
    if not success:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
