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
run_args="${@:6}"

script_dir="$(cd $(dirname $0) && pwd)"
source "$script_dir/env.sh"

build_dir="$out_dir/$build_subdir"

$script_dir/build.sh $target $mode $build_subdir

if [[ $target != "default" ]]; then
  cd "$build_dir"
  if [[ -f "$build_dir/$target" ]]; then
    echo "Running '$target'"
    "$build_dir/$target" $run_args
  else
    echo "$target is not binary"
  fi
fi

