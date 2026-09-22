#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
import subprocess
import sys
import os
from utils.paths import project_root_dir


def update_compdb(build_dir) -> bool:
    try:
        compdb_result = subprocess.run(
            ["ninja", "-C", str(build_dir), "-t", "compdb"],
            capture_output=True,
            text=True,
            check=True,
            cwd=project_root_dir,
        )
        (build_dir / "compile_commands.json").write_text(compdb_result.stdout)
        return True
    except subprocess.CalledProcessError as e:
        print(f"Failed to generate compdb: {e}", file=sys.stderr)
        return False


def build(
    target: str = "default",
    mode: str = "debug",
    is_clang: str = "true",
    use_lld: str = "true",
    build_subdir: str = "build",
    target_os: str = "",
    target_cpu: str = "",
    gen_only: bool = False,
    fast: bool = False,
) -> int:
    out_dir = project_root_dir / "out"
    build_dir = out_dir / build_subdir

    is_debug = "true" if mode == "debug" else "false"

    gn_args = f"is_debug={is_debug} is_clang={is_clang} use_lld={use_lld}"
    if target_os:
        gn_args += f' target_os="{target_os}"'
    if target_cpu:
        gn_args += f' target_cpu="{target_cpu}"'

    try:
        if not fast:
            # gn gen
            subprocess.run(
                [
                    "gn",
                    "gen",
                    str(build_dir),
                    f"--args={gn_args}",
                ],
                check=True,
                cwd=project_root_dir,
            )

            # gn check
            subprocess.run(
                ["gn", "check", str(build_dir), "//src/*"],
                check=True,
                cwd=project_root_dir,
            )

            # update compdb
            if not update_compdb(build_dir):
                return 1

            if gen_only:
                return 0
        elif gen_only:
            print(
                "error: --fast and --gen-only are mutually exclusive",
                file=sys.stderr,
            )
            return 1

        # ninja target
        subprocess.run(
            ["ninja", "-C", str(build_dir), target],
            check=True,
            cwd=project_root_dir,
        )
        return 0
    except subprocess.CalledProcessError as e:
        print(f"Build failed: {e}", file=sys.stderr)
        return e.returncode


def main():
    parser = argparse.ArgumentParser(description="Build project targets.")
    parser.add_argument(
        "--target",
        default="default",
        help="Build target (default: default)",
    )
    parser.add_argument(
        "--mode",
        default="debug",
        choices=["debug", "release"],
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
        "--target-os",
        default="",
        help='GN target_os override (e.g. "emscripten" for wasm builds)',
    )
    parser.add_argument(
        "--target-cpu",
        default="",
        help='GN target_cpu override (e.g. "wasm32"; defaults per target_os)',
    )
    parser.add_argument(
        "--gen-only",
        action="store_true",
        help="Run gn gen, gn check, and update compdb only (without building target)",
    )
    parser.add_argument(
        "--fast",
        action="store_true",
        help="Skip gn gen, gn check, and compdb; straight to ninja "
        "(iteration only, never for CI)",
    )
    args = parser.parse_args()

    return build(
        args.target,
        args.mode,
        args.clang,
        args.lld,
        args.build_subdir,
        args.target_os,
        args.target_cpu,
        args.gen_only,
        args.fast,
    )


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted by user. Exiting immediately...", file=sys.stderr)
        os._exit(130)
