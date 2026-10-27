// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The foldable regions of one document: every brace pair whose two
// delimiters are on different lines, and every run of comments on
// adjacent lines. Reads the lossless CST, which is the only tree that
// keeps the comments and the whitespace a fold is about.
text::JsonValue document_folding_ranges(const Document& document);

}  // namespace lsp
