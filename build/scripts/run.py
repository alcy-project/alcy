#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
import os
import shutil
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
        "--fast",
        action="store_true",
        help="Skip gn gen, gn check, and compdb before building "
        "(iteration only, never for CI)",
    )
    parser.add_argument(
        "run_args",
        nargs=argparse.REMAINDER,
        help="Arguments to pass to the executable (use '--' before run_args if passing flags)",
    )
    args = parser.parse_args()

    ret = build(
        args.target,
        args.mode,
        args.clang,
        args.lld,
        args.build_subdir,
        args.target_os,
        args.target_cpu,
        False,
        args.fast,
    )
    if ret != 0:
        return ret

    if args.target != "default":
        build_dir = project_root_dir / "out" / args.build_subdir
        candidates = [
            build_dir / args.target,
            build_dir / (args.target + ".exe"),
            build_dir / (args.target + ".js"),
        ]
        target_bin = next((c for c in candidates if c.is_file()), None)

        if target_bin is not None:
            print(f"Running '{target_bin.name}'")
            env = dict(os.environ)
            # LLVM's target initialization trips a known
            # libc++ container-overflow false positive under ASan;
            # user overrides win.
            if "ASAN_OPTIONS" not in env:
                env["ASAN_OPTIONS"] = "detect_container_overflow=0"
            if target_bin.suffix == ".js":
                # Prefer bun over node for faster startup and TypeScript support
                bun_path = shutil.which("bun")
                node_path = shutil.which("node")
                runtime = bun_path or node_path
                if runtime is None:
                    print(
                        "error: neither 'bun' nor 'node' found; cannot run .js binary",
                        file=sys.stderr,
                    )
                    return 1
                cmd = [runtime, str(target_bin)] + args.run_args
            else:
                cmd = [str(target_bin)] + args.run_args
            result = subprocess.run(cmd, cwd=build_dir, env=env)
            return result.returncode
        else:
            print(
                f"error: '{args.target}' is not binary "
                f"(looked for {', '.join(str(c) for c in candidates)})",
                file=sys.stderr,
            )
            return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted by user. Exiting immediately...", file=sys.stderr)
        os._exit(130)
