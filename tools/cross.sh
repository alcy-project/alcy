#!/usr/bin/env bash

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -e

script_dir=$(dirname "$0")
cd "$script_dir/.." && root_dir=$(pwd)
tools_dir="$root_dir/tools"

# --gen-only stops after the cross-os half: configuring every target
# operating system costs a minute and is what catches a toolchain
# definition that cannot produce one of them, which is the half CI
# schedules; the arch builds below are a local cross-compile workout.
gen_only=false
for flag in "$@"; do
  case "$flag" in
    --gen-only) gen_only=true ;;
    *) echo "error: unknown flag '$flag'" >&2; exit 1 ;;
  esac
done

echo "running cross os gen-only check..."
for os in linux win mac; do
  for mode in debug release; do
    uv run "$tools_dir/build.py" \
      --gen-only \
      --mode=$mode \
      --build-subdir="cross-$os-$mode" \
      --target-os=$os
  done
done

if [[ $gen_only == true ]]; then
  echo "cross gen check ok"
  exit 0
fi

echo "running cross arch build test..."
for arch in x64 x86 arm64 arm riscv64; do
  for mode in debug release; do
    uv run "$tools_dir/build.py" \
      --target=all \
      --mode=$mode \
      --build-subdir="cross-$arch-$mode" \
      --target-cpu=$arch
  done
done

echo "cross build check ok"

