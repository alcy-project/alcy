// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The `TextEdit[]` that replaces the document with the formatter's
// canonical layout. Empty when the buffer already has that layout.
text::JsonValue document_formatting(const Document& document);

}  // namespace lsp
