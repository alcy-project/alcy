#!/usr/bin/env bash

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

set -e

script_dir=$(dirname "$0")
cd "$script_dir/../.." && root_dir=$(pwd)

typos

if [ -z "$IN_NIX_SHELL" ]; then
  exec nix develop -c "$0" "$@"
fi

release_subdir="build_release"
debug_subdir="build"

uv run "$root_dir/build/scripts/build.py" \
  --target=all \
  --mode=release \
  --build-subdir=$release_subdir

uv run "$root_dir/build/scripts/build.py" \
  --target=all \
  --mode=debug \
  --build-subdir=$debug_subdir

uv run "$root_dir/build/scripts/run.py" \
  --target=tests \
  --mode=debug \
  --build-subdir=$debug_subdir \
  -- --no-skip

uv run "$root_dir/build/scripts/format.py" --dry-run
uv run "$root_dir/build/scripts/lint.py"

uv run "$root_dir/build/scripts/verify_static_linkage.py" \
  --build-dir="$root_dir/out/$release_subdir"
uv run "$root_dir/build/scripts/verify_static_linkage.py" \
  --build-dir="$root_dir/out/$debug_subdir"
