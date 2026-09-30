#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from pathlib import Path

project_root_dir: Path = Path(__file__).resolve().parents[2]

build_dir: Path = project_root_dir / "build"
tools_dir: Path = project_root_dir / "tools"
out_dir: Path = project_root_dir / "out"
default_out_dir: Path = out_dir / "build"

config_toml_file: Path = project_root_dir / "config.toml"

# include_dir: Path = project_root_dir / "include"
compiler_dir: Path = project_root_dir / "compiler"
# tests_dir: Path = project_root_dir / "tests"
# benchmarks_dir: Path = project_root_dir / "benchmarks"


project_source_dirs: list[Path] = [
    compiler_dir,
    # tests_dir,
    # benchmarks_dir,
]

third_party_dir: Path = project_root_dir / "third_party"

# Directory names inside the source root that the checks skip: the fuzz
# targets need libFuzzer and exist only under `--args=is_fuzz=true`, so a
# default build describes no target for them and a walk of the source root
# finds files that belong to none.
excluded_source_dirs: list[str] = ["fuzz"]
