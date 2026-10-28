// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/highlights.h"

#include <optional>
#include <utility>

#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/navigation.h"
#include "lsp/protocol.h"
#include "text/json.h"

namespace lsp {

namespace {

void add(text::JsonValue* list, const Document& document, diag::Span span) {
  text::JsonValue highlight = text::JsonValue::make_object();
  highlight.set("range", range_json(document, span.offset, span.length));
  highlight.set("kind", text::JsonValue::make_number(
                            static_cast<f64>(DocumentHighlightKind::Text)));
  list->push(std::move(highlight));
}

bool same_span(diag::Span left, diag::Span right) {
  return left.file == right.file && left.offset == right.offset &&
         left.length == right.length;
}

}  // namespace

text::JsonValue document_highlights(const Document& document,
                                    const DocumentAnalysis& analysis,
                                    Position position) {
  text::JsonValue highlights = text::JsonValue::make_array();
  const std::optional<diag::Span> declaration =
      definition_at(document, analysis, position);
  if (!declaration.has_value()) {
    return highlights;
  }
  add(&highlights, document, *declaration);
  for (const diag::Span site : references_at(document, analysis, position)) {
    if (!same_span(site, *declaration)) {
      add(&highlights, document, site);
    }
  }
  return highlights;
}

}  // namespace lsp
