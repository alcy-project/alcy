// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <vector>

#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// One SelectionRange per requested position: the word the position is in,
// its line, the syntax nodes around it, and the whole buffer, nested
// smallest to largest. The nodes come from the compiler's parse, so a
// selection grows along the structure the compiler read.
text::JsonValue selection_ranges(const Document& document,
                                 const DocumentAnalysis& analysis,
                                 std::span<const Position> positions);

}  // namespace lsp
