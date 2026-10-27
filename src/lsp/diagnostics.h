// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The `Diagnostic[]` to publish for one document. The compiler's checker
// runs over the buffer on each call, so the caller sees the diagnostics
// of the text it passed rather than of a cached parse.
text::JsonValue document_diagnostics(const Document& document);

}  // namespace lsp
