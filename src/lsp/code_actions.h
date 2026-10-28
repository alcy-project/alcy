// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The quick fixes for the lint findings that intersect the range: one
// CodeAction per machine-applicable repair the linter attached, carrying
// the edit the linter decided. The result is an array of CodeAction.
text::JsonValue code_actions(const Document& document,
                             Position start,
                             Position end);

}  // namespace lsp
