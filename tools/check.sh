#!/usr/bin/env bash

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# The gates live in check.py; this stays as the entry point the
# documentation names and holds no logic of its own. --help there lists
# every flag and what it skips.

if command -v uv >/dev/null 2>&1; then
  exec uv run python "$(dirname "$0")/check.py" "$@"
elif command -v python3 >/dev/null 2>&1; then
  exec python3 "$(dirname "$0")/check.py" "$@"
else
  echo "error: uv or python3 is required to run this script" >&2
  exit 1
fi
