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

# script flags
#
#   --nix          run all checks in nix develop environment
#   --wasm         also build and run the tests as WebAssembly (needs
#                  Emscripten and node)
#   --no-sanitize  skip the sanitized build of the exe cases (needs clang,
#                  and roughly doubles their runtime)
#   --no-coverage  skip the coverage ratchet (needs llvm-cov, and rebuilds
#                  the tests and the compiler with instrumentation)
#   --no-grammar   skip the tree-sitter checks (needs the tree-sitter CLI
#                  and node, which build the grammar)
nix=false
run_wasm=false
run_sanitize=true
run_coverage=true
run_grammar=true
for flag in "$@"; do
  case "$flag" in
    --nix) nix=true ;;
    --wasm) run_wasm=true ;;
    --no-wasm) run_wasm=false ;;
    --no-sanitize) run_sanitize=false ;;
    --no-coverage) run_coverage=false ;;
    --no-grammar) run_grammar=false ;;
    *) echo "error: unknown flag '$flag'" >&2; exit 1 ;;
  esac
done

if command -v typos >/dev/null 2>&1; then
  typos
else
  # The other optional tools in this script either refuse or say so;
  # `typos` was the one that skipped in silence, which is the one way a
  # skipped check can be mistaken for a check that passed.
  echo "note: typos not on PATH, so the spell check did not run" >&2
fi

# Enter nix develop shell if nix = true and nix is available and not already inside
if [[ $nix == true ]] && [ -z "${IN_NIX_SHELL:-}" ] && command -v nix >/dev/null 2>&1; then
  exec nix develop -c "$0" "$@"
fi

if [[ $nix == true ]]; then
  release_subdir="build_release_nix"
  debug_subdir="build_nix"
  wasm_subdir="build_wasm_nix"
else
  release_subdir="build_release"
  debug_subdir="build"
  wasm_subdir="build_wasm"
fi

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

"${py_runner[@]}" "$tools_dir/check_borrow_rules.py" \
  --build-subdir=$debug_subdir

# The specification is what a program is written against, and the compiler
# is how this one happens to implement it. That boundary is prose in
# `docs/spec/overview.md` and a check here, since a reference that creeps
# in is the kind of thing nobody notices until the thing it names moves.
"${py_runner[@]}" "$tools_dir/check_spec.py"

# The checks below read the tree rather than build it. Each names the
# tool it could not find: a check that does not run must not read as one
# that passed.
if command -v taplo >/dev/null 2>&1; then
  taplo fmt --check
else
  echo "note: taplo not on PATH, so the TOML check did not run" >&2
fi

if [ "${py_runner[0]}" = "uv" ]; then
  "${py_runner[@]}" ast-grep test
  "${py_runner[@]}" ast-grep scan --error compiler
  "${py_runner[@]}" rumdl check .
  "${py_runner[@]}" ruff check tools
  "${py_runner[@]}" ruff format --check tools
else
  echo "note: uv not found, so the rule, Markdown and Python checks did not run" >&2
fi

# The tree-sitter grammar is a second reading of the language, so it is
# checked against the first: its own corpus here, and every .al file in the
# repository against the binary the checks above just built. It needs no
# toolchain beyond the CLI, which is why the grammar half is not behind the
# build.
if [[ $run_grammar == true ]]; then
  command -v tree-sitter >/dev/null 2>&1 || {
    echo "error: tree-sitter not found; pass --no-grammar to skip" >&2
    exit 1
  }
  "${py_runner[@]}" "$tools_dir/check_treesitter.py" --grammar
  "${py_runner[@]}" "$tools_dir/check_treesitter.py" \
    --differential \
    --build-subdir=$debug_subdir
fi

# The samples run through the harness the cases above use, so a program
# kept as an example is also one that still compiles and still prints
# what it says it prints. Here rather than with the cases because these
# are read as much as run, and a sample that is wrong is a
# documentation fault.
"${py_runner[@]}" "$tools_dir/check_exe.py" \
  --build-subdir=$debug_subdir \
  --cases-root=samples

# The same cases again through --emit=llvm-ir, so the code alcy generates
# is compiled by an external toolchain that can instrument it. Slow, so
# it is a separate step, and refused rather than skipped where clang is
# absent.
if [[ $run_sanitize == true ]]; then
  # `check_exe.py --sanitize` refuses without clang rather than degrading,
  # so this gate fails on a machine that has no clang. Naming the flag is
  # the difference between a gate somebody can work with and one they
  # have to read the source of to get past.
  command -v clang >/dev/null 2>&1 || {
    echo "error: clang not found; pass --no-sanitize to skip" >&2
    exit 1
  }
  "${py_runner[@]}" "$tools_dir/check_exe.py" \
    --build-subdir=$debug_subdir \
    --sanitize
fi

"${py_runner[@]}" "$tools_dir/format.py" --dry-run
# Lint reuses the debug build's compile database: a dedicated GN tree
# would install its own copy of LLVM for the same bytes.
"${py_runner[@]}" "$tools_dir/lint.py" --build-subdir="$debug_subdir"

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
