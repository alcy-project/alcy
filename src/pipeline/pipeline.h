// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "pkg/resolve.h"
#include "source/source.h"

namespace pipeline {

// Source discovery convention: `<dir>/**/*.al`, walked recursively and
// sorted lexically for deterministic builds. The extension lives in
// path::kSourceExtension; subdirectories containing alcy.toml are nested
// packages and skipped (their sources belong to that package). `alcy new`
// creates `<name>/main.al`, which this rule picks up naturally.
struct DiscoveredSources {
  // Directory as given (not canonicalized).
  std::string root;
  std::vector<source::FileId> files;
};

struct ProjectBuild {
  u32 packages = 0;
  u32 files_loaded = 0;
};

// Discovers and loads every source file under dir. Per-file lex/parse
// attaches to the files list once those phases exist.
diag::Fallible<DiscoveredSources> discover_sources(
    std::string_view dir,
    source::SourceManager& sources,
    diag::DiagBag& bag);

// Full project flow over resolved packages: discover + load each package.
// Lexing/parsing/codegen attach per file inside the loop once implemented.
diag::Fallible<ProjectBuild> compile_project(
    std::span<const pkg::ResolvedPackage> packages,
    source::SourceManager& sources,
    diag::DiagBag& bag);

}  // namespace pipeline
