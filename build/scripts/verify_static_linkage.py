#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

from __future__ import annotations

import argparse
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

LLVM_DYNAMIC_RE = re.compile(
    r"libLLVM[^\s]*\.(?:so|dylib|dll)",
    re.IGNORECASE,
)
LINUX_STDLIB_DYNAMIC_RE = re.compile(
    r"(?:libc\+\+|libstdc\+\+)[^\s]*\.so",
    re.IGNORECASE,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Verify that compiler binaries do not dynamically link LLVM "
            "or the C++ standard library."
        )
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        required=True,
        help="Build output directory to inspect.",
    )
    return parser.parse_args()


def detect_platform() -> str:
    system = platform.system()

    if system == "Linux":
        return "linux"

    if system == "Darwin":
        return "macos"

    if system == "Windows":
        return "windows"

    raise RuntimeError(f"unsupported platform: {system}")


def is_candidate_binary(path: Path, target_platform: str) -> bool:
    if not path.is_file():
        return False

    if target_platform == "windows":
        return path.suffix.lower() == ".exe"

    return bool(path.stat().st_mode & 0o111)


def is_supported_binary(path: Path, target_platform: str) -> bool:
    try:
        result = subprocess.run(
            ["file", str(path)],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError(f"failed to inspect file type: {path}") from exc

    description = result.stdout

    if target_platform == "linux":
        return "ELF" in description

    if target_platform == "macos":
        return "Mach-O" in description

    if target_platform == "windows":
        return "PE32" in description or "PE32+" in description

    raise AssertionError(f"unsupported platform: {target_platform}")


def get_needed_libraries(path: Path) -> str:
    llvm_readobj = shutil.which("llvm-readobj")
    if llvm_readobj is None:
        raise RuntimeError("llvm-readobj was not found in PATH")

    result = subprocess.run(
        [llvm_readobj, "--needed-libs", str(path)],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout


def verify_binary(path: Path, target_platform: str) -> bool:
    print(f"==> {path}")

    needed = get_needed_libraries(path)

    if not needed.strip():
        print("No dynamic libraries found.")
        return True

    print(needed, end="" if needed.endswith("\n") else "\n")

    failed = False

    # LLVM backend must always be statically linked.
    if LLVM_DYNAMIC_RE.search(needed):
        print(
            f"error: LLVM backend is dynamically linked: {path}",
            file=sys.stderr,
        )
        failed = True

    # The system C++ standard library is dynamically linked on macOS by design.
    if target_platform == "linux" and LINUX_STDLIB_DYNAMIC_RE.search(needed):
        print(
            f"error: C++ standard library is dynamically linked: {path}",
            file=sys.stderr,
        )
        failed = True

    return not failed


def main() -> int:
    args = parse_args()
    target_platform = detect_platform()

    print(f"Platform: {target_platform}")
    print(f"Build directory: {args.build_dir}")

    if not args.build_dir.is_dir():
        print(
            f"error: build directory does not exist: {args.build_dir}",
            file=sys.stderr,
        )
        return 1

    if shutil.which("file") is None:
        print("error: 'file' was not found in PATH", file=sys.stderr)
        return 1

    if shutil.which("llvm-readobj") is None:
        print("error: 'llvm-readobj' was not found in PATH", file=sys.stderr)
        return 1

    binaries = [
        path
        for path in args.build_dir.rglob("*")
        if is_candidate_binary(path, target_platform)
    ]

    if not binaries:
        print(
            f"error: no candidate binaries found under {args.build_dir}",
            file=sys.stderr,
        )
        return 1

    failed = False
    checked = 0

    for path in sorted(binaries):
        if not is_supported_binary(path, target_platform):
            continue

        checked += 1
        if not verify_binary(path, target_platform):
            failed = True

    if checked == 0:
        print(
            f"error: no supported binaries found under {args.build_dir}",
            file=sys.stderr,
        )
        return 1

    if failed:
        print("Static linkage verification failed.", file=sys.stderr)
        return 1

    print(f"Static linkage verification passed for {checked} binary/binaries.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
