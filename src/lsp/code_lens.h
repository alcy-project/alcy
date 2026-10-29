// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// One code lens per callable or nominal the document declares, carrying
// the number of references the checker recorded. The lens command asks
// the client to show those references.
text::JsonValue code_lenses(const Document& document,
                            const DocumentAnalysis& analysis);

}  // namespace lsp
