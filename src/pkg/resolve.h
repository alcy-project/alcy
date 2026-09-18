// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "fpag/mem/arena.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pkg {

struct ResolvedPackage {
  PackageManifest manifest;
  // Canonical directory path (owns its bytes).
  std::string dir;
  // Manifest file id in the SourceManager passed to resolve_package().
  source::FileId manifest_file = source::kUnknownFile;
};

// Resolves the package at `dir` (which must contain alcy.toml) plus its
// transitive path-only dependencies, depth-first over key-sorted dependency
// tables (deterministic, but not manifest order). Reports cycles, unreadable
// manifests, and unparsable manifests as diagnostics. Shared dependencies
// resolve once per incoming edge (no dedup in MVP). Manifest views borrow
// from `arena`, which must outlive the result.
diag::Fallible<std::vector<ResolvedPackage>> resolve_package(
    std::string_view dir,
    source::SourceManager& sources,
    mem::Arena& arena,
    diag::DiagBag& bag);

}  // namespace pkg
