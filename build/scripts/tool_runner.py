#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import sys
import subprocess
import os


def main():
    if len(sys.argv) < 2:
        print("Usage: tool_runner.py [--cwd <dir>] <executable> [args...]")
        sys.exit(1)

    args_start = 1
    work_dir = None

    if sys.argv[1] == "--cwd":
        if len(sys.argv) < 4:
            print("Usage: tool_runner.py --cwd <dir> <executable> [args...]")
            sys.exit(1)
        work_dir = os.path.abspath(sys.argv[2])
        args_start = 3

    raw_exe = sys.argv[args_start]
    args = sys.argv[args_start + 1 :]

    if not os.path.isabs(raw_exe) and not os.path.dirname(raw_exe):
        exe = os.path.join(".", raw_exe)
    else:
        exe = raw_exe

    exe = os.path.abspath(exe)

    if not os.path.exists(exe):
        # print(f"Error: Binary not found at: {exe}")
        # sys.exit(1)
        exe = raw_exe

    cmd = [exe] + args

    result = subprocess.run(cmd, text=True, capture_output=True, cwd=work_dir)

    if result.stdout.strip():
        print(result.stdout)

    if result.stderr.strip():
        print(result.stderr)

    if result.returncode != 0:
        print(f"Failed with exit code {result.returncode}: {' '.join(cmd)}")
        sys.exit(result.returncode)


if __name__ == "__main__":
    main()
