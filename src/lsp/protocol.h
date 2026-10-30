// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>

#include "diag/span.h"
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

// A location in this document for a span the checker recorded.
text::JsonValue location_json(const Document& document, diag::Span span);

// A span's identity as one integer, for a key in a table. Features
// compare spans within one root file, so the file is not part of the
// key.
u64 span_key(diag::Span span);

// Whether two spans cover the same range of the same file.
bool same_span(diag::Span left, diag::Span right);

// The protocol enumerations this server emits, carrying the values the
// specification assigns them. A feature names a kind rather than
// counting one, so the number that crosses the wire is the protocol's
// and not a local tally. Only the members the server uses are listed.

enum class SymbolKind : u8 {
  File = 1,
  Module = 2,
  Namespace = 3,
  Package = 4,
  Class = 5,
  Method = 6,
  Property = 7,
  Field = 8,
  Constructor = 9,
  Enum = 10,
  Interface = 11,
  Function = 12,
  Variable = 13,
  Constant = 14,
  String = 15,
  Number = 16,
  Boolean = 17,
  Array = 18,
  Object = 19,
  Key = 20,
  Null = 21,
  EnumMember = 22,
  Struct = 23,
  Event = 24,
  Operator = 25,
  TypeParameter = 26,
};

enum class CompletionItemKind : u8 {
  Text = 1,
  Method = 2,
  Function = 3,
  Constructor = 4,
  Field = 5,
  Variable = 6,
  Class = 7,
  Interface = 8,
  Module = 9,
  Property = 10,
  Unit = 11,
  Value = 12,
  Enum = 13,
  Keyword = 14,
  Snippet = 15,
  Color = 16,
  File = 17,
  Reference = 18,
  Folder = 19,
  EnumMember = 20,
  Constant = 21,
  Struct = 22,
  Event = 23,
  Operator = 24,
  TypeParameter = 25,
};

enum class DiagnosticSeverity : u8 {
  Error = 1,
  Warning = 2,
  Information = 3,
  Hint = 4,
};

enum class DocumentHighlightKind : u8 { Text = 1, Read = 2, Write = 3 };

enum class InlayHintKind : u8 { Type = 1, Parameter = 2 };

// The token types the server classifies, in legend order: a member's
// value is its wire index, and semantic_token_type_names() lists the
// same order, so the two cannot disagree. Count bounds the list.
enum class SemanticTokenType : u8 {
  Namespace,
  Type,
  Struct,
  Enum,
  TypeParameter,
  Parameter,
  Variable,
  Property,
  EnumMember,
  Function,
  Method,
  Keyword,
  Modifier,
  Comment,
  String,
  Number,
  Operator,
  Count,
};

// The wire names of the semantic token types, in enum order. The length
// is SemanticTokenType::Count.
std::span<const std::string_view> semantic_token_type_names();

}  // namespace lsp
