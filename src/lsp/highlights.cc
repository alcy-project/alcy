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
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

void add(text::JsonValue* list,
         const Document& document,
         diag::Span span,
         bool is_write) {
  text::JsonValue highlight = text::JsonValue::make_object();
  highlight.set("range", range_json(document, span.offset, span.length));
  highlight.set("kind", text::JsonValue::make_number(static_cast<f64>(
                            is_write ? DocumentHighlightKind::Write
                                     : DocumentHighlightKind::Read)));
  list->push(std::move(highlight));
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
  const source::FileId root = analysis.root();
  if (declaration->file == root) {
    add(&highlights, document, *declaration, false);
  }
  for (const ReferenceSite site : references_at(document, analysis, position)) {
    if (site.span.file == root && !same_span(site.span, *declaration)) {
      add(&highlights, document, site.span, site.is_write);
    }
  }
  return highlights;
}

text::JsonValue linked_editing_ranges(const Document& document,
                                      const DocumentAnalysis& analysis,
                                      Position position) {
  const std::optional<diag::Span> declaration =
      definition_at(document, analysis, position);
  if (!declaration.has_value()) {
    return text::JsonValue{};
  }
  text::JsonValue ranges = text::JsonValue::make_array();
  const source::FileId root = analysis.root();
  if (declaration->file == root) {
    ranges.push(range_json(document, declaration->offset, declaration->length));
  }
  for (const ReferenceSite site : references_at(document, analysis, position)) {
    if (site.span.file == root && !same_span(site.span, *declaration)) {
      ranges.push(range_json(document, site.span.offset, site.span.length));
    }
  }
  text::JsonValue result = text::JsonValue::make_object();
  result.set("ranges", std::move(ranges));
  result.set("wordPattern",
             text::JsonValue::make_string("[A-Za-z_][A-Za-z0-9_]*"));
  return result;
}

}  // namespace lsp
