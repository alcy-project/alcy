// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "fpag/mem/arena.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pkg {

// One selected module: its slash-separated name (`utils/io`) and
// source file. Names borrow arena storage owned by the caller.
struct ModuleFile {
  std::string_view name;
  source::FileId id = source::kUnknownFile;
};

// Selects module files for a manifest: explicit `include` entries
// map to `<entry>.al` under root, while a wildcard adds every
// discovered file by its root-relative path. Explicit entries must
// resolve; undiscovered names are errors, and files outside the
// selection stay out (the driver warns about them separately).
diag::Fallible<std::vector<ModuleFile>> resolve_module_files(
    const PackageManifest& manifest,
    std::string_view root,
    const std::vector<source::FileId>& files,
    source::SourceManager& sources,
    diag::DiagBag& bag,
    mem::Arena& arena);

}  // namespace pkg
