// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "diag/span.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// Whether a name is one identifier the compiler's lexer would accept: a
// single identifier token covering the whole string.
bool is_rename_identifier(std::string_view name);

// The identifier under the cursor and its spelling, when that identifier
// resolves to a declaration the checker recorded.
struct RenameTarget {
  diag::Span span;
  std::string name;
};

std::optional<RenameTarget> rename_target(const Document& document,
                                          const DocumentAnalysis& analysis,
                                          Position position);

// A WorkspaceEdit that renames the name at position to new_name, or
// nullopt when the name cannot be renamed. A site that spells the name
// more than once, as A::A would, is declined rather than guessed at.
std::optional<text::JsonValue> workspace_rename(
    const Document& document,
    const DocumentAnalysis& analysis,
    Position position,
    std::string_view new_name);

}  // namespace lsp
