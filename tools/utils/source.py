#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

source_extensions = {".c", ".cc", ".h"}
compile_unit_extensions = {".c", ".cc"}
header_extensions = source_extensions - compile_unit_extensions
gn_extensions = {".gn", ".gni"}
script_extensions = {".sh", ".py"}
