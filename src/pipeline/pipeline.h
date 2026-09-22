// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "diag/render.h"
#include "fpag/base/numeric.h"
#include "pkg/resolve.h"
#include "source/source.h"

namespace pipeline {

// Source discovery convention: `<dir>/**/*.al`, walked recursively and
// sorted lexically for deterministic builds. Subdirectories containing
// alcy.toml are nested packages and skipped (their sources belong to that
// package). `alcy new` creates `<name>/src/main.al`, which this rule picks
// up naturally.
constexpr std::string_view kSourceExtension = ".al";

struct DiscoveredSources {
  // Directory as given (not canonicalized).
  std::string root;
  std::vector<source::FileId> files;
};

// diag::SourceFetch adapter over a SourceManager: ctx must point to a live
// SourceManager. Lives here (not on SourceManager) so the source module
// stays free of diag includes.
diag::SourceText fetch_source(source::FileId id, const void* ctx);

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
