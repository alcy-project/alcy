// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The JSON shapes the protocol repeats across methods. They live in one
// place so a position or a range cannot be spelled two ways.

text::JsonValue position_json(Position position);

// A range covering [offset, offset + length) in the document. The two
// ends are converted through the document's own position index.
text::JsonValue range_json(const Document& document, u32 offset, u32 length);

}  // namespace lsp
