#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

import sys
import subprocess

# Reroute execution through 'uv run python'
cmd = ["uv", "run", "python"] + sys.argv[1:]
sys.exit(subprocess.call(cmd))
