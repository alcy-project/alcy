// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>

#include "lsp/analysis_cache.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The WorkspaceEdit a client should apply around `workspace/willRenameFiles`.
// A module is named by its path in the package, so renaming a source file
// also renames the module that `use` paths and qualified paths spell and
// the module list the manifest declares.
//
// Only files under the renamed file's package are considered. A rename
// that needs no edit answers nothing.
std::optional<text::JsonValue> will_rename_files(const DocumentStore& documents,
                                                 AnalysisCache& cache,
                                                 const text::JsonValue& params);

}  // namespace lsp
