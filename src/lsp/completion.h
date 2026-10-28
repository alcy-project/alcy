// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The completion list at a position: the names the checked package
// holds, plus the language's keywords, filtered by the identifier
// prefix the cursor has typed. The result is a CompletionList object.
text::JsonValue completion_items(const Document& document,
                                 const DocumentAnalysis& analysis,
                                 Position position);

}  // namespace lsp
