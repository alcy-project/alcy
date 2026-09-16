#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

import argparse
import os
import re
import subprocess
import sys


def sanitize_symbol_name(name: str) -> str:
    """Replaces non-alphanumeric characters with underscores, matching llvm-objcopy's behavior."""
    return re.sub(r"[^a-zA-Z0-9]", "_", name)


def main():
    parser = argparse.ArgumentParser(description="Embed binary asset into object file.")
    parser.add_argument("--input", required=True, help="Input asset file")
    parser.add_argument("--output", required=True, help="Output object (.o) file")
    parser.add_argument(
        "--target-cpu",
        default="x64",
        choices=["x64", "x86", "arm64", "arm", "riscv64"],
        help="Target CPU architecture",
    )
    parser.add_argument(
        "--objcopy",
        default="objcopy",
        help="Path or command name for objcopy",
    )
    args = parser.parse_args()

    # Map current_cpu to llvm-objcopy binary target format
    # Defaulting to ELF for Linux targets
    arch_map = {
        "x64": ("elf64-x86-64", "i386:x86-64"),
        "x86": ("elf32-i386", "i386"),
        "arm64": ("elf64-littleaarch64", "aarch64"),
        "arm": ("elf32-littlearm", "arm"),
        "riscv64": ("elf64-littleriscv", "riscv"),
    }

    if args.target_cpu not in arch_map:
        print(f"Error: Unsupported target CPU {args.target_cpu}")
        sys.exit(1)

    bfd_output, bfd_arch = arch_map[args.target_cpu]

    raw_symbol_base = sanitize_symbol_name(args.input)
    old_start = f"_binary_{raw_symbol_base}_start"
    old_end = f"_binary_{raw_symbol_base}_end"
    old_size = f"_binary_{raw_symbol_base}_size"

    file_basename = os.path.basename(args.input)
    clean_symbol_base = sanitize_symbol_name(file_basename)
    new_start = f"_binary_{clean_symbol_base}_start"
    new_end = f"_binary_{clean_symbol_base}_end"
    new_size = f"_binary_{clean_symbol_base}_size"

    cmd = [
        args.objcopy,
        "-I",
        "binary",
        "-O",
        bfd_output,
        "-B",
        bfd_arch,
        "--redefine-sym",
        f"{old_start}={new_start}",
        "--redefine-sym",
        f"{old_end}={new_end}",
        "--redefine-sym",
        f"{old_size}={new_size}",
        args.input,
        args.output,
    ]

    result = subprocess.run(cmd)
    if result.returncode != 0:
        sys.exit(result.returncode)


if __name__ == "__main__":
    main()
