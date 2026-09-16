#!/usr/bin/env bash

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

set -e

target=${1:-"default"}
build_subdir=${2:-"build"}
mode=${3:-"debug"}
clang=${4:-"true"}
lld=${5:-"true"}
target_os=${6:-""}
target_cpu=${7:-""}
run_args="${@:8}"

script_dir="$(cd $(dirname $0) && pwd)"
source "$script_dir/env.sh"

build_dir="$out_dir/$build_subdir"

$script_dir/build.sh $target $build_subdir $mode $clang $lld $target_os $target_cpu

if [[ $target != "default" ]]; then
  cd "$build_dir"
  if [[ -f "$build_dir/$target" ]]; then
    echo "Running '$target'"
    "$build_dir/$target" $run_args
  elif [[ -f "$build_dir/$target.exe" ]]; then
    echo "Running '$target.exe'"
    "$build_dir/$target.exe" $run_args
  elif [[ -f "$build_dir/$target.js" ]]; then
    echo "Running '$target.js'"
    node "$build_dir/$target.js" $run_args
  else
    echo "error: '$target' is not binary (looked for $build_dir/$target{,.exe,.js})" >&2
    exit 1
  fi
fi

