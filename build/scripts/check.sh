#!/usr/bin/env bash

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -e

script_dir=$(dirname "$0")
cd "$script_dir/../.." && root_dir=$(pwd)
tool_scripts_dir="$root_dir/build/scripts"

# Pass --wasm to also build and run the tests as WebAssembly
# (requires Emscripten and node on PATH).
run_wasm=false
if [[ "${1:-}" == "--wasm" ]]; then
  run_wasm=true
fi

typos

if [ -z "$IN_NIX_SHELL" ]; then
  exec nix develop -c "$0" "$@"
fi

release_subdir="build_release"
debug_subdir="build"
wasm_subdir="build_wasm"

uv run "$tool_scripts_dir/build.py" \
  --target=all \
  --mode=release \
  --build-subdir=$release_subdir

uv run "$tool_scripts_dir/build.py" \
  --target=all \
  --mode=debug \
  --build-subdir=$debug_subdir

uv run "$tool_scripts_dir/run.py" \
  --target=tests \
  --mode=debug \
  --build-subdir=$debug_subdir \
  -- --no-skip

uv run "$tool_scripts_dir/e2e.py" \
  --build-subdir=$debug_subdir

uv run "$tool_scripts_dir/check_runtime.py"

uv run "$tool_scripts_dir/check_exe.py" \
  --build-subdir=$debug_subdir

uv run "$tool_scripts_dir/format.py" --dry-run
uv run "$tool_scripts_dir/lint.py"

uv run "$tool_scripts_dir/verify_static_linkage.py" \
  --build-dir="$root_dir/out/$release_subdir"
uv run "$tool_scripts_dir/verify_static_linkage.py" \
  --build-dir="$root_dir/out/$debug_subdir"

if [[ $run_wasm == true ]]; then
  command -v emcc >/dev/null || {
    echo "error: emcc not found; install Emscripten first" >&2
    exit 1
  }
  command -v node >/dev/null || {
    echo "error: node not found; install node first" >&2
    exit 1
  }
  uv run "$tool_scripts_dir/run.py" \
    --target=tests \
    --mode=debug \
    --build-subdir=$wasm_subdir \
    --target-os=emscripten
fi

echo "check ok"
