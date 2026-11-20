// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "lsp/analysis_cache.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// Symbols from every source file of the packages the open documents
// belong to, matching the query, as an LSP SymbolInformation array. Each
// file is read with the outline parser alone, so a symbol survives the
// errors that stop analysis.
text::JsonValue workspace_symbols(const DocumentStore& documents,
                                  AnalysisCache& cache,
                                  std::string_view query);
text::JsonValue workspace_symbols(const DocumentStore& documents,
                                  std::string_view query);

}  // namespace lsp
