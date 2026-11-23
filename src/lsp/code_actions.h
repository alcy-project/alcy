// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "diag/diagnostic.h"
#include "lsp/analysis_cache.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The LSP CodeAction kinds this server can return.
constexpr std::string_view CODE_ACTION_QUICKFIX = "quickfix";
constexpr std::string_view CODE_ACTION_REFACTOR = "refactor";
constexpr std::string_view CODE_ACTION_SOURCE = "source";

// The LSP CodeAction kind for a repair: quickfix or refactor.
std::string_view code_action_kind_name(diag::FixKind kind);

// The code actions for the findings that intersect the range: one
// CodeAction per machine-applicable repair, carrying its LSP kind and
// the edit the linter decided. The findings come from the same package reading
// the diagnostics were published from. The result is an array of CodeAction.
text::JsonValue code_actions(const Document& document,
                             AnalysisCache& cache,
                             Position start,
                             Position end);
text::JsonValue code_actions(const Document& document,
                             Position start,
                             Position end);

}  // namespace lsp
