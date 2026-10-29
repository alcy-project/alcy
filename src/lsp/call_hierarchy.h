// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The function or method the position names, as a CallHierarchyItem, or
// an empty array when the position names no callable. A call site is a
// reference the checker recorded at the callee of a call, so the graph
// is the checker's own reading.
text::JsonValue prepare_call_hierarchy(const Document& document,
                                       const DocumentAnalysis& analysis,
                                       Position position);

// The functions that call the item, one entry per caller with the call
// sites inside it.
text::JsonValue incoming_calls(const Document& document,
                               const DocumentAnalysis& analysis,
                               const text::JsonValue& item);

// The callables the item calls, one entry per callee with the call
// sites that reach it.
text::JsonValue outgoing_calls(const Document& document,
                               const DocumentAnalysis& analysis,
                               const text::JsonValue& item);

}  // namespace lsp
