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
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "lsp/protocol.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"
#include "text/json.h"

namespace lsp {

namespace {

// Writes absolute token positions as the LSP delta stream. Both the
// first computation and a cached-array filter use this one rule.
class DeltaEncoder {
 public:
  explicit DeltaEncoder(std::vector<u32>& out) : out_(out) {}

  void add(Position start, u32 length, u32 type, u8 modifier) {
    if (length == 0) {
      return;
    }
    const bool new_line = first_ || start.line != previous_line_;
    out_.push_back(new_line ? start.line - (first_ ? 0 : previous_line_) : 0);
    out_.push_back(new_line ? start.character
                            : start.character - previous_character_);
    out_.push_back(length);
    out_.push_back(type);
    out_.push_back(modifier);
    previous_line_ = start.line;
    previous_character_ = start.character;
    first_ = false;
  }

 private:
  std::vector<u32>& out_;
  u32 previous_line_ = 0;
  u32 previous_character_ = 0;
  bool first_ = true;
};

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
    case lexer::TokenKind::Str:
    case lexer::TokenKind::CharType:
    case lexer::TokenKind::RuneType: return SemanticTokenType::Type;
    case lexer::TokenKind::Integer:
    case lexer::TokenKind::Float: return SemanticTokenType::Number;
    case lexer::TokenKind::String:
    case lexer::TokenKind::Char: return SemanticTokenType::String;
    case lexer::TokenKind::DocComment: return SemanticTokenType::Comment;
    default: return std::nullopt;
  }
}

// The kinds the checker attached to names, addressable by the span a
// name occupies, the end of a path, or the name itself.
struct SymbolKinds {
  std::unordered_map<FullSpanKey, SemanticTokenType, FullSpanKeyHash> by_span;
  std::unordered_map<FullSpanKey, u8, FullSpanKeyHash> modifiers;
  std::unordered_map<u64, SemanticTokenType> by_path_end;
  std::unordered_map<std::string_view, SemanticTokenType> by_name;
};

void add_span(SymbolKinds& kinds, diag::Span span, SemanticTokenType type) {
  if (span.length != 0) {
    kinds.by_span.emplace(FullSpanKey{span.file, span_key(span)}, type);
  }
}

void collect(const DocumentAnalysis& analysis, SymbolKinds& kinds) {
  const ast::AstArena& ast = analysis.context().ast;
  const analyzer::CheckedPackage& package = analysis.package();
  const source::FileId root = analysis.root();
  const auto mark_declaration = [&](diag::Span span, bool is_readonly = false) {
    if (span.file != root || span.length == 0) {
      return;
    }
    u8& bits = kinds.modifiers[FullSpanKey{span.file, span_key(span)}];
    bits |= static_cast<u8>(1u << static_cast<u8>(TokenModifier::Declaration));
    if (is_readonly) {
      bits |= static_cast<u8>(1u << static_cast<u8>(TokenModifier::ReadOnly));
    }
  };
  ast.patterns.for_each_node([&](const ast::PatternIdx index) {
    const ast::PatternNode& node = ast.patterns[index];
    if (node.kind == ast::PatternKind::Ident) {
      mark_declaration(node.payload.ident.name.span, true);
    } else if (node.kind == ast::PatternKind::MutIdent) {
      mark_declaration(node.payload.mut_ident.name.span);
    }
  });
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::FnSig& fn : module.functions) {
      if (!fn.item.is_valid()) {
        continue;
      }
      const SemanticTokenType type = fn.is_method ? SemanticTokenType::Method
                                                  : SemanticTokenType::Function;
      const diag::Span name = ::ast::item_name_span(ast, ast.items[fn.item]);
      add_span(kinds, name, type);
      mark_declaration(name);
      if (name.file == root) {
        kinds.by_name.emplace(fn.name, type);
      }
    }
    for (const analyzer::CheckedModule::MethodInfo& method : module.methods) {
      if (!method.item.is_valid()) {
        continue;
      }
      const diag::Span name =
          ::ast::item_name_span(ast, ast.items[method.item]);
      add_span(kinds, name, SemanticTokenType::Method);
      mark_declaration(name);
      kinds.by_name.emplace(method.name, SemanticTokenType::Method);
    }
    for (const analyzer::CheckedModule::StructInfo& info : module.structs) {
      if (!info.item.is_valid()) {
        continue;
      }
      const ast::ItemNode& item = ast.items[info.item];
      const diag::Span name = ::ast::item_name_span(ast, item);
      add_span(kinds, name, SemanticTokenType::Struct);
      mark_declaration(name);
      kinds.by_name.emplace(item.payload.get<ast::ItemStruct>().name.name,
                            SemanticTokenType::Struct);
      for (const ast::ItemStructField& field :
           item.payload.get<ast::ItemStruct>().fields) {
        add_span(kinds, field.name.span, SemanticTokenType::Property);
        mark_declaration(field.name.span);
      }
    }
    for (const analyzer::CheckedModule::EnumInfo& info : module.enums) {
      if (!info.item.is_valid()) {
        continue;
      }
      const ast::ItemNode& item = ast.items[info.item];
      const diag::Span name = ::ast::item_name_span(ast, item);
      add_span(kinds, name, SemanticTokenType::Enum);
      mark_declaration(name);
      kinds.by_name.emplace(info.name, SemanticTokenType::Enum);
      const ast::ItemEnum& declaration = item.payload.get<ast::ItemEnum>();
      for (const ast::ItemEnumVariant& variant : declaration.variants) {
        add_span(kinds, variant.name.span, SemanticTokenType::EnumMember);
        mark_declaration(variant.name.span);
        kinds.by_name.emplace(variant.name.name, SemanticTokenType::EnumMember);
      }
    }
    for (const analyzer::CheckedModule::StaticInfo& info : module.statics) {
      if (!info.item.is_valid()) {
        continue;
      }
      const diag::Span name = ::ast::item_name_span(ast, ast.items[info.item]);
      add_span(kinds, name, SemanticTokenType::Variable);
      mark_declaration(name, true);
      kinds.by_name.emplace(info.name, SemanticTokenType::Variable);
    }
  }
  for (const analyzer::CheckedModule& module : package.modules) {
    for (const analyzer::CheckedModule::Reference& reference :
         module.references) {
      if (reference.site.file != root) {
        continue;
      }
      const auto declared = kinds.by_span.find(FullSpanKey{
          reference.declaration.file, span_key(reference.declaration)});
      const SemanticTokenType type = declared == kinds.by_span.end()
                                         ? SemanticTokenType::Variable
                                         : declared->second;
      add_span(kinds, reference.declaration, SemanticTokenType::Variable);
      add_span(kinds, reference.site, type);
      const u64 path_end =
          (static_cast<u64>(reference.site.file) << 32) |
          static_cast<u64>(reference.site.offset + reference.site.length);
      kinds.by_path_end.emplace(path_end, type);
    }
  }
}

std::optional<SemanticTokenType> classify_identifier(const SymbolKinds& kinds,
                                                     std::string_view source,
                                                     source::FileId file,
                                                     diag::Span span) {
  const auto exact = kinds.by_span.find(FullSpanKey{file, span_key(span)});
  if (exact != kinds.by_span.end()) {
    return exact->second;
  }
  const u64 path_end = (static_cast<u64>(file) << 32) |
                       static_cast<u64>(span.offset + span.length);
  const auto path = kinds.by_path_end.find(path_end);
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
  DeltaEncoder encoder(data);
  const auto emit = [&](Position start, u32 length, SemanticTokenType type,
                        u8 modifier_bits = 0) {
    encoder.add(start, length, static_cast<u32>(type), modifier_bits);
  };
  // A token cannot cross a line on the wire, so a block comment or a
  // multi-line string is emitted one line at a time.
  const auto emit_span = [&](diag::Span span, SemanticTokenType type) {
    const auto mod =
        kinds.modifiers.find(FullSpanKey{span.file, span_key(span)});
    const u8 modifier_bits = mod == kinds.modifiers.end() ? 0 : mod->second;
    const usize end =
        std::min(static_cast<usize>(span.offset) + span.length, range_end);
    usize begin = std::max(static_cast<usize>(span.offset), range_begin);
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
      emit(start, stop.character - start.character, type, modifier_bits);
      begin = line_end + 1;
    }
  };

  for (const cst::Lexeme& lexeme : cst::lex(source)) {
    if (lexeme.kind == cst::Lexeme::Kind::End) {
      break;
    }
    if (lexeme.offset >= range_end) {
      continue;
    }
    diag::Span span;
    span.file = analysis.root();
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
          classify_identifier(kinds, source, span.file, span);
      if (type.has_value()) {
        emit_span(span, *type);
      }
      continue;
    }
    if (const std::optional<SemanticTokenType> literal =
            literal_type(lexeme.token);
        literal.has_value()) {
      emit_span(span, *literal);
      continue;
    }
    if (lexer::is_keyword(lexeme.token)) {
      emit_span(span, SemanticTokenType::Keyword);
      continue;
    }
    if (lexer::is_operator(lexeme.token)) {
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

namespace {

// Keeps the tokens whose start lies in [start, end) from an already
// computed array, re-encoding absolute positions as LSP deltas.
std::vector<u32> filter_tokens(const std::vector<u32>& data,
                               Position start,
                               Position end) {
  std::vector<u32> out;
  DeltaEncoder encoder(out);
  u32 line = 0;
  u32 character = 0;
  for (usize i = 0; i + 4 < data.size(); i += 5) {
    if (data[i] == 0) {
      character += data[i + 1];
    } else {
      line += data[i];
      character = data[i + 1];
    }
    if (line < start.line || line > end.line) {
      continue;
    }
    u32 length = data[i + 2];
    u32 from = character;
    // Clip both ends, matching the fresh path's range_begin/range_end.
    if (line == start.line && from < start.character) {
      if (from + length <= start.character) {
        continue;
      }
      length -= start.character - from;
      from = start.character;
    }
    if (line == end.line && from + length > end.character) {
      length = end.character > from ? end.character - from : 0;
    }
    if (length == 0) {
      continue;
    }
    encoder.add({line, from}, length, data[i + 3],
                static_cast<u8>(data[i + 4]));
  }
  return out;
}

}  // namespace

text::JsonValue SemanticTokensCache::full(const Document& document,
                                          const DocumentAnalysis& analysis) {
  const auto found = entries_.find(document.uri());
  if (found != entries_.end() && found->second.version == document.version()) {
    return tokens_json(found->second.data, found->second.result_id);
  }
  std::vector<u32> data =
      semantic_tokens_in(document, analysis, 0, document.text().size());
  const std::string id = std::to_string(next_id_++);
  entries_.insert_or_assign(document.uri(),
                            Entry{id, document.version(), std::move(data)});
  const Entry& entry = entries_.at(document.uri());
  return tokens_json(entry.data, entry.result_id);
}

text::JsonValue SemanticTokensCache::delta(
    const Document& document,
    const DocumentAnalysis& analysis,
    std::string_view previous_result_id) {
  const auto cached = entries_.find(document.uri());
  if (cached != entries_.end() &&
      cached->second.version == document.version() &&
      cached->second.result_id == previous_result_id) {
    text::JsonValue result = text::JsonValue::make_object();
    result.set("resultId",
               text::JsonValue::make_string(cached->second.result_id));
    result.set("edits", text::JsonValue::make_array());
    return result;
  }
  std::vector<u32> current =
      semantic_tokens_in(document, analysis, 0, document.text().size());
  const std::string id = std::to_string(next_id_++);
  const auto entry = entries_.find(document.uri());
  if (entry == entries_.end() ||
      entry->second.result_id != previous_result_id) {
    entries_.insert_or_assign(
        document.uri(), Entry{id, document.version(), std::move(current)});
    const Entry& stored = entries_.at(document.uri());
    return tokens_json(stored.data, stored.result_id);
  }
  text::JsonValue edits = compute_edits(entry->second.data, current);
  entries_.insert_or_assign(document.uri(),
                            Entry{id, document.version(), std::move(current)});
  text::JsonValue result = text::JsonValue::make_object();
  result.set("resultId", text::JsonValue::make_string(id));
  result.set("edits", std::move(edits));
  return result;
}

text::JsonValue SemanticTokensCache::range(const Document& document,
                                           const DocumentAnalysis& analysis,
                                           Position start,
                                           Position end) {
  const auto found = entries_.find(document.uri());
  if (found != entries_.end() && found->second.version == document.version()) {
    return tokens_json(filter_tokens(found->second.data, start, end), {});
  }
  const usize begin = document.index().byte_offset(document.text(), start);
  const usize finish = document.index().byte_offset(document.text(), end);
  return tokens_json(semantic_tokens_in(document, analysis, begin, finish), {});
}

void SemanticTokensCache::erase(std::string_view uri) {
  entries_.erase(std::string(uri));
}

}  // namespace lsp
