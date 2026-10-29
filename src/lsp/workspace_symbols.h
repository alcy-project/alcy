// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// Symbols from every open document whose name matches the query, as an
// LSP SymbolInformation array. The query is a case-insensitive
// substring; an empty query matches every symbol. Each document is read
// with the outline parser alone, so a symbol survives the errors that
// stop analysis.
text::JsonValue workspace_symbols(const DocumentStore& documents,
                                  std::string_view query);

}  // namespace lsp
