#!/usr/bin/env python3

# Copyright 2026 The Alcy Project Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from pathlib import Path

project_root_dir: Path = Path(__file__).resolve().parent.parent.parent.parent

build_dir: Path = project_root_dir / "build"
scripts_dir: Path = build_dir / "scripts"
default_out_dir: Path = project_root_dir / "out" / "build"

config_toml_file: Path = project_root_dir / "config.toml"

# include_dir: Path = project_root_dir / "include"
src_dir: Path = project_root_dir / "src"
# tests_dir: Path = project_root_dir / "tests"
# benchmarks_dir: Path = project_root_dir / "benchmarks"


project_source_dirs: list[Path] = [
    src_dir,
    # tests_dir,
    # benchmarks_dir,
]

third_party_dir: Path = project_root_dir / "third_party"
