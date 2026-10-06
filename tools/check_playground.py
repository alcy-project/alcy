#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Smoke test for the playground's wasm build.

Expects `out/<build-subdir>/alcy_playground.js` (and its `.wasm` sidecar)
from:

    tools/build.py --target=playground --target-os=emscripten \\
        --build-subdir=playground --gn-arg='alcy_backends=[]'

and runs `compiler/playground/js/smoke.mjs` under node, which loads the
launcher, compiles a program through the C ABI, runs the module it
produced, and checks a rejected source comes back with diagnostics. The
program's output is compared here, so the whole path -- page wrapper,
ABI, compiler, emitted module -- is one test.
"""

import argparse
import os
import subprocess
import sys

from utils.paths import project_root_dir


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="Run the playground wasm smoke test.")
    parser.add_argument(
        "--build-subdir",
        default="playground",
        help="Subdirectory inside out/ holding the playground build",
    )
    args = parser.parse_args()

    launcher = project_root_dir / "out" / args.build_subdir / "alcy_playground.js"
    if not launcher.is_file():
        print(f"playground launcher not found at {launcher}")
        print("build it with tools/build.py --target=playground --target-os=emscripten")
        return -1

    smoke = project_root_dir / "compiler" / "playground" / "js" / "smoke.mjs"
    env = dict(os.environ)
    env["NODE_NO_WARNINGS"] = "1"
    proc = subprocess.run(
        ["node", str(smoke), str(launcher)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        cwd=project_root_dir,
        env=env,
        timeout=300,
    )
    if proc.returncode != 0:
        print(f"playground smoke failed (exit {proc.returncode}):")
        print(proc.stdout + proc.stderr)
        return 1
    if proc.stdout != "hello, playground\n":
        print(f"playground smoke: got {proc.stdout!r}, want 'hello, playground\\n'")
        return 1
    print("playground: smoke passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
