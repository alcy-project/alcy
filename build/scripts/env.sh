#!/usr/bin/env bash

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

set -e

script_dir="$(cd $(dirname $0) && pwd)"
root_dir="$(cd $script_dir/../.. && pwd)"
out_dir="$root_dir/out"
