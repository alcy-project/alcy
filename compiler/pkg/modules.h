// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>
#include <vector>

#include "diag/bag.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "pkg/manifest.h"
#include "source/source.h"

namespace pkg {

// One selected module: its slash-separated name (`utils/io`) and
// source file. Names borrow arena storage owned by the caller.
struct ModuleFile {
  std::string_view name;
  source::FileId id = source::UNKNOWN_FILE;
};

// Selects module files for a manifest: explicit `include` entries map to
// `<entry>.al` under root, while a wildcard adds every discovered file by
// its root-relative path. An include that matches no file is an error; the
// cli warns separately about files left outside the selection.
base::Result<std::vector<ModuleFile>, diag::Reported> resolve_module_files(
    const PackageManifest& manifest,
    std::string_view root,
    const std::vector<source::FileId>& files,
    source::SourceManager& sources,
    diag::DiagBag& bag,
    mem::Arena& arena);

}  // namespace pkg
