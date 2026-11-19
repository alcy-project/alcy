#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Configure and build for every target operating system or architecture.

--gen-only stops after the cross-os half: configuring every target
operating system costs a minute and is what catches a toolchain
definition that cannot produce one of them, which is the half CI
schedules; the arch builds below are a local cross-compile workout.
"""

import argparse
import os
import subprocess
import sys

from utils.paths import project_root_dir

TOOLS_DIR = project_root_dir / "tools"
MODES = ("debug", "release")
GEN_ONLY_TARGETS = ("linux", "win", "mac")
ARCH_TARGETS = ("x64", "x86", "arm64", "arm", "riscv64")


def build(build_subdir: str, mode: str, *args: str) -> None:
    command = [
        "uv",
        "run",
        str(TOOLS_DIR / "build.py"),
        f"--mode={mode}",
        f"--build-subdir={build_subdir}",
        *args,
    ]
    if subprocess.run(command).returncode != 0:
        sys.exit(f"error: {' '.join(command)} failed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--gen-only",
        action="store_true",
        help="configure every target operating system and stop",
    )
    args = parser.parse_args()

    os.chdir(project_root_dir)

    if args.gen_only:
        print("running cross os gen-only check...", flush=True)
        for os_name in GEN_ONLY_TARGETS:
            for mode in MODES:
                build(
                    f"cross-{os_name}-{mode}",
                    mode,
                    "--gen-only",
                    f"--target-os={os_name}",
                )
        print("cross gen check ok", flush=True)
        return 0

    print("running cross arch build test...", flush=True)
    for arch in ARCH_TARGETS:
        for mode in MODES:
            build(f"cross-{arch}-{mode}", mode, "--target=all", f"--target-cpu={arch}")
    print("cross build check ok", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
