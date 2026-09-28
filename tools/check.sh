#!/usr/bin/env bash

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -e

# Determine Python runner tool or fall back to python3
if command -v uv >/dev/null 2>&1; then
  py_runner=(uv run)
elif command -v python3 >/dev/null 2>&1; then
  py_runner=(python3)
else
  echo "error: uv or python3 is required to run this script" >&2
  exit 1
fi

script_dir=$(dirname "$0")
cd "$script_dir/.." && root_dir=$(pwd)
tools_dir="$root_dir/tools"

# Switches rather than a mode, so any order and any subset work.
#
#   --wasm         also build and run the tests as WebAssembly (needs
#                  Emscripten and node)
#   --no-sanitize  skip the sanitized build of the exe cases (needs clang,
#                  and roughly doubles their runtime)
#   --no-coverage  skip the coverage ratchet (needs llvm-cov, and rebuilds
#                  the tests and the compiler with instrumentation)
run_wasm=false
run_sanitize=true
run_coverage=true
for flag in "$@"; do
  case "$flag" in
    --wasm) run_wasm=true ;;
    --no-wasm) run_wasm=false ;;
    --no-sanitize) run_sanitize=false ;;
    --no-coverage) run_coverage=false ;;
    *) echo "error: unknown flag '$flag'" >&2; exit 1 ;;
  esac
done

if command -v typos >/dev/null 2>&1; then
  typos
fi

# Enter nix develop shell if nix is available and not already inside
if [ -z "${IN_NIX_SHELL:-}" ] && command -v nix >/dev/null 2>&1; then
  exec nix develop -c "$0" "$@"
fi

release_subdir="build_release"
debug_subdir="build"
wasm_subdir="build_wasm"

"${py_runner[@]}" "$tools_dir/build.py" \
  --target=all \
  --mode=debug \
  --build-subdir=$debug_subdir

"${py_runner[@]}" "$tools_dir/run.py" \
  --target=tests \
  --mode=debug \
  --build-subdir=$debug_subdir \
  -- --no-skip

"${py_runner[@]}" "$tools_dir/build.py" \
  --target=all \
  --mode=release \
  --build-subdir=$release_subdir

"${py_runner[@]}" "$tools_dir/check_e2e.py" \
  --build-subdir=$debug_subdir

"${py_runner[@]}" "$tools_dir/check_runtime.py"

"${py_runner[@]}" "$tools_dir/check_exe.py" \
  --build-subdir=$debug_subdir

# The same cases again through --emit=llvm-ir, so the code alcy generates
# is compiled by an external toolchain that can instrument it. Skipped
# where clang is absent, and slow, so it is a separate step.
if [[ $run_sanitize == true ]]; then
  "${py_runner[@]}" "$tools_dir/check_exe.py" \
    --build-subdir=$debug_subdir \
    --sanitize
fi

"${py_runner[@]}" "$tools_dir/format.py" --dry-run
"${py_runner[@]}" "$tools_dir/lint.py"

# The coverage ratchet rebuilds instrumented binaries, so it runs after
# the format and lint gates and reuses its own output directory. Skip it
# with --no-coverage when llvm-cov is not available.
if [[ $run_coverage == true ]]; then
  command -v llvm-cov >/dev/null 2>&1 || {
    echo "error: llvm-cov not found; pass --no-coverage to skip" >&2
    exit 1
  }
  "${py_runner[@]}" "$tools_dir/check_coverage.py"
fi

"${py_runner[@]}" "$tools_dir/verify_static_linkage.py" \
  --build-dir="$root_dir/out/$release_subdir"
"${py_runner[@]}" "$tools_dir/verify_static_linkage.py" \
  --build-dir="$root_dir/out/$debug_subdir"

if [[ $run_wasm == true ]]; then
  command -v emcc >/dev/null 2>&1 || {
    echo "error: emcc not found; install Emscripten first" >&2
    exit 1
  }
  command -v node >/dev/null 2>&1 || {
    echo "error: node not found; install node first" >&2
    exit 1
  }
  "${py_runner[@]}" "$tools_dir/run.py" \
    --target=tests \
    --mode=debug \
    --build-subdir=$wasm_subdir \
    --target-os=emscripten
fi

echo "check ok"
