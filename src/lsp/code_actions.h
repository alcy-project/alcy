// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/analysis_cache.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The quick fixes for the findings that intersect the range: one
// CodeAction per machine-applicable repair, carrying the edit the linter
// decided. The findings come from the same package reading the
// diagnostics were published from. The result is an array of CodeAction.
text::JsonValue code_actions(const Document& document,
                             AnalysisCache& cache,
                             Position start,
                             Position end);
text::JsonValue code_actions(const Document& document,
                             Position start,
                             Position end);

}  // namespace lsp
