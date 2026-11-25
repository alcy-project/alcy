#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

from utils.paths import project_root_dir
from utils.source import source_extensions


def shared_llvm_dir(mode: str, target_os: str) -> str:
    """The pinned LLVM install shared between worktrees, when available.

    An explicit `--llvm-dir` wins. This helper is only asked for a path
    when the caller did not name one. A failure to populate the cache is
    not a build failure: GN falls back to its own per-worktree setup.
    """
    tools_dir = str(Path(__file__).resolve().parent)
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
    try:
        from llvm import cache
        from utils.config import get_llvm_fork_tag

        tag = get_llvm_fork_tag()
        triple = (
            "wasm32-unknown-emscripten"
            if "emscripten" in target_os
            else cache.host_triple()
        )
        build_type = "Debug" if mode == "debug" else "Release"
        found = cache.resolve(tag, triple, build_type)
        return str(found) if found is not None else ""
    except ImportError, OSError, ValueError:
        return ""


def update_compdb(build_dir) -> bool:
    try:
        compdb_result = subprocess.run(
            ["ninja", "-C", str(build_dir), "-t", "compdb"],
            capture_output=True,
            text=True,
            check=True,
            cwd=project_root_dir,
        )
        # ninja emits an entry per target, not per compilation, so the
        # output also holds phony targets, archives, link steps and script
        # invocations. Some of those share a name with a source, which is
        # enough for a consumer matching a header to a command to pick a
        # link line and then fail to resolve anything. An entry belongs in
        # a compilation database when it names a source.
        entries = [
            entry
            for entry in json.loads(compdb_result.stdout)
            if entry.get("command")
            and os.path.splitext(entry["file"])[1] in source_extensions
        ]
        (build_dir / "compile_commands.json").write_text(json.dumps(entries))
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
    gn_args_extra: str = "",
    llvm_dir: str = "",
    llvm_cache: bool = True,
    debug_info: str = "line",
    sanitizer: str = "address",
    env: dict[str, str] | None = None,
) -> int:
    out_dir = project_root_dir / "out"
    build_dir = out_dir / build_subdir

    is_debug = "true" if mode == "debug" else "false"

    gn_args = f"is_debug={is_debug} is_clang={is_clang} use_lld={use_lld}"
    # Forwarded even in release, where the mode decides the flags and the
    # argument is inert: one spelling keeps a build directory's recorded
    # arguments honest about what it was configured with.
    gn_args += f' alcy_debug_info="{debug_info}"'
    # Inert in release, where no sanitizer is added; forwarded for the same
    # reason as the debug information.
    gn_args += f' alcy_sanitizer="{sanitizer}"'
    if not llvm_dir and llvm_cache:
        llvm_dir = shared_llvm_dir(mode, target_os)
    if llvm_dir:
        # A preinstalled LLVM prefix instead of the tag artifact.
        # Absolute and forward-slashed, because gn resolves it from the
        # build directory and treats backslashes as string escapes on
        # Windows.
        prefix = os.path.abspath(llvm_dir).replace(os.sep, "/")
        gn_args += f' llvm_install_dir="{prefix}"'
    if target_os:
        gn_args += f' target_os="{target_os}"'
    if target_cpu:
        gn_args += f' target_cpu="{target_cpu}"'
    # Forwarded verbatim, so an opt-in target (the fuzzers, which need
    # libFuzzer) can be built without invoking gn by hand.
    if gn_args_extra:
        gn_args += f" {gn_args_extra}"

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
                env=env,
            )

            # gn check
            subprocess.run(
                ["gn", "check", str(build_dir), "//src/*"],
                check=True,
                cwd=project_root_dir,
                env=env,
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
            env=env,
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
        "--llvm-dir",
        default="",
        help="Use a preinstalled LLVM prefix (with include/ and lib/) "
        "instead of downloading the fork's tag artifact",
    )
    parser.add_argument(
        "--no-llvm-cache",
        action="store_true",
        help="Keep LLVM inside this worktree instead of sharing the pinned "
        "install through the OS cache. Also respects ALCY_LLVM_CACHE=0",
    )
    parser.add_argument(
        "--debug-info",
        default="line",
        choices=["line", "full"],
        help="Debug information in a debug build: line tables (default) "
        "or the full macro and type detail -g3 carries",
    )
    parser.add_argument(
        "--sanitizer",
        default="address",
        choices=["address", "thread", "none"],
        help="Sanitizer in a debug build: the address sanitizer (default), "
        "the thread sanitizer, which is what a race in the job system "
        "needs to be seen, or none when the pinned toolchain has no "
        "runtime that works on this host",
    )
    parser.add_argument(
        "--gn-arg",
        dest="gn_args_extra",
        default="",
        help="Extra GN arguments, passed through verbatim (e.g. --gn-arg=is_fuzz=true)",
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
        args.gn_args_extra,
        args.llvm_dir,
        llvm_cache=not args.no_llvm_cache,
        debug_info=args.debug_info,
        sanitizer=args.sanitizer,
    )


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted by user. Exiting immediately...", file=sys.stderr)
        os._exit(130)
