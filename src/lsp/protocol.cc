// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/protocol.h"

#include <span>
#include <string_view>

#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

text::JsonValue position_json(Position position) {
  text::JsonValue out = text::JsonValue::make_object();
  out.set("line", text::JsonValue::make_number(position.line));
  out.set("character", text::JsonValue::make_number(position.character));
  return out;
}

text::JsonValue range_json(const Document& document, u32 offset, u32 length) {
  text::JsonValue out = text::JsonValue::make_object();
  out.set("start",
          position_json(document.index().position_at(document.text(), offset)));
  out.set("end", position_json(document.index().position_at(
                     document.text(), static_cast<usize>(offset) + length)));
  return out;
}

text::JsonValue location_json(const Document& document, diag::Span span) {
  text::JsonValue out = text::JsonValue::make_object();
  out.set("uri", text::JsonValue::make_string(document.uri()));
  out.set("range", range_json(document, span.offset, span.length));
  return out;
}

u64 span_key(diag::Span span) {
  return (static_cast<u64>(span.offset) << 32) | static_cast<u64>(span.length);
}

bool same_span(diag::Span left, diag::Span right) {
  return left.file == right.file && left.offset == right.offset &&
         left.length == right.length;
}

namespace {

constexpr std::string_view TOKEN_TYPE_NAMES[] = {
    "namespace", "type",     "struct",   "enum",       "typeParameter",
    "parameter", "variable", "property", "enumMember", "function",
    "method",    "keyword",  "modifier", "comment",    "string",
    "number",    "operator"};

static_assert(
    sizeof(TOKEN_TYPE_NAMES) / sizeof(TOKEN_TYPE_NAMES[0]) ==
        static_cast<usize>(SemanticTokenType::Count),
    "the semantic token legend and SemanticTokenType are out of step");

}  // namespace

std::span<const std::string_view> semantic_token_type_names() {
  return TOKEN_TYPE_NAMES;
}

}  // namespace lsp
