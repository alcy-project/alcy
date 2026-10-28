// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/rename.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cst/lex.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "lexer/token.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/navigation.h"
#include "lsp/protocol.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

std::optional<diag::Span> identifier_token_at(
    const std::vector<cst::Lexeme>& lexemes,
    source::FileId root,
    usize offset) {
  for (const cst::Lexeme& lexeme : lexemes) {
    if (lexeme.kind == cst::Lexeme::Kind::End) {
      break;
    }
    if (lexeme.kind != cst::Lexeme::Kind::Token ||
        lexeme.token != lexer::TokenKind::Ident) {
      continue;
    }
    if (lexeme.offset <= offset && offset < lexeme.offset + lexeme.length) {
      diag::Span span;
      span.file = root;
      span.offset = lexeme.offset;
      span.length = lexeme.length;
      return span;
    }
  }
  return std::nullopt;
}

std::optional<diag::Span> named_segment_in(
    const std::vector<cst::Lexeme>& lexemes,
    diag::Span site,
    std::string_view name,
    std::string_view source) {
  std::optional<diag::Span> found;
  for (const cst::Lexeme& lexeme : lexemes) {
    if (lexeme.kind == cst::Lexeme::Kind::End) {
      break;
    }
    if (lexeme.offset < site.offset) {
      continue;
    }
    if (lexeme.offset >= site.offset + site.length) {
      break;
    }
    if (lexeme.offset + lexeme.length > site.offset + site.length) {
      break;
    }
    if (lexeme.kind != cst::Lexeme::Kind::Token ||
        lexeme.token != lexer::TokenKind::Ident) {
      continue;
    }
    if (source.substr(lexeme.offset, lexeme.length) != name) {
      continue;
    }
    if (found.has_value()) {
      return std::nullopt;
    }
    diag::Span span;
    span.file = site.file;
    span.offset = lexeme.offset;
    span.length = lexeme.length;
    found = span;
  }
  return found;
}

}  // namespace

bool is_rename_identifier(std::string_view name) {
  const std::vector<cst::Lexeme> lexemes = cst::lex(name);
  if (lexemes.size() != 2) {
    return false;
  }
  const cst::Lexeme& token = lexemes.front();
  return token.kind == cst::Lexeme::Kind::Token &&
         token.token == lexer::TokenKind::Ident && token.offset == 0 &&
         token.length == name.size();
}

std::optional<RenameTarget> rename_target(const Document& document,
                                          const DocumentAnalysis& analysis,
                                          Position position) {
  const std::string_view source = document.text();
  const usize offset = document.index().byte_offset(source, position);
  const std::vector<cst::Lexeme> lexemes = cst::lex(source);
  const std::optional<diag::Span> token =
      identifier_token_at(lexemes, analysis.root(), offset);
  if (!token.has_value() ||
      !definition_at(document, analysis, position).has_value()) {
    return std::nullopt;
  }
  return RenameTarget{*token,
                      std::string(source.substr(token->offset, token->length))};
}

std::optional<text::JsonValue> workspace_rename(
    const Document& document,
    const DocumentAnalysis& analysis,
    Position position,
    std::string_view new_name) {
  const std::optional<RenameTarget> target =
      rename_target(document, analysis, position);
  if (!target.has_value()) {
    return std::nullopt;
  }
  const std::optional<diag::Span> declaration =
      definition_at(document, analysis, position);
  if (!declaration.has_value()) {
    return std::nullopt;
  }
  const std::string_view source = document.text();
  const std::string name(
      source.substr(declaration->offset, declaration->length));
  const std::vector<cst::Lexeme> lexemes = cst::lex(source);

  std::vector<diag::Span> spans;
  spans.push_back(*declaration);
  for (const diag::Span site : references_at(document, analysis, position)) {
    if (site.file != analysis.root()) {
      continue;
    }
    const std::optional<diag::Span> segment =
        named_segment_in(lexemes, site, name, source);
    if (!segment.has_value()) {
      return std::nullopt;
    }
    spans.push_back(*segment);
  }

  std::sort(spans.begin(), spans.end(), [](diag::Span left, diag::Span right) {
    if (left.offset != right.offset) {
      return left.offset < right.offset;
    }
    return left.length < right.length;
  });
  spans.erase(std::unique(spans.begin(), spans.end(),
                          [](diag::Span left, diag::Span right) {
                            return left.offset == right.offset &&
                                   left.length == right.length;
                          }),
              spans.end());

  text::JsonValue edits = text::JsonValue::make_array();
  for (const diag::Span span : spans) {
    text::JsonValue edit = text::JsonValue::make_object();
    edit.set("range", range_json(document, span.offset, span.length));
    edit.set("newText", text::JsonValue::make_string(std::string(new_name)));
    edits.push(std::move(edit));
  }
  text::JsonValue changes = text::JsonValue::make_object();
  changes.set(document.uri(), std::move(edits));
  text::JsonValue result = text::JsonValue::make_object();
  result.set("changes", std::move(changes));
  return result;
}

}  // namespace lsp
