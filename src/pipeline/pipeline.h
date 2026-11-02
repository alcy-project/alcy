// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "source/source.h"

namespace pipeline {

// Source discovery convention: `<dir>/**/*.al`, walked recursively and
// sorted lexically for deterministic builds. Subdirectories containing
// alcy.toml are nested packages and are skipped.
struct DiscoveredSourcePaths {
  // Directory as given (not canonicalized).
  std::string root;
  std::vector<std::string> files;
};

// Discovers the name of every source file under dir without reading one.
// A caller that only lists the files - the formatter rewrites what it
// finds - takes this one: a loaded source stays mapped, and Windows will
// not truncate a file a view of it still exists for.
base::Result<DiscoveredSourcePaths, diag::Reported> discover_source_paths(
    std::string_view dir,
    diag::DiagBag& bag);

struct DiscoveredSources {
  // Directory as given (not canonicalized).
  std::string root;
  std::vector<source::FileId> files;
};

// Discovers and loads every source file under dir. It does not lex or parse.
base::Result<DiscoveredSources, diag::Reported> discover_sources(
    std::string_view dir,
    source::SourceManager& sources,
    diag::DiagBag& bag);

}  // namespace pipeline
