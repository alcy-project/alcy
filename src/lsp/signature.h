// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The signature help for the innermost call the position sits in, or a
// null JSON value when the position is not inside a resolved call.
text::JsonValue signature_help(const Document& document,
                               const DocumentAnalysis& analysis,
                               Position position);

}  // namespace lsp
