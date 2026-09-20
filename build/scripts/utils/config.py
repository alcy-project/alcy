#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# This source code is licensed under the Apache License, Version 2.0 with LLVM
# Exceptions which can be found in the LICENSE file.

import tomllib
from typing import Any, Dict
from utils.paths import config_toml_file


def load_config() -> Dict[str, Any]:
    """Reads and parses config.toml from project root."""
    if not config_toml_file.is_file():
        raise FileNotFoundError(f"config.toml not found at {config_toml_file}")

    with open(config_toml_file, "rb") as f:
        return tomllib.load(f)


def get_llvm_toolchain_version() -> str:
    config = load_config()
    return config.get("llvm_toolchain_version", "")


def get_llvm_fork_tag() -> str:
    config = load_config()
    return config.get("llvm_fork_tag", "")
