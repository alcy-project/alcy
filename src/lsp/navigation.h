// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <optional>
#include <vector>

#include "diag/span.h"
#include "lsp/analysis.h"
#include "lsp/document.h"

namespace lsp {

// The declaration the name at position resolves to. A use answers with
// the declaration the checker recorded; a declaration answers with
// itself. Nothing at the position answers nothing.
std::optional<diag::Span> definition_at(const Document& document,
                                        const DocumentAnalysis& analysis,
                                        Position position);

// Every recorded use of the declaration the position names, the
// declaration site excluded. The declaration is the one a use resolved
// to, or the declaration the position sits on.
std::vector<diag::Span> references_at(const Document& document,
                                      const DocumentAnalysis& analysis,
                                      Position position);

// The declaration of the type of the value or written type at position:
// a variable of struct type answers with the struct declaration. A type
// the language defines, such as a primitive, answers nothing.
std::optional<diag::Span> type_definition_at(const Document& document,
                                             const DocumentAnalysis& analysis,
                                             Position position);

}  // namespace lsp
