// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

// The type or spec the position names, as a TypeHierarchyItem, or an
// empty array when the position names neither.
text::JsonValue prepare_type_hierarchy(const Document& document,
                                       const DocumentAnalysis& analysis,
                                       Position position);

// The specs a struct or enum implements, or the spec a spec refines.
text::JsonValue supertypes(const Document& document,
                           const DocumentAnalysis& analysis,
                           const text::JsonValue& item);

// The types that implement a spec.
text::JsonValue subtypes(const Document& document,
                         const DocumentAnalysis& analysis,
                         const text::JsonValue& item);

}  // namespace lsp
