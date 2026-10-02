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
