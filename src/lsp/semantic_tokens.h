// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The token types the server classifies, in legend order: a token's type
// on the wire is its index in this array.
text::JsonValue semantic_token_types();

// An empty SemanticTokens result, for a document the server does not
// hold or one the frontend could not read.
text::JsonValue semantic_tokens_empty();

// The semantic tokens for a document, as an LSP SemanticTokens object.
// Keywords, literals, comments, and operators come from the lossless
// lexer; an identifier's kind comes from the checker's records, so a
// name is colored as what the compiler resolved it to.
text::JsonValue semantic_tokens(const Document& document,
                                const DocumentAnalysis& analysis);

}  // namespace lsp
