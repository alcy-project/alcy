#!/usr/bin/env bash

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -e

script_dir=$(dirname "$0")
cd "$script_dir/../.." && root_dir=$(pwd)
tool_scripts_dir="$root_dir/build/scripts"

echo "running cross os gen-only check..."
for os in linux win mac; do
  for mode in debug release; do
    uv run "$tool_scripts_dir/build.py" \
      --gen-only \
      --mode=$mode \
      --build-subdir="cross-$os-$mode" \
      --target-os=$os
  done
done

echo "running cross arch build test..."
for arch in x64 x86 arm64 arm riscv64; do
  for mode in debug release; do
    uv run "$tool_scripts_dir/build.py" \
      --target=all \
      --mode=$mode \
      --build-subdir="cross-$arch-$mode" \
      --target-cpu=$arch
  done
done

echo "cross build check ok"

