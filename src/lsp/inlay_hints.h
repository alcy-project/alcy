// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The inlay hints for a document: the inferred type of every binding
// that did not write one, and the memory layout of every struct or enum
// and every struct field. The result is an array of InlayHint objects.
text::JsonValue inlay_hints(const Document& document);

}  // namespace lsp
