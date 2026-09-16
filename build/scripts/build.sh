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

script_dir="$(cd $(dirname $0) && pwd)"
source "$script_dir/env.sh"

build_dir="$out_dir/$build_subdir"

if [[ $mode == "debug" ]]; then
  is_debug="true"
else
  is_debug="false"
fi

gn_args="is_debug=$is_debug is_clang=$clang use_lld=$lld"
if [[ -n $target_os ]]; then
  gn_args="$gn_args target_os=\"$target_os\""
fi
if [[ -n $target_cpu ]]; then
  gn_args="$gn_args target_cpu=\"$target_cpu\""
fi

gn gen $build_dir --args="$gn_args"
gn check $build_dir "//src/*"
ninja -C $build_dir -t compdb > compile_commands.json
ninja -C $build_dir $target
