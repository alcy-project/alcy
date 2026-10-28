// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The occurrences of the name at position: its declaration and every use
// the checker resolved to it, as an array of DocumentHighlight. The
// checker does not record reads and writes apart, so every occurrence is
// a plain text highlight.
text::JsonValue document_highlights(const Document& document,
                                    const DocumentAnalysis& analysis,
                                    Position position);

}  // namespace lsp
