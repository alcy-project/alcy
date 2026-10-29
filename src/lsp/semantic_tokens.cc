// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/semantic_tokens.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "cst/lex.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "lexer/token.h"
#include "lsp/analysis.h"
#include "lsp/ast_query.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

u64 span_key(diag::Span span) {
  return (static_cast<u64>(span.offset) << 32) | static_cast<u64>(span.length);
}

std::optional<SemanticTokenType> literal_type(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::I8:
    case lexer::TokenKind::I16:
    case lexer::TokenKind::I32:
    case lexer::TokenKind::I64:
    case lexer::TokenKind::I128:
    case lexer::TokenKind::Isize:
    case lexer::TokenKind::U8:
    case lexer::TokenKind::U16:
    case lexer::TokenKind::U32:
    case lexer::TokenKind::U64:
    case lexer::TokenKind::U128:
    case lexer::TokenKind::Usize:
    case lexer::TokenKind::F32:
    case lexer::TokenKind::F64:
    case lexer::TokenKind::Bool:
    case lexer::TokenKind::Str: return SemanticTokenType::Type;
    case lexer::TokenKind::Integer:
    case lexer::TokenKind::Float: return SemanticTokenType::Number;
    case lexer::TokenKind::String:
    case lexer::TokenKind::Char: return SemanticTokenType::String;
    case lexer::TokenKind::DocComment: return SemanticTokenType::Comment;
    default: return std::nullopt;
  }
}

bool is_keyword(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::Fn:
    case lexer::TokenKind::Intrinsic:
    case lexer::TokenKind::Struct:
    case lexer::TokenKind::Enum:
    case lexer::TokenKind::Impl:
    case lexer::TokenKind::Static:
    case lexer::TokenKind::Pub:
    case lexer::TokenKind::Const:
    case lexer::TokenKind::Mut:
    case lexer::TokenKind::Use:
    case lexer::TokenKind::If:
    case lexer::TokenKind::Else:
    case lexer::TokenKind::Loop:
    case lexer::TokenKind::While:
    case lexer::TokenKind::Break:
    case lexer::TokenKind::Continue:
    case lexer::TokenKind::Ret:
    case lexer::TokenKind::Match:
    case lexer::TokenKind::Package:
    case lexer::TokenKind::Self:
    case lexer::TokenKind::SelfType:
    case lexer::TokenKind::As:
    case lexer::TokenKind::Async:
    case lexer::TokenKind::Await:
    case lexer::TokenKind::Union:
    case lexer::TokenKind::Register:
    case lexer::TokenKind::Comp:
    case lexer::TokenKind::Spec:
    case lexer::TokenKind::Extern:
    case lexer::TokenKind::Unsafe:
    case lexer::TokenKind::For:
    case lexer::TokenKind::In:
    case lexer::TokenKind::Where:
    case lexer::TokenKind::Dyn:
    case lexer::TokenKind::True:
    case lexer::TokenKind::False: return true;
    default: return false;
  }
}

bool is_operator(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::Plus:
    case lexer::TokenKind::Minus:
    case lexer::TokenKind::Star:
    case lexer::TokenKind::Slash:
    case lexer::TokenKind::Percent:
    case lexer::TokenKind::StarStar:
    case lexer::TokenKind::Amp:
    case lexer::TokenKind::Pipe:
    case lexer::TokenKind::Caret:
    case lexer::TokenKind::Tilde:
    case lexer::TokenKind::LessLess:
    case lexer::TokenKind::GreaterGreater:
    case lexer::TokenKind::PlusEq:
    case lexer::TokenKind::MinusEq:
    case lexer::TokenKind::StarEq:
    case lexer::TokenKind::SlashEq:
    case lexer::TokenKind::PercentEq:
    case lexer::TokenKind::StarStarEq:
    case lexer::TokenKind::AmpEq:
    case lexer::TokenKind::PipeEq:
    case lexer::TokenKind::CaretEq:
    case lexer::TokenKind::LessLessEq:
    case lexer::TokenKind::GreaterGreaterEq:
    case lexer::TokenKind::ColonEq:
    case lexer::TokenKind::Eq:
    case lexer::TokenKind::EqEq:
    case lexer::TokenKind::Bang:
    case lexer::TokenKind::AmpAmp:
    case lexer::TokenKind::PipePipe:
    case lexer::TokenKind::BangEq:
    case lexer::TokenKind::Greater:
    case lexer::TokenKind::Less:
    case lexer::TokenKind::GreaterEq:
    case lexer::TokenKind::LessEq:
    case lexer::TokenKind::Arrow:
    case lexer::TokenKind::FatArrow:
    case lexer::TokenKind::DotDot:
    case lexer::TokenKind::DotDotEq:
    case lexer::TokenKind::DotDotLess:
    case lexer::TokenKind::Question: return true;
    default: return false;
  }
}

// The kinds the checker attached to names, addressable by the span a
// name occupies, the end of a path, or the name itself.
struct SymbolKinds {
  std::unordered_map<u64, SemanticTokenType> by_span;
  std::unordered_map<u32, SemanticTokenType> by_path_end;
  std::unordered_map<std::string_view, SemanticTokenType> by_name;
};

void add_span(SymbolKinds& kinds, diag::Span span, SemanticTokenType type) {
  if (span.length != 0) {
    kinds.by_span.emplace(span_key(span), type);
  }
}

void collect(const DocumentAnalysis& analysis, SymbolKinds& kinds) {
  const ast::AstArena& ast = analysis.context().ast;
  const analyzer::CheckedPackage& package = analysis.package();
  const source::FileId root = analysis.root();
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::FnSig& fn : module.functions) {
      if (!fn.item.is_valid()) {
        continue;
      }
      const SemanticTokenType type = fn.is_method ? SemanticTokenType::Method
                                                  : SemanticTokenType::Function;
      const diag::Span name = item_name_span(ast, ast.items[fn.item]);
      add_span(kinds, name, type);
      if (name.file == root) {
        kinds.by_name.emplace(fn.name, type);
      }
    }
    for (const analyzer::CheckedModule::MethodInfo& method : module.methods) {
      if (!method.item.is_valid()) {
        continue;
      }
      const diag::Span name = item_name_span(ast, ast.items[method.item]);
      add_span(kinds, name, SemanticTokenType::Method);
      kinds.by_name.emplace(method.name, SemanticTokenType::Method);
    }
    for (const analyzer::CheckedModule::StructInfo& info : module.structs) {
      if (!info.item.is_valid()) {
        continue;
      }
      const ast::ItemNode& item = ast.items[info.item];
      const diag::Span name = item_name_span(ast, item);
      add_span(kinds, name, SemanticTokenType::Struct);
      kinds.by_name.emplace(item.payload.get<ast::ItemStruct>().name.name,
                            SemanticTokenType::Struct);
      for (const ast::ItemStructField& field :
           item.payload.get<ast::ItemStruct>().fields) {
        add_span(kinds, field.name.span, SemanticTokenType::Property);
      }
    }
    for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
      if (!info.item.is_valid()) {
        continue;
      }
      const ast::ItemNode& item = ast.items[info.item];
      add_span(kinds, item_name_span(ast, item), SemanticTokenType::Enum);
      kinds.by_name.emplace(info.name, SemanticTokenType::Enum);
      const ast::ItemEnum& declaration = item.payload.get<ast::ItemEnum>();
      for (const ast::ItemEnumVariant& variant : declaration.variants) {
        add_span(kinds, variant.name.span, SemanticTokenType::EnumMember);
        kinds.by_name.emplace(variant.name.name, SemanticTokenType::EnumMember);
      }
    }
    for (const analyzer::CheckedModule::StaticInfo& info : module.statics) {
      if (!info.item.is_valid()) {
        continue;
      }
      add_span(kinds, item_name_span(ast, ast.items[info.item]),
               SemanticTokenType::Variable);
      kinds.by_name.emplace(info.name, SemanticTokenType::Variable);
    }
  }
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::Reference& reference :
         module.references) {
      if (reference.site.file != root) {
        continue;
      }
      const auto declared = kinds.by_span.find(span_key(reference.declaration));
      const SemanticTokenType type = declared == kinds.by_span.end()
                                         ? SemanticTokenType::Variable
                                         : declared->second;
      add_span(kinds, reference.declaration, SemanticTokenType::Variable);
      add_span(kinds, reference.site, type);
      kinds.by_path_end.emplace(reference.site.offset + reference.site.length,
                                type);
    }
  }
}

std::optional<SemanticTokenType> classify_identifier(const SymbolKinds& kinds,
                                                     std::string_view source,
                                                     diag::Span span) {
  const u64 key = span_key(span);
  const auto exact = kinds.by_span.find(key);
  if (exact != kinds.by_span.end()) {
    return exact->second;
  }
  const auto path = kinds.by_path_end.find(span.offset + span.length);
  if (path != kinds.by_path_end.end()) {
    return path->second;
  }
  const auto named =
      kinds.by_name.find(source.substr(span.offset, span.length));
  if (named != kinds.by_name.end()) {
    return named->second;
  }
  return std::nullopt;
}

}  // namespace

text::JsonValue semantic_token_types() {
  text::JsonValue types = text::JsonValue::make_array();
  for (const std::string_view type : semantic_token_type_names()) {
    types.push(text::JsonValue::make_string(std::string(type)));
  }
  return types;
}

text::JsonValue semantic_tokens_empty() {
  text::JsonValue result = text::JsonValue::make_object();
  result.set("data", text::JsonValue::make_array());
  return result;
}

namespace {

std::vector<u32> semantic_tokens_in(const Document& document,
                                    const DocumentAnalysis& analysis,
                                    usize range_begin,
                                    usize range_end) {
  const std::string_view source = document.text();
  SymbolKinds kinds;
  if (analysis.ok()) {
    collect(analysis, kinds);
  }

  std::vector<u32> data;
  u32 previous_line = 0;
  u32 previous_character = 0;
  bool first = true;
  const auto emit = [&](Position start, u32 length, SemanticTokenType type) {
    if (length == 0) {
      return;
    }
    const u32 delta_line = first || start.line != previous_line
                               ? start.line - (first ? 0 : previous_line)
                               : 0;
    const u32 delta_character = first || start.line != previous_line
                                    ? start.character
                                    : start.character - previous_character;
    data.push_back(delta_line);
    data.push_back(delta_character);
    data.push_back(length);
    data.push_back(static_cast<u32>(type));
    data.push_back(0);
    previous_line = start.line;
    previous_character = start.character;
    first = false;
  };
  // A token cannot cross a line on the wire, so a block comment or a
  // multi-line string is emitted one line at a time.
  const auto emit_span = [&](diag::Span span, SemanticTokenType type) {
    const usize end =
        std::min(static_cast<usize>(span.offset) + span.length, range_end);
    usize begin = span.offset;
    while (begin < end) {
      usize line_end = begin;
      while (line_end < end && source[line_end] != '\n') {
        ++line_end;
      }
      usize content_end = line_end;
      if (content_end > begin && source[content_end - 1] == '\r') {
        --content_end;
      }
      const Position start = document.index().position_at(source, begin);
      const Position stop = document.index().position_at(source, content_end);
      emit(start, stop.character - start.character, type);
      begin = line_end + 1;
    }
  };

  for (const cst::Lexeme& lexeme : cst::lex(source)) {
    if (lexeme.kind == cst::Lexeme::Kind::End) {
      break;
    }
    if (lexeme.offset < range_begin || lexeme.offset >= range_end) {
      continue;
    }
    diag::Span span;
    span.offset = lexeme.offset;
    span.length = lexeme.length;
    if (lexeme.kind == cst::Lexeme::Kind::Comment) {
      emit_span(span, SemanticTokenType::Comment);
      continue;
    }
    if (lexeme.synthetic_semi) {
      continue;
    }
    if (lexeme.token == lexer::TokenKind::Ident) {
      const std::optional<SemanticTokenType> type =
          classify_identifier(kinds, source, span);
      if (type.has_value()) {
        emit_span(span, *type);
      }
      continue;
    }
    if (is_keyword(lexeme.token)) {
      emit_span(span, SemanticTokenType::Keyword);
      continue;
    }
    if (const std::optional<SemanticTokenType> literal =
            literal_type(lexeme.token);
        literal.has_value()) {
      emit_span(span, *literal);
      continue;
    }
    if (is_operator(lexeme.token)) {
      emit_span(span, SemanticTokenType::Operator);
    }
  }

  return data;
}

text::JsonValue tokens_json(std::span<const u32> data,
                            std::string_view result_id) {
  text::JsonValue array = text::JsonValue::make_array();
  for (const u32 value : data) {
    array.push(text::JsonValue::make_number(static_cast<f64>(value)));
  }
  text::JsonValue result = text::JsonValue::make_object();
  result.set("data", std::move(array));
  if (!result_id.empty()) {
    result.set("resultId",
               text::JsonValue::make_string(std::string(result_id)));
  }
  return result;
}

text::JsonValue compute_edits(std::span<const u32> previous,
                              std::span<const u32> current) {
  usize prefix = 0;
  while (prefix < previous.size() && prefix < current.size() &&
         previous[prefix] == current[prefix]) {
    ++prefix;
  }
  prefix -= prefix % 5;
  usize suffix = 0;
  while (suffix < previous.size() - prefix &&
         suffix < current.size() - prefix &&
         previous[previous.size() - 1 - suffix] ==
             current[current.size() - 1 - suffix]) {
    ++suffix;
  }
  suffix -= suffix % 5;

  text::JsonValue edits = text::JsonValue::make_array();
  const usize removed = previous.size() - prefix - suffix;
  const usize added = current.size() - prefix - suffix;
  if (removed != 0 || added != 0) {
    text::JsonValue edit = text::JsonValue::make_object();
    edit.set("start", text::JsonValue::make_number(static_cast<f64>(prefix)));
    edit.set("deleteCount",
             text::JsonValue::make_number(static_cast<f64>(removed)));
    if (added != 0) {
      text::JsonValue inserted = text::JsonValue::make_array();
      for (usize i = prefix; i < current.size() - suffix; ++i) {
        inserted.push(
            text::JsonValue::make_number(static_cast<f64>(current[i])));
      }
      edit.set("data", std::move(inserted));
    }
    edits.push(std::move(edit));
  }
  return edits;
}

}  // namespace

text::JsonValue semantic_tokens(const Document& document,
                                const DocumentAnalysis& analysis) {
  return tokens_json(
      semantic_tokens_in(document, analysis, 0, document.text().size()), {});
}

text::JsonValue semantic_tokens_range(const Document& document,
                                      const DocumentAnalysis& analysis,
                                      Position start,
                                      Position end) {
  const usize begin = document.index().byte_offset(document.text(), start);
  const usize finish = document.index().byte_offset(document.text(), end);
  return tokens_json(semantic_tokens_in(document, analysis, begin, finish), {});
}

text::JsonValue SemanticTokensCache::full(const Document& document,
                                          const DocumentAnalysis& analysis) {
  std::vector<u32> data =
      semantic_tokens_in(document, analysis, 0, document.text().size());
  const std::string id = std::to_string(next_id_++);
  entries_.insert_or_assign(document.uri(), Entry{id, std::move(data)});
  const Entry& entry = entries_.at(document.uri());
  return tokens_json(entry.data, entry.result_id);
}

text::JsonValue SemanticTokensCache::delta(
    const Document& document,
    const DocumentAnalysis& analysis,
    std::string_view previous_result_id) {
  std::vector<u32> current =
      semantic_tokens_in(document, analysis, 0, document.text().size());
  const std::string id = std::to_string(next_id_++);
  const auto entry = entries_.find(document.uri());
  if (entry == entries_.end() ||
      entry->second.result_id != previous_result_id) {
    entries_.insert_or_assign(document.uri(), Entry{id, std::move(current)});
    const Entry& stored = entries_.at(document.uri());
    return tokens_json(stored.data, stored.result_id);
  }
  text::JsonValue edits = compute_edits(entry->second.data, current);
  entries_.insert_or_assign(document.uri(), Entry{id, std::move(current)});
  text::JsonValue result = text::JsonValue::make_object();
  result.set("resultId", text::JsonValue::make_string(id));
  result.set("edits", std::move(edits));
  return result;
}

void SemanticTokensCache::erase(std::string_view uri) {
  entries_.erase(std::string(uri));
}

}  // namespace lsp
