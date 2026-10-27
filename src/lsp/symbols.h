// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The hierarchical outline of one document: the compiler AST's items,
// with struct fields, enum variants, and impl or spec members as
// children. Parsing alone answers it, so an outline survives the errors
// that stop analysis.
text::JsonValue document_symbols(const Document& document);

}  // namespace lsp
