#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

import argparse
import subprocess
import sys
from build import build
from utils.paths import project_root_dir


def main():
    parser = argparse.ArgumentParser(description="Build and run project targets.")
    parser.add_argument(
        "--target",
        default="default",
        help="Build target (default: default)",
    )
    parser.add_argument(
        "--mode",
        default="debug",
        help="Build mode (default: debug)",
    )
    parser.add_argument(
        "--clang",
        default="true",
        choices=["true", "false"],
        help="Use clang as compiler (default: true)",
    )
    parser.add_argument(
        "--lld",
        default="true",
        choices=["true", "false"],
        help="Use lld as linker (default: true)",
    )
    parser.add_argument(
        "--build-subdir",
        default="build",
        help="Subdirectory inside out/ (default: build)",
    )
    parser.add_argument(
        "run_args",
        nargs=argparse.REMAINDER,
        help="Arguments to pass to the executable (use '--' before run_args if passing flags)",
    )
    args = parser.parse_args()

    ret = build(args.target, args.mode, args.clang, args.lld, args.build_subdir)
    if ret != 0:
        sys.exit(ret)

    if args.target != "default":
        build_dir = project_root_dir / "out" / args.build_subdir
        target_bin = build_dir / args.target

        if target_bin.is_file():
            print(f"Running '{args.target}'")
            cmd = [str(target_bin)] + args.run_args
            result = subprocess.run(cmd, cwd=build_dir)
            sys.exit(result.returncode)
        else:
            print(f"{args.target} is not binary")


if __name__ == "__main__":
    main()
