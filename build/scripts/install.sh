#!/usr/bin/env bash

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -e

script_dir=$(dirname "$0")
cd "$script_dir/../.." && root_dir=$(pwd)
tool_scripts_dir="$root_dir/build/scripts"

if [ -z "$IN_NIX_SHELL" ]; then
  exec nix develop -c "$0" "$@"
fi

release_subdir="build_release"

uv run "$tool_scripts_dir/build.py" \
  --target=default \
  --mode=release \
  --build-subdir=$release_subdir

executable_name="alcy"
build_artifact="$root_dir/out/$release_subdir/$executable_name"
install_destination="$HOME/.local/bin/$executable_name"
cp "$build_artifact" "$install_destination"

echo "Installed successfully to $install_destination"
