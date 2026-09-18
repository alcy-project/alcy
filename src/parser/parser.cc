// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "parser/parser.h"

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "lexer/token.h"
#include "source/source.h"

namespace parser {

namespace {

// Diagnostic codes 4100-4199 are reserved for the parser.
constexpr u32 kParserUnexpectedToken = 4100;
constexpr u32 kParserReservedWord = 4101;

bool is_reserved(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::Async:
    case lexer::TokenKind::Await:
    case lexer::TokenKind::Union:
    case lexer::TokenKind::Register:
    case lexer::TokenKind::Comp:
    case lexer::TokenKind::Extern:
    case lexer::TokenKind::Unsafe:
    case lexer::TokenKind::For:
    case lexer::TokenKind::In:
    case lexer::TokenKind::Where:
    case lexer::TokenKind::Dyn: return true;
    default: return false;
  }
}

bool is_compound_assign(lexer::TokenKind kind) {
  switch (kind) {
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
    case lexer::TokenKind::GreaterGreaterEq: return true;
    default: return false;
  }
}

ast::BinaryOp compound_op(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::PlusEq: return ast::BinaryOp::Add;
    case lexer::TokenKind::MinusEq: return ast::BinaryOp::Sub;
    case lexer::TokenKind::StarEq: return ast::BinaryOp::Mul;
    case lexer::TokenKind::SlashEq: return ast::BinaryOp::Div;
    case lexer::TokenKind::PercentEq: return ast::BinaryOp::Mod;
    case lexer::TokenKind::StarStarEq: return ast::BinaryOp::Pow;
    case lexer::TokenKind::AmpEq: return ast::BinaryOp::BitAnd;
    case lexer::TokenKind::PipeEq: return ast::BinaryOp::BitOr;
    case lexer::TokenKind::CaretEq: return ast::BinaryOp::BitXor;
    case lexer::TokenKind::LessLessEq: return ast::BinaryOp::Shl;
    default: return ast::BinaryOp::Shr;
  }
}

bool is_path_segment(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::Ident:
    case lexer::TokenKind::Self:
    case lexer::TokenKind::Super:
    case lexer::TokenKind::Package:
    case lexer::TokenKind::SelfType: return true;
    default: return false;
  }
}

}  // namespace

Parser::Parser(std::span<const lexer::Token> tokens,
               std::string_view bytes,
               source::FileId file,
               mem::Arena& arena,
               diag::DiagBag& bag)
    : tokens_(tokens),
      bytes_(bytes),
      file_(file),
      arena_(arena),
      bag_(bag),
      previous_span_({file, 0, 0}) {
  skip_insignificant();
}

bool Parser::at_end() const {
  return pos_ >= tokens_.size() || tokens_[pos_].kind == lexer::TokenKind::Eof;
}

lexer::TokenKind Parser::peek_kind() const {
  return peek().kind;
}

lexer::Token Parser::peek() const {
  return tokens_[pos_];
}

lexer::Token Parser::previous() const {
  return tokens_[pos_ - 1];
}

void Parser::skip_insignificant() {
  while (pos_ < tokens_.size()) {
    const lexer::TokenKind kind = tokens_[pos_].kind;
    if (kind == lexer::TokenKind::Error ||
        kind == lexer::TokenKind::DocComment) {
      ++pos_;
      continue;
    }
    if (is_reserved(kind)) {
      const diag::Span span = tokens_[pos_].span;
      const std::string_view spelling = bytes_.substr(span.offset, span.length);
      const u32 index =
          bag_.emit(diag::Severity::Error, kParserReservedWord, span,
                    "`{}` is reserved for future use", spelling);
      (void)index;
      ++pos_;
      continue;
    }
    break;
  }
}

void Parser::advance() {
  previous_span_ = tokens_[pos_].span;
  ++pos_;
  skip_insignificant();
}

bool Parser::check(lexer::TokenKind kind) const {
  return !at_end() && peek_kind() == kind;
}

bool Parser::match(lexer::TokenKind kind) {
  if (!check(kind)) {
    return false;
  }
  advance();
  return true;
}

bool Parser::expect(lexer::TokenKind kind, std::string_view what) {
  if (match(kind)) {
    return true;
  }
  if (at_end()) {
    const u32 index =
        bag_.emit(diag::Severity::Error, kParserUnexpectedToken,
                  span_from(pos_), "expected {}, found end of file", what);
    (void)index;
    return false;
  }
  const diag::Span span = peek().span;
  const u32 index = bag_.emit(diag::Severity::Error, kParserUnexpectedToken,
                              span, "expected {}, found `{}`", what,
                              bytes_.substr(span.offset, span.length));
  (void)index;
  return false;
}

diag::Span Parser::span_from(usize mark) const {
  const u32 start = tokens_[mark].span.offset;
  const u32 end = previous_span_.offset + previous_span_.length;
  if (end < start) {
    return diag::Span{file_, start, 0};
  }
  return diag::Span{file_, start, end - start};
}

void Parser::synchronize() {
  const usize start = pos_;
  int depth = 0;
  while (!at_end()) {
    const lexer::TokenKind kind = peek_kind();
    if (depth == 0 &&
        (kind == lexer::TokenKind::Semicolon ||
         kind == lexer::TokenKind::Comma || kind == lexer::TokenKind::RBrace ||
         kind == lexer::TokenKind::RBracket ||
         kind == lexer::TokenKind::RParen)) {
      break;
    }
    if (kind == lexer::TokenKind::LBrace ||
        kind == lexer::TokenKind::LBracket ||
        kind == lexer::TokenKind::LParen) {
      ++depth;
    } else if ((kind == lexer::TokenKind::RBrace ||
                kind == lexer::TokenKind::RBracket ||
                kind == lexer::TokenKind::RParen) &&
               depth > 0) {
      --depth;
    }
    advance();
  }
  // Boundary tokens are left for the enclosing construct, but recovery
  // must always make progress: stealing one closer beats looping forever
  // on pathological input.
  if (pos_ == start && !at_end()) {
    advance();
  } else if (!at_end() && (peek_kind() == lexer::TokenKind::Semicolon ||
                           peek_kind() == lexer::TokenKind::Comma)) {
    advance();
  }
}

Parser::StmtLead Parser::scan_lead() const {
  int depth = 0;
  for (usize i = pos_; i < tokens_.size();) {
    const lexer::TokenKind kind = tokens_[i].kind;
    if (kind == lexer::TokenKind::Error ||
        kind == lexer::TokenKind::DocComment || is_reserved(kind)) {
      ++i;
      continue;
    }
    if (depth == 0) {
      if (kind == lexer::TokenKind::ColonEq) {
        return StmtLead::Decl;
      }
      if (kind == lexer::TokenKind::Eq || is_compound_assign(kind)) {
        return StmtLead::Reassign;
      }
      if (kind == lexer::TokenKind::Semicolon ||
          kind == lexer::TokenKind::RBrace ||
          kind == lexer::TokenKind::LBrace || kind == lexer::TokenKind::Eof) {
        return StmtLead::None;
      }
    }
    if (kind == lexer::TokenKind::LBrace ||
        kind == lexer::TokenKind::LBracket ||
        kind == lexer::TokenKind::LParen) {
      ++depth;
    } else if ((kind == lexer::TokenKind::RBrace ||
                kind == lexer::TokenKind::RBracket ||
                kind == lexer::TokenKind::RParen) &&
               depth > 0) {
      --depth;
    }
    ++i;
  }
  return StmtLead::None;
}

std::span<ast::Item* const> Parser::parse() {
  std::vector<ast::Item*> items;
  while (!at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    ast::Item* item = parse_item();
    if (item == nullptr) {
      synchronize();
      continue;
    }
    items.push_back(item);
  }
  return ast::copy_to_arena(arena_, items);
}

ast::Item* Parser::parse_item() {
  const bool is_pub = match(lexer::TokenKind::Pub);
  switch (peek_kind()) {
    case lexer::TokenKind::Fn: return parse_fn(is_pub);
    case lexer::TokenKind::Struct: return parse_struct(is_pub);
    case lexer::TokenKind::Enum: return parse_enum(is_pub);
    case lexer::TokenKind::Impl: return parse_impl(is_pub);
    case lexer::TokenKind::Mod: return parse_mod(is_pub);
    case lexer::TokenKind::Static: return parse_static(is_pub);
    case lexer::TokenKind::Const: return parse_const(is_pub);
    case lexer::TokenKind::Use: return parse_use(is_pub);
    default: break;
  }
  if (at_end()) {
    const u32 index =
        bag_.emit(diag::Severity::Error, kParserUnexpectedToken,
                  span_from(pos_), "expected item, found end of file");
    (void)index;
    return nullptr;
  }
  const diag::Span span = peek().span;
  const u32 index = bag_.emit(diag::Severity::Error, kParserUnexpectedToken,
                              span, "expected item, found `{}`",
                              bytes_.substr(span.offset, span.length));
  (void)index;
  return nullptr;
}

base::Result<ast::Ident, diag::Fatal> Parser::parse_ident(
    std::string_view what) {
  if (peek_kind() != lexer::TokenKind::Ident) {
    expect(lexer::TokenKind::Ident, what);
    return base::make_err(diag::Fatal{});
  }
  const diag::Span span = peek().span;
  ast::Ident id{bytes_.substr(span.offset, span.length), span};
  advance();
  return base::make_ok(id);
}

ast::Path* Parser::parse_path() {
  const usize mark = pos_;
  std::vector<ast::Ident> segments;
  while (true) {
    if (!is_path_segment(peek_kind())) {
      if (segments.empty()) {
        expect(lexer::TokenKind::Ident, "path");
        return nullptr;
      }
      break;
    }
    const diag::Span span = peek().span;
    segments.push_back(
        ast::Ident{bytes_.substr(span.offset, span.length), span});
    advance();
    if (!match(lexer::TokenKind::ColonColon)) {
      break;
    }
    if (at_end()) {
      expect(lexer::TokenKind::Ident, "path segment");
      return nullptr;
    }
  }
  ast::Path* path = arena_.create<ast::Path>();
  path->segments = ast::copy_to_arena(arena_, segments);
  path->span = span_from(mark);
  return path;
}

ast::FnItem* Parser::parse_fn(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Fn, "function")) {
    return nullptr;
  }
  base::Result<ast::Ident, diag::Fatal> name = parse_ident("function name");
  if (name.is_err()) {
    return nullptr;
  }
  if (!expect(lexer::TokenKind::LParen, "`(`")) {
    return nullptr;
  }
  std::vector<ast::FnParam> params;
  if (!check(lexer::TokenKind::RParen)) {
    while (true) {
      ast::Pattern* pattern = parse_pattern();
      if (pattern == nullptr) {
        return nullptr;
      }
      if (!expect(lexer::TokenKind::Colon, "`:`")) {
        return nullptr;
      }
      ast::Type* type = parse_closed_type();
      if (type == nullptr) {
        return nullptr;
      }
      params.push_back(ast::FnParam{pattern, type});
      if (!match(lexer::TokenKind::Comma)) {
        break;
      }
      if (check(lexer::TokenKind::RParen)) {
        break;
      }
    }
  }
  if (!expect(lexer::TokenKind::RParen, "`)`")) {
    return nullptr;
  }
  ast::Type* return_type = nullptr;
  if (match(lexer::TokenKind::Arrow)) {
    return_type = parse_closed_type();
    if (return_type == nullptr) {
      return nullptr;
    }
  }
  ast::Block* body = parse_block();
  if (body == nullptr) {
    return nullptr;
  }
  ast::FnItem* fn = arena_.create<ast::FnItem>();
  fn->kind = ast::ItemKind::Fn;
  fn->span = span_from(mark);
  fn->is_pub = is_pub;
  fn->name = std::move(name).unwrap();
  fn->params = ast::copy_to_arena(arena_, params);
  fn->return_type = return_type;
  fn->body = body;
  return fn;
}

ast::StructItem* Parser::parse_struct(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Struct, "struct")) {
    return nullptr;
  }
  base::Result<ast::Ident, diag::Fatal> name = parse_ident("struct name");
  if (name.is_err()) {
    return nullptr;
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return nullptr;
  }
  std::vector<ast::StructField> fields;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    base::Result<ast::Ident, diag::Fatal> field_name =
        parse_ident("field name");
    if (field_name.is_err()) {
      return nullptr;
    }
    if (!expect(lexer::TokenKind::Colon, "`:`")) {
      return nullptr;
    }
    ast::Type* type = parse_closed_type();
    if (type == nullptr) {
      return nullptr;
    }
    fields.push_back(ast::StructField{std::move(field_name).unwrap(), type});
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return nullptr;
  }
  ast::StructItem* item = arena_.create<ast::StructItem>();
  item->kind = ast::ItemKind::Struct;
  item->span = span_from(mark);
  item->is_pub = is_pub;
  item->name = std::move(name).unwrap();
  item->fields = ast::copy_to_arena(arena_, fields);
  return item;
}

ast::EnumItem* Parser::parse_enum(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Enum, "enum")) {
    return nullptr;
  }
  base::Result<ast::Ident, diag::Fatal> name = parse_ident("enum name");
  if (name.is_err()) {
    return nullptr;
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return nullptr;
  }
  std::vector<ast::EnumVariant> variants;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    base::Result<ast::Ident, diag::Fatal> variant_name =
        parse_ident("variant name");
    if (variant_name.is_err()) {
      return nullptr;
    }
    std::vector<ast::Type*> fields;
    if (match(lexer::TokenKind::LParen)) {
      if (!check(lexer::TokenKind::RParen)) {
        while (true) {
          ast::Type* field = parse_type();
          if (field == nullptr) {
            return nullptr;
          }
          fields.push_back(field);
          if (!match(lexer::TokenKind::Comma)) {
            break;
          }
          if (check(lexer::TokenKind::RParen)) {
            break;
          }
        }
      }
      if (!expect(lexer::TokenKind::RParen, "`)`")) {
        return nullptr;
      }
    }
    variants.push_back(ast::EnumVariant{std::move(variant_name).unwrap(),
                                        ast::copy_to_arena(arena_, fields)});
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return nullptr;
  }
  ast::EnumItem* item = arena_.create<ast::EnumItem>();
  item->kind = ast::ItemKind::Enum;
  item->span = span_from(mark);
  item->is_pub = is_pub;
  item->name = std::move(name).unwrap();
  item->variants = ast::copy_to_arena(arena_, variants);
  return item;
}

ast::ImplItem* Parser::parse_impl(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Impl, "impl")) {
    return nullptr;
  }
  ast::Type* type = parse_closed_type();
  if (type == nullptr) {
    return nullptr;
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return nullptr;
  }
  std::vector<ast::FnItem*> methods;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    ast::FnItem* method = parse_fn(false);
    if (method == nullptr) {
      synchronize();
      continue;
    }
    methods.push_back(method);
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return nullptr;
  }
  ast::ImplItem* item = arena_.create<ast::ImplItem>();
  item->kind = ast::ItemKind::Impl;
  item->span = span_from(mark);
  item->is_pub = is_pub;
  item->type = type;
  item->methods = ast::copy_to_arena(arena_, methods);
  return item;
}

ast::ModItem* Parser::parse_mod(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Mod, "module")) {
    return nullptr;
  }
  base::Result<ast::Ident, diag::Fatal> name = parse_ident("module name");
  if (name.is_err()) {
    return nullptr;
  }
  ast::ModItem* item = arena_.create<ast::ModItem>();
  item->kind = ast::ItemKind::Mod;
  item->is_pub = is_pub;
  item->name = std::move(name).unwrap();
  if (match(lexer::TokenKind::Semicolon)) {
    item->span = span_from(mark);
    item->items = {};
    item->is_inline = false;
    return item;
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return nullptr;
  }
  std::vector<ast::Item*> items;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    ast::Item* child = parse_item();
    if (child == nullptr) {
      synchronize();
      continue;
    }
    items.push_back(child);
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return nullptr;
  }
  item->span = span_from(mark);
  item->items = ast::copy_to_arena(arena_, items);
  item->is_inline = true;
  return item;
}

ast::StaticItem* Parser::parse_static(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Static, "static")) {
    return nullptr;
  }
  base::Result<ast::Ident, diag::Fatal> name = parse_ident("static name");
  if (name.is_err()) {
    return nullptr;
  }
  if (!expect(lexer::TokenKind::Colon, "`:`")) {
    return nullptr;
  }
  ast::Type* type = parse_closed_type();
  if (type == nullptr) {
    return nullptr;
  }
  if (!expect(lexer::TokenKind::Eq, "`=`")) {
    return nullptr;
  }
  ast::Expr* init = parse_expr();
  if (init == nullptr) {
    return nullptr;
  }
  ast::StaticItem* item = arena_.create<ast::StaticItem>();
  item->kind = ast::ItemKind::Static;
  item->span = span_from(mark);
  item->is_pub = is_pub;
  item->name = std::move(name).unwrap();
  item->type = type;
  item->init = init;
  return item;
}

ast::ConstItem* Parser::parse_const(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Const, "const")) {
    return nullptr;
  }
  base::Result<ast::Ident, diag::Fatal> name = parse_ident("const name");
  if (name.is_err()) {
    return nullptr;
  }
  if (!expect(lexer::TokenKind::Colon, "`:`")) {
    return nullptr;
  }
  ast::Type* type = parse_closed_type();
  if (type == nullptr) {
    return nullptr;
  }
  if (!expect(lexer::TokenKind::Eq, "`=`")) {
    return nullptr;
  }
  ast::Expr* init = parse_expr();
  if (init == nullptr) {
    return nullptr;
  }
  ast::ConstItem* item = arena_.create<ast::ConstItem>();
  item->kind = ast::ItemKind::Const;
  item->span = span_from(mark);
  item->is_pub = is_pub;
  item->name = std::move(name).unwrap();
  item->type = type;
  item->init = init;
  return item;
}

ast::UseItem* Parser::parse_use(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Use, "use")) {
    return nullptr;
  }
  ast::Path* path = parse_path();
  if (path == nullptr) {
    return nullptr;
  }
  ast::UseItem* item = arena_.create<ast::UseItem>();
  item->kind = ast::ItemKind::Use;
  item->is_pub = is_pub;
  item->path = path;
  item->has_alias = false;
  item->alias = ast::Ident{};
  if (match(lexer::TokenKind::As)) {
    base::Result<ast::Ident, diag::Fatal> alias = parse_ident("alias");
    if (alias.is_err()) {
      return nullptr;
    }
    item->has_alias = true;
    item->alias = std::move(alias).unwrap();
  }
  if (!expect(lexer::TokenKind::Semicolon, "`;`")) {
    return nullptr;
  }
  item->span = span_from(mark);
  return item;
}

bool Parser::consume_gt() {
  if (banked_gt_ > 0) {
    --banked_gt_;
    return true;
  }
  if (check(lexer::TokenKind::Greater)) {
    advance();
    return true;
  }
  if (check(lexer::TokenKind::GreaterGreater)) {
    advance();
    banked_gt_ = 1;
    return true;
  }
  return false;
}

ast::Type* Parser::parse_type() {
  const usize mark = pos_;
  switch (peek_kind()) {
    case lexer::TokenKind::LParen: {
      advance();
      if (match(lexer::TokenKind::RParen)) {
        ast::Type* unit = arena_.create<ast::Type>();
        unit->kind = ast::TypeKind::Unit;
        unit->span = span_from(mark);
        return unit;
      }
      ast::Type* first = parse_type();
      if (first == nullptr) {
        return nullptr;
      }
      if (!match(lexer::TokenKind::Comma)) {
        if (!expect(lexer::TokenKind::RParen, "`)`")) {
          return nullptr;
        }
        return first;
      }
      std::vector<ast::Type*> elements;
      elements.push_back(first);
      while (!check(lexer::TokenKind::RParen) && !at_end()) {
        ast::Type* element = parse_type();
        if (element == nullptr) {
          return nullptr;
        }
        elements.push_back(element);
        if (!match(lexer::TokenKind::Comma)) {
          break;
        }
      }
      if (!expect(lexer::TokenKind::RParen, "`)`")) {
        return nullptr;
      }
      ast::TupleType* tuple = arena_.create<ast::TupleType>();
      tuple->kind = ast::TypeKind::Tuple;
      tuple->span = span_from(mark);
      tuple->elements = ast::copy_to_arena(arena_, elements);
      return tuple;
    }
    case lexer::TokenKind::Bang: {
      advance();
      ast::Type* never = arena_.create<ast::Type>();
      never->kind = ast::TypeKind::Never;
      never->span = span_from(mark);
      return never;
    }
    case lexer::TokenKind::Amp: {
      advance();
      const bool is_mut = match(lexer::TokenKind::Mut);
      ast::Type* inner = parse_type();
      if (inner == nullptr) {
        return nullptr;
      }
      ast::RefType* ref = arena_.create<ast::RefType>();
      ref->kind = ast::TypeKind::Ref;
      ref->span = span_from(mark);
      ref->is_mut = is_mut;
      ref->inner = inner;
      return ref;
    }
    case lexer::TokenKind::Str: {
      advance();
      ast::Type* str = arena_.create<ast::Type>();
      str->kind = ast::TypeKind::Str;
      str->span = span_from(mark);
      return str;
    }
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
    case lexer::TokenKind::Bool: {
      const lexer::TokenKind kind = peek_kind();
      advance();
      ast::PrimitiveType* primitive = arena_.create<ast::PrimitiveType>();
      primitive->kind = ast::TypeKind::Primitive;
      primitive->span = span_from(mark);
      switch (kind) {
        case lexer::TokenKind::I8:
          primitive->primitive = ast::PrimitiveKind::I8;
          break;
        case lexer::TokenKind::I16:
          primitive->primitive = ast::PrimitiveKind::I16;
          break;
        case lexer::TokenKind::I32:
          primitive->primitive = ast::PrimitiveKind::I32;
          break;
        case lexer::TokenKind::I64:
          primitive->primitive = ast::PrimitiveKind::I64;
          break;
        case lexer::TokenKind::I128:
          primitive->primitive = ast::PrimitiveKind::I128;
          break;
        case lexer::TokenKind::Isize:
          primitive->primitive = ast::PrimitiveKind::Isize;
          break;
        case lexer::TokenKind::U8:
          primitive->primitive = ast::PrimitiveKind::U8;
          break;
        case lexer::TokenKind::U16:
          primitive->primitive = ast::PrimitiveKind::U16;
          break;
        case lexer::TokenKind::U32:
          primitive->primitive = ast::PrimitiveKind::U32;
          break;
        case lexer::TokenKind::U64:
          primitive->primitive = ast::PrimitiveKind::U64;
          break;
        case lexer::TokenKind::U128:
          primitive->primitive = ast::PrimitiveKind::U128;
          break;
        case lexer::TokenKind::Usize:
          primitive->primitive = ast::PrimitiveKind::Usize;
          break;
        case lexer::TokenKind::F32:
          primitive->primitive = ast::PrimitiveKind::F32;
          break;
        case lexer::TokenKind::F64:
          primitive->primitive = ast::PrimitiveKind::F64;
          break;
        default: primitive->primitive = ast::PrimitiveKind::Bool; break;
      }
      return primitive;
    }
    default: break;
  }
  ast::Path* path = parse_path();
  if (path == nullptr) {
    return nullptr;
  }
  ast::PathType* typed = arena_.create<ast::PathType>();
  typed->kind = ast::TypeKind::Path;
  typed->path = path;
  if (match(lexer::TokenKind::Less)) {
    std::vector<ast::Type*> args;
    if (!check(lexer::TokenKind::Greater) &&
        !check(lexer::TokenKind::GreaterGreater) && !at_end()) {
      while (true) {
        ast::Type* arg = parse_type();
        if (arg == nullptr) {
          return nullptr;
        }
        args.push_back(arg);
        if (!match(lexer::TokenKind::Comma)) {
          break;
        }
        if (check(lexer::TokenKind::Greater) ||
            check(lexer::TokenKind::GreaterGreater) || at_end()) {
          break;
        }
      }
    }
    if (!consume_gt()) {
      expect(lexer::TokenKind::Greater, "`>`");
      return nullptr;
    }
    typed->args = ast::copy_to_arena(arena_, args);
  } else {
    typed->args = {};
  }
  typed->span = span_from(mark);
  return typed;
}

ast::Type* Parser::parse_closed_type() {
  ast::Type* type = parse_type();
  if (type == nullptr) {
    return nullptr;
  }
  if (banked_gt_ > 0) {
    banked_gt_ = 0;
    expect(lexer::TokenKind::Greater, "`>`");
    return nullptr;
  }
  return type;
}

ast::Pattern* Parser::parse_pattern() {
  return parse_or_pattern();
}

ast::Pattern* Parser::parse_or_pattern() {
  const usize mark = pos_;
  ast::Pattern* first = parse_primary_pattern();
  if (first == nullptr) {
    return nullptr;
  }
  if (!match(lexer::TokenKind::Pipe)) {
    return first;
  }
  std::vector<ast::Pattern*> alternatives;
  alternatives.push_back(first);
  while (true) {
    ast::Pattern* alternative = parse_primary_pattern();
    if (alternative == nullptr) {
      return nullptr;
    }
    alternatives.push_back(alternative);
    if (!match(lexer::TokenKind::Pipe)) {
      break;
    }
  }
  ast::OrPattern* or_pattern = arena_.create<ast::OrPattern>();
  or_pattern->kind = ast::PatternKind::Or;
  or_pattern->span = span_from(mark);
  or_pattern->alternatives = ast::copy_to_arena(arena_, alternatives);
  return or_pattern;
}

ast::Pattern* Parser::parse_primary_pattern() {
  const usize mark = pos_;
  switch (peek_kind()) {
    case lexer::TokenKind::Underscore: {
      advance();
      ast::WildcardPattern* pattern = arena_.create<ast::WildcardPattern>();
      pattern->kind = ast::PatternKind::Wildcard;
      pattern->span = span_from(mark);
      return pattern;
    }
    case lexer::TokenKind::Mut: {
      advance();
      base::Result<ast::Ident, diag::Fatal> name = parse_ident("pattern name");
      if (name.is_err()) {
        return nullptr;
      }
      ast::MutIdentPattern* pattern = arena_.create<ast::MutIdentPattern>();
      pattern->kind = ast::PatternKind::MutIdent;
      pattern->span = span_from(mark);
      pattern->name = std::move(name).unwrap();
      return pattern;
    }
    case lexer::TokenKind::Integer:
    case lexer::TokenKind::Float:
    case lexer::TokenKind::String:
    case lexer::TokenKind::Char:
    case lexer::TokenKind::True:
    case lexer::TokenKind::False: {
      ast::Literal* lit = arena_.create<ast::Literal>();
      switch (peek_kind()) {
        case lexer::TokenKind::Integer:
          lit->kind = ast::LiteralKind::Integer;
          break;
        case lexer::TokenKind::Float:
          lit->kind = ast::LiteralKind::Float;
          break;
        case lexer::TokenKind::String:
          lit->kind = ast::LiteralKind::String;
          break;
        case lexer::TokenKind::Char: lit->kind = ast::LiteralKind::Char; break;
        default: lit->kind = ast::LiteralKind::Bool; break;
      }
      const diag::Span span = peek().span;
      lit->span = span;
      lit->spelling = bytes_.substr(span.offset, span.length);
      advance();
      ast::LiteralPattern* pattern = arena_.create<ast::LiteralPattern>();
      pattern->kind = ast::PatternKind::Literal;
      pattern->span = span_from(mark);
      pattern->value = lit;
      return pattern;
    }
    case lexer::TokenKind::Minus: {
      // Negative number patterns ("-1"): the span covers the sign so
      // later stages read the value with its sign.
      advance();
      if (peek_kind() != lexer::TokenKind::Integer &&
          peek_kind() != lexer::TokenKind::Float) {
        expect(lexer::TokenKind::Integer, "number literal");
        return nullptr;
      }
      const bool is_float = peek_kind() == lexer::TokenKind::Float;
      const diag::Span span = peek().span;
      ast::Literal* lit = arena_.create<ast::Literal>();
      lit->kind =
          is_float ? ast::LiteralKind::Float : ast::LiteralKind::Integer;
      lit->span = span;
      lit->spelling = bytes_.substr(span.offset, span.length);
      advance();
      ast::LiteralPattern* pattern = arena_.create<ast::LiteralPattern>();
      pattern->kind = ast::PatternKind::Literal;
      pattern->span = span_from(mark);
      pattern->value = lit;
      return pattern;
    }
    case lexer::TokenKind::Amp: {
      advance();
      const bool is_mut = match(lexer::TokenKind::Mut);
      ast::Pattern* inner = parse_primary_pattern();
      if (inner == nullptr) {
        return nullptr;
      }
      ast::RefPattern* pattern = arena_.create<ast::RefPattern>();
      pattern->kind = ast::PatternKind::Ref;
      pattern->span = span_from(mark);
      pattern->is_mut = is_mut;
      pattern->inner = inner;
      return pattern;
    }
    case lexer::TokenKind::LParen: {
      advance();
      ast::Pattern* first = parse_pattern();
      if (first == nullptr) {
        return nullptr;
      }
      if (!match(lexer::TokenKind::Comma)) {
        if (!expect(lexer::TokenKind::RParen, "`)`")) {
          return nullptr;
        }
        return first;
      }
      std::vector<ast::Pattern*> elements;
      elements.push_back(first);
      while (!check(lexer::TokenKind::RParen) && !at_end()) {
        ast::Pattern* element = parse_pattern();
        if (element == nullptr) {
          return nullptr;
        }
        elements.push_back(element);
        if (!match(lexer::TokenKind::Comma)) {
          break;
        }
      }
      if (!expect(lexer::TokenKind::RParen, "`)`")) {
        return nullptr;
      }
      ast::TuplePattern* pattern = arena_.create<ast::TuplePattern>();
      pattern->kind = ast::PatternKind::Tuple;
      pattern->span = span_from(mark);
      pattern->path = nullptr;
      pattern->elements = ast::copy_to_arena(arena_, elements);
      return pattern;
    }
    default: break;
  }
  ast::Path* path = parse_path();
  if (path == nullptr) {
    return nullptr;
  }
  // A lone lowercase identifier binds a variable; anything else names a
  // unit variant (uppercase `None`, qualified `Option::None`). Name
  // resolution refines this, but the parser must commit to a shape.
  if (path->segments.size() == 1) {
    const std::string_view name = path->segments[0].name;
    if (!name.empty() &&
        (name.front() == '_' || (name.front() >= 'a' && name.front() <= 'z'))) {
      ast::IdentPattern* binding = arena_.create<ast::IdentPattern>();
      binding->kind = ast::PatternKind::Ident;
      binding->span = span_from(mark);
      binding->name = path->segments[0];
      return binding;
    }
  }
  if (match(lexer::TokenKind::LParen)) {
    std::vector<ast::Pattern*> elements;
    if (!check(lexer::TokenKind::RParen)) {
      while (true) {
        ast::Pattern* element = parse_pattern();
        if (element == nullptr) {
          return nullptr;
        }
        elements.push_back(element);
        if (!match(lexer::TokenKind::Comma)) {
          break;
        }
        if (check(lexer::TokenKind::RParen)) {
          break;
        }
      }
    }
    if (!expect(lexer::TokenKind::RParen, "`)`")) {
      return nullptr;
    }
    ast::TuplePattern* pattern = arena_.create<ast::TuplePattern>();
    pattern->kind = ast::PatternKind::Tuple;
    pattern->span = span_from(mark);
    pattern->path = path;
    pattern->elements = ast::copy_to_arena(arena_, elements);
    return pattern;
  }
  if (match(lexer::TokenKind::LBrace)) {
    std::vector<ast::FieldPattern> fields;
    while (!check(lexer::TokenKind::RBrace) && !at_end()) {
      base::Result<ast::Ident, diag::Fatal> name = parse_ident("field name");
      if (name.is_err()) {
        return nullptr;
      }
      const ast::Pattern* sub = nullptr;
      if (match(lexer::TokenKind::Colon)) {
        sub = parse_pattern();
        if (sub == nullptr) {
          return nullptr;
        }
      }
      ast::Ident bound = std::move(name).unwrap();
      if (sub == nullptr) {
        // Shorthand `Foo { x }` binds the field to a variable of the
        // same name.
        ast::IdentPattern* shorthand = arena_.create<ast::IdentPattern>();
        shorthand->kind = ast::PatternKind::Ident;
        shorthand->span = bound.span;
        shorthand->name = bound;
        sub = shorthand;
      }
      fields.push_back(ast::FieldPattern{bound, sub});
      if (!match(lexer::TokenKind::Comma)) {
        break;
      }
    }
    if (!expect(lexer::TokenKind::RBrace, "`}`")) {
      return nullptr;
    }
    ast::StructPattern* pattern = arena_.create<ast::StructPattern>();
    pattern->kind = ast::PatternKind::Struct;
    pattern->span = span_from(mark);
    pattern->path = path;
    pattern->fields = ast::copy_to_arena(arena_, fields);
    return pattern;
  }
  // A bare path is a unit-variant pattern.
  ast::TuplePattern* pattern = arena_.create<ast::TuplePattern>();
  pattern->kind = ast::PatternKind::Tuple;
  pattern->span = span_from(mark);
  pattern->path = path;
  pattern->elements = {};
  return pattern;
}

ast::Expr* Parser::parse_expr() {
  return parse_range();
}

ast::Expr* Parser::parse_range() {
  const usize mark = pos_;
  if (check(lexer::TokenKind::DotDot) || check(lexer::TokenKind::DotDotEq) ||
      check(lexer::TokenKind::DotDotLess)) {
    const lexer::TokenKind kind = peek_kind();
    advance();
    ast::RangeExpr* range = arena_.create<ast::RangeExpr>();
    range->kind = ast::ExprKind::Range;
    range->start = nullptr;
    range->inclusive = (kind == lexer::TokenKind::DotDotEq);
    if (kind == lexer::TokenKind::DotDot || at_end() ||
        check(lexer::TokenKind::RBrace) || check(lexer::TokenKind::RBracket) ||
        check(lexer::TokenKind::RParen) || check(lexer::TokenKind::Comma) ||
        check(lexer::TokenKind::Semicolon)) {
      range->end = nullptr;
    } else {
      range->end = parse_or();
      if (range->end == nullptr) {
        return nullptr;
      }
    }
    range->span = span_from(mark);
    return range;
  }
  ast::Expr* lhs = parse_or();
  if (lhs == nullptr) {
    return nullptr;
  }
  if (!check(lexer::TokenKind::DotDot) && !check(lexer::TokenKind::DotDotEq) &&
      !check(lexer::TokenKind::DotDotLess)) {
    return lhs;
  }
  const lexer::TokenKind kind = peek_kind();
  advance();
  ast::RangeExpr* range = arena_.create<ast::RangeExpr>();
  range->kind = ast::ExprKind::Range;
  range->start = lhs;
  range->inclusive = (kind == lexer::TokenKind::DotDotEq);
  if (kind == lexer::TokenKind::DotDot || at_end() ||
      check(lexer::TokenKind::RBrace) || check(lexer::TokenKind::RBracket) ||
      check(lexer::TokenKind::RParen) || check(lexer::TokenKind::Comma) ||
      check(lexer::TokenKind::Semicolon)) {
    range->end = nullptr;
  } else {
    range->end = parse_or();
    if (range->end == nullptr) {
      return nullptr;
    }
  }
  range->span = span_from(mark);
  return range;
}

ast::Expr* Parser::parse_or() {
  const usize mark = pos_;
  ast::Expr* lhs = parse_and();
  if (lhs == nullptr) {
    return nullptr;
  }
  while (match(lexer::TokenKind::PipePipe)) {
    ast::Expr* rhs = parse_and();
    if (rhs == nullptr) {
      return nullptr;
    }
    ast::BinaryExpr* expr = arena_.create<ast::BinaryExpr>();
    expr->kind = ast::ExprKind::Binary;
    expr->span = span_from(mark);
    expr->op = ast::BinaryOp::Or;
    expr->lhs = lhs;
    expr->rhs = rhs;
    lhs = expr;
  }
  return lhs;
}

ast::Expr* Parser::parse_and() {
  const usize mark = pos_;
  ast::Expr* lhs = parse_cmp();
  if (lhs == nullptr) {
    return nullptr;
  }
  while (match(lexer::TokenKind::AmpAmp)) {
    ast::Expr* rhs = parse_cmp();
    if (rhs == nullptr) {
      return nullptr;
    }
    ast::BinaryExpr* expr = arena_.create<ast::BinaryExpr>();
    expr->kind = ast::ExprKind::Binary;
    expr->span = span_from(mark);
    expr->op = ast::BinaryOp::And;
    expr->lhs = lhs;
    expr->rhs = rhs;
    lhs = expr;
  }
  return lhs;
}

ast::Expr* Parser::parse_cmp() {
  const usize mark = pos_;
  ast::Expr* lhs = parse_bitor();
  if (lhs == nullptr) {
    return nullptr;
  }
  ast::BinaryOp op = ast::BinaryOp::Add;
  switch (peek_kind()) {
    case lexer::TokenKind::EqEq: op = ast::BinaryOp::Eq; break;
    case lexer::TokenKind::BangEq: op = ast::BinaryOp::NotEq; break;
    case lexer::TokenKind::Greater: op = ast::BinaryOp::Gt; break;
    case lexer::TokenKind::Less: op = ast::BinaryOp::Lt; break;
    case lexer::TokenKind::GreaterEq: op = ast::BinaryOp::GtEq; break;
    case lexer::TokenKind::LessEq: op = ast::BinaryOp::LtEq; break;
    default: return lhs;
  }
  advance();
  ast::Expr* rhs = parse_bitor();
  if (rhs == nullptr) {
    return nullptr;
  }
  ast::BinaryExpr* expr = arena_.create<ast::BinaryExpr>();
  expr->kind = ast::ExprKind::Binary;
  expr->span = span_from(mark);
  expr->op = op;
  expr->lhs = lhs;
  expr->rhs = rhs;
  return expr;
}

ast::Expr* Parser::parse_bitor() {
  const usize mark = pos_;
  ast::Expr* lhs = parse_bitxor();
  if (lhs == nullptr) {
    return nullptr;
  }
  while (match(lexer::TokenKind::Pipe)) {
    ast::Expr* rhs = parse_bitxor();
    if (rhs == nullptr) {
      return nullptr;
    }
    ast::BinaryExpr* expr = arena_.create<ast::BinaryExpr>();
    expr->kind = ast::ExprKind::Binary;
    expr->span = span_from(mark);
    expr->op = ast::BinaryOp::BitOr;
    expr->lhs = lhs;
    expr->rhs = rhs;
    lhs = expr;
  }
  return lhs;
}

ast::Expr* Parser::parse_bitxor() {
  const usize mark = pos_;
  ast::Expr* lhs = parse_bitand();
  if (lhs == nullptr) {
    return nullptr;
  }
  while (match(lexer::TokenKind::Caret)) {
    ast::Expr* rhs = parse_bitand();
    if (rhs == nullptr) {
      return nullptr;
    }
    ast::BinaryExpr* expr = arena_.create<ast::BinaryExpr>();
    expr->kind = ast::ExprKind::Binary;
    expr->span = span_from(mark);
    expr->op = ast::BinaryOp::BitXor;
    expr->lhs = lhs;
    expr->rhs = rhs;
    lhs = expr;
  }
  return lhs;
}

ast::Expr* Parser::parse_bitand() {
  const usize mark = pos_;
  ast::Expr* lhs = parse_shift();
  if (lhs == nullptr) {
    return nullptr;
  }
  while (match(lexer::TokenKind::Amp)) {
    ast::Expr* rhs = parse_shift();
    if (rhs == nullptr) {
      return nullptr;
    }
    ast::BinaryExpr* expr = arena_.create<ast::BinaryExpr>();
    expr->kind = ast::ExprKind::Binary;
    expr->span = span_from(mark);
    expr->op = ast::BinaryOp::BitAnd;
    expr->lhs = lhs;
    expr->rhs = rhs;
    lhs = expr;
  }
  return lhs;
}

ast::Expr* Parser::parse_shift() {
  const usize mark = pos_;
  ast::Expr* lhs = parse_add();
  if (lhs == nullptr) {
    return nullptr;
  }
  while (true) {
    ast::BinaryOp op = ast::BinaryOp::Add;
    if (match(lexer::TokenKind::LessLess)) {
      op = ast::BinaryOp::Shl;
    } else if (match(lexer::TokenKind::GreaterGreater)) {
      op = ast::BinaryOp::Shr;
    } else {
      return lhs;
    }
    ast::Expr* rhs = parse_add();
    if (rhs == nullptr) {
      return nullptr;
    }
    ast::BinaryExpr* expr = arena_.create<ast::BinaryExpr>();
    expr->kind = ast::ExprKind::Binary;
    expr->span = span_from(mark);
    expr->op = op;
    expr->lhs = lhs;
    expr->rhs = rhs;
    lhs = expr;
  }
}

ast::Expr* Parser::parse_add() {
  const usize mark = pos_;
  ast::Expr* lhs = parse_mul();
  if (lhs == nullptr) {
    return nullptr;
  }
  while (true) {
    ast::BinaryOp op = ast::BinaryOp::Add;
    if (match(lexer::TokenKind::Plus)) {
      op = ast::BinaryOp::Add;
    } else if (match(lexer::TokenKind::Minus)) {
      op = ast::BinaryOp::Sub;
    } else {
      return lhs;
    }
    ast::Expr* rhs = parse_mul();
    if (rhs == nullptr) {
      return nullptr;
    }
    ast::BinaryExpr* expr = arena_.create<ast::BinaryExpr>();
    expr->kind = ast::ExprKind::Binary;
    expr->span = span_from(mark);
    expr->op = op;
    expr->lhs = lhs;
    expr->rhs = rhs;
    lhs = expr;
  }
}

ast::Expr* Parser::parse_mul() {
  const usize mark = pos_;
  ast::Expr* lhs = parse_pow();
  if (lhs == nullptr) {
    return nullptr;
  }
  while (true) {
    ast::BinaryOp op = ast::BinaryOp::Add;
    if (match(lexer::TokenKind::Star)) {
      op = ast::BinaryOp::Mul;
    } else if (match(lexer::TokenKind::Slash)) {
      op = ast::BinaryOp::Div;
    } else if (match(lexer::TokenKind::Percent)) {
      op = ast::BinaryOp::Mod;
    } else {
      return lhs;
    }
    ast::Expr* rhs = parse_pow();
    if (rhs == nullptr) {
      return nullptr;
    }
    ast::BinaryExpr* expr = arena_.create<ast::BinaryExpr>();
    expr->kind = ast::ExprKind::Binary;
    expr->span = span_from(mark);
    expr->op = op;
    expr->lhs = lhs;
    expr->rhs = rhs;
    lhs = expr;
  }
}

ast::Expr* Parser::parse_pow() {
  const usize mark = pos_;
  ast::Expr* lhs = parse_cast();
  if (lhs == nullptr) {
    return nullptr;
  }
  if (!match(lexer::TokenKind::StarStar)) {
    return lhs;
  }
  ast::Expr* rhs = parse_pow();
  if (rhs == nullptr) {
    return nullptr;
  }
  ast::BinaryExpr* expr = arena_.create<ast::BinaryExpr>();
  expr->kind = ast::ExprKind::Binary;
  expr->span = span_from(mark);
  expr->op = ast::BinaryOp::Pow;
  expr->lhs = lhs;
  expr->rhs = rhs;
  return expr;
}

ast::Expr* Parser::parse_cast() {
  const usize mark = pos_;
  ast::Expr* inner = parse_unary();
  if (inner == nullptr) {
    return nullptr;
  }
  if (!match(lexer::TokenKind::As)) {
    return inner;
  }
  ast::Type* type = parse_closed_type();
  if (type == nullptr) {
    return nullptr;
  }
  ast::CastExpr* expr = arena_.create<ast::CastExpr>();
  expr->kind = ast::ExprKind::Cast;
  expr->span = span_from(mark);
  expr->inner = inner;
  expr->type = type;
  return expr;
}

ast::Expr* Parser::parse_unary() {
  const usize mark = pos_;
  ast::UnaryOp op = ast::UnaryOp::Neg;
  switch (peek_kind()) {
    case lexer::TokenKind::Minus: op = ast::UnaryOp::Neg; break;
    case lexer::TokenKind::Bang: op = ast::UnaryOp::Not; break;
    case lexer::TokenKind::Tilde: op = ast::UnaryOp::BitNot; break;
    default: return parse_postfix();
  }
  advance();
  ast::Expr* inner = parse_unary();
  if (inner == nullptr) {
    return nullptr;
  }
  ast::UnaryExpr* expr = arena_.create<ast::UnaryExpr>();
  expr->kind = ast::ExprKind::Unary;
  expr->span = span_from(mark);
  expr->op = op;
  expr->inner = inner;
  return expr;
}

ast::Expr* Parser::parse_postfix() {
  const usize mark = pos_;
  ast::Expr* base = parse_primary();
  if (base == nullptr) {
    return nullptr;
  }
  while (true) {
    if (match(lexer::TokenKind::LParen)) {
      std::vector<ast::Expr*> args;
      if (!check(lexer::TokenKind::RParen)) {
        while (true) {
          ast::Expr* arg = parse_expr();
          if (arg == nullptr) {
            return nullptr;
          }
          args.push_back(arg);
          if (!match(lexer::TokenKind::Comma)) {
            break;
          }
          if (check(lexer::TokenKind::RParen)) {
            break;
          }
        }
      }
      if (!expect(lexer::TokenKind::RParen, "`)`")) {
        return nullptr;
      }
      ast::CallExpr* call = arena_.create<ast::CallExpr>();
      call->kind = ast::ExprKind::Call;
      call->span = span_from(mark);
      call->callee = base;
      call->args = ast::copy_to_arena(arena_, args);
      base = call;
    } else if (match(lexer::TokenKind::Dot)) {
      if (peek_kind() == lexer::TokenKind::Integer) {
        const diag::Span span = peek().span;
        ast::Ident name{bytes_.substr(span.offset, span.length), span};
        advance();
        ast::FieldExpr* field = arena_.create<ast::FieldExpr>();
        field->kind = ast::ExprKind::Field;
        field->span = span_from(mark);
        field->receiver = base;
        field->name = name;
        base = field;
      } else {
        base::Result<ast::Ident, diag::Fatal> name = parse_ident("field name");
        if (name.is_err()) {
          return nullptr;
        }
        if (match(lexer::TokenKind::LParen)) {
          std::vector<ast::Expr*> args;
          if (!check(lexer::TokenKind::RParen)) {
            while (true) {
              ast::Expr* arg = parse_expr();
              if (arg == nullptr) {
                return nullptr;
              }
              args.push_back(arg);
              if (!match(lexer::TokenKind::Comma)) {
                break;
              }
              if (check(lexer::TokenKind::RParen)) {
                break;
              }
            }
          }
          if (!expect(lexer::TokenKind::RParen, "`)`")) {
            return nullptr;
          }
          ast::MethodCallExpr* call = arena_.create<ast::MethodCallExpr>();
          call->kind = ast::ExprKind::MethodCall;
          call->span = span_from(mark);
          call->receiver = base;
          call->name = std::move(name).unwrap();
          call->args = ast::copy_to_arena(arena_, args);
          base = call;
        } else {
          ast::FieldExpr* field = arena_.create<ast::FieldExpr>();
          field->kind = ast::ExprKind::Field;
          field->span = span_from(mark);
          field->receiver = base;
          field->name = std::move(name).unwrap();
          base = field;
        }
      }
    } else if (match(lexer::TokenKind::LBracket)) {
      ast::Expr* index = parse_expr();
      if (index == nullptr) {
        return nullptr;
      }
      if (!expect(lexer::TokenKind::RBracket, "`]`")) {
        return nullptr;
      }
      ast::IndexExpr* access = arena_.create<ast::IndexExpr>();
      access->kind = ast::ExprKind::Index;
      access->span = span_from(mark);
      access->receiver = base;
      access->index = index;
      base = access;
    } else if (match(lexer::TokenKind::Question)) {
      ast::QuestionExpr* question = arena_.create<ast::QuestionExpr>();
      question->kind = ast::ExprKind::Question;
      question->span = span_from(mark);
      question->inner = base;
      base = question;
    } else {
      return base;
    }
  }
}

ast::Expr* Parser::parse_primary() {
  const usize mark = pos_;
  switch (peek_kind()) {
    case lexer::TokenKind::Integer:
    case lexer::TokenKind::Float:
    case lexer::TokenKind::String:
    case lexer::TokenKind::Char:
    case lexer::TokenKind::True:
    case lexer::TokenKind::False: {
      ast::Literal* lit = arena_.create<ast::Literal>();
      switch (peek_kind()) {
        case lexer::TokenKind::Integer:
          lit->kind = ast::LiteralKind::Integer;
          break;
        case lexer::TokenKind::Float:
          lit->kind = ast::LiteralKind::Float;
          break;
        case lexer::TokenKind::String:
          lit->kind = ast::LiteralKind::String;
          break;
        case lexer::TokenKind::Char: lit->kind = ast::LiteralKind::Char; break;
        default: lit->kind = ast::LiteralKind::Bool; break;
      }
      const diag::Span span = peek().span;
      lit->span = span;
      lit->spelling = bytes_.substr(span.offset, span.length);
      advance();
      ast::LiteralExpr* expr = arena_.create<ast::LiteralExpr>();
      expr->kind = ast::ExprKind::Literal;
      expr->span = span_from(mark);
      expr->value = lit;
      return expr;
    }
    case lexer::TokenKind::LParen: {
      advance();
      ast::Expr* first = parse_expr();
      if (first == nullptr) {
        return nullptr;
      }
      if (!match(lexer::TokenKind::Comma)) {
        if (!expect(lexer::TokenKind::RParen, "`)`")) {
          return nullptr;
        }
        return first;
      }
      std::vector<ast::Expr*> elements;
      elements.push_back(first);
      while (!check(lexer::TokenKind::RParen) && !at_end()) {
        ast::Expr* element = parse_expr();
        if (element == nullptr) {
          return nullptr;
        }
        elements.push_back(element);
        if (!match(lexer::TokenKind::Comma)) {
          break;
        }
      }
      if (!expect(lexer::TokenKind::RParen, "`)`")) {
        return nullptr;
      }
      ast::TupleExpr* tuple = arena_.create<ast::TupleExpr>();
      tuple->kind = ast::ExprKind::Tuple;
      tuple->span = span_from(mark);
      tuple->elements = ast::copy_to_arena(arena_, elements);
      return tuple;
    }
    case lexer::TokenKind::LBrace: {
      return parse_block_expr();
    }
    case lexer::TokenKind::If: return parse_if();
    case lexer::TokenKind::Match: return parse_match();
    case lexer::TokenKind::Loop: return parse_loop();
    case lexer::TokenKind::While: return parse_while();
    case lexer::TokenKind::Ret: {
      advance();
      ast::ReturnExpr* ret = arena_.create<ast::ReturnExpr>();
      ret->kind = ast::ExprKind::Return;
      ret->value = nullptr;
      if (!check(lexer::TokenKind::Semicolon) &&
          !check(lexer::TokenKind::RBrace) && !at_end()) {
        ret->value = parse_expr();
        if (ret->value == nullptr) {
          return nullptr;
        }
      }
      ret->span = span_from(mark);
      return ret;
    }
    case lexer::TokenKind::Break: {
      advance();
      ast::BreakExpr* stop = arena_.create<ast::BreakExpr>();
      stop->kind = ast::ExprKind::Break;
      stop->span = span_from(mark);
      return stop;
    }
    case lexer::TokenKind::Continue: {
      advance();
      ast::ContinueExpr* next = arena_.create<ast::ContinueExpr>();
      next->kind = ast::ExprKind::Continue;
      next->span = span_from(mark);
      return next;
    }
    default: break;
  }
  ast::Path* path = parse_path();
  if (path == nullptr) {
    return nullptr;
  }
  if (match(lexer::TokenKind::LParen)) {
    std::vector<ast::Expr*> args;
    if (!check(lexer::TokenKind::RParen)) {
      while (true) {
        ast::Expr* arg = parse_expr();
        if (arg == nullptr) {
          return nullptr;
        }
        args.push_back(arg);
        if (!match(lexer::TokenKind::Comma)) {
          break;
        }
        if (check(lexer::TokenKind::RParen)) {
          break;
        }
      }
    }
    if (!expect(lexer::TokenKind::RParen, "`)`")) {
      return nullptr;
    }
    ast::PathExpr* callee = arena_.create<ast::PathExpr>();
    callee->kind = ast::ExprKind::Path;
    callee->span = path->span;
    callee->path = path;
    ast::CallExpr* call = arena_.create<ast::CallExpr>();
    call->kind = ast::ExprKind::Call;
    call->span = span_from(mark);
    call->callee = callee;
    call->args = ast::copy_to_arena(arena_, args);
    return call;
  }
  if (allow_struct_lit_ && match(lexer::TokenKind::LBrace)) {
    std::vector<ast::FieldInit> fields;
    const ast::Expr* base_expr = nullptr;
    while (!check(lexer::TokenKind::RBrace) && !at_end()) {
      if (match(lexer::TokenKind::DotDot)) {
        base_expr = parse_expr();
        if (base_expr == nullptr) {
          return nullptr;
        }
        break;
      }
      base::Result<ast::Ident, diag::Fatal> name = parse_ident("field name");
      if (name.is_err()) {
        return nullptr;
      }
      if (!expect(lexer::TokenKind::Colon, "`:`")) {
        return nullptr;
      }
      const ast::Expr* value = parse_expr();
      if (value == nullptr) {
        return nullptr;
      }
      fields.push_back(ast::FieldInit{std::move(name).unwrap(), value});
      if (!match(lexer::TokenKind::Comma)) {
        break;
      }
    }
    if (base_expr == nullptr && match(lexer::TokenKind::DotDot)) {
      base_expr = parse_expr();
      if (base_expr == nullptr) {
        return nullptr;
      }
    }
    if (!expect(lexer::TokenKind::RBrace, "`}`")) {
      return nullptr;
    }
    ast::StructExpr* init = arena_.create<ast::StructExpr>();
    init->kind = ast::ExprKind::Struct;
    init->span = span_from(mark);
    init->path = path;
    init->init = ast::copy_to_arena(arena_, fields);
    init->base_expr = base_expr;
    return init;
  }
  ast::PathExpr* expr = arena_.create<ast::PathExpr>();
  expr->kind = ast::ExprKind::Path;
  expr->span = span_from(mark);
  expr->path = path;
  return expr;
}

ast::Cond* Parser::parse_cond() {
  // Pattern declarations (`if pat := expr`) are detected by scanning
  // for ":=" at bracket depth zero; everything else is an expression.
  if (scan_lead() != StmtLead::Decl) {
    // Struct literals stay out so a following "{" reads as the body.
    const bool saved = allow_struct_lit_;
    allow_struct_lit_ = false;
    ast::Expr* value = parse_expr();
    allow_struct_lit_ = saved;
    if (value == nullptr) {
      return nullptr;
    }
    ast::Cond* cond = arena_.create<ast::Cond>();
    cond->is_pattern = false;
    cond->pattern = nullptr;
    cond->init = nullptr;
    cond->value = value;
    return cond;
  }
  const ast::Pattern* pattern = parse_pattern();
  if (pattern == nullptr) {
    return nullptr;
  }
  if (!expect(lexer::TokenKind::ColonEq, "`:=`")) {
    return nullptr;
  }
  const ast::Expr* init = parse_expr();
  if (init == nullptr) {
    return nullptr;
  }
  ast::Cond* cond = arena_.create<ast::Cond>();
  cond->is_pattern = true;
  cond->pattern = pattern;
  cond->init = init;
  cond->value = nullptr;
  return cond;
}

ast::Expr* Parser::parse_if() {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::If, "if")) {
    return nullptr;
  }
  ast::Cond* cond = parse_cond();
  if (cond == nullptr) {
    return nullptr;
  }
  const ast::Block* then_block = parse_block();
  if (then_block == nullptr) {
    return nullptr;
  }
  const ast::Block* else_block = nullptr;
  if (match(lexer::TokenKind::Else)) {
    else_block = parse_block();
    if (else_block == nullptr) {
      return nullptr;
    }
  }
  ast::IfExpr* expr = arena_.create<ast::IfExpr>();
  expr->kind = ast::ExprKind::If;
  expr->span = span_from(mark);
  expr->cond = cond;
  expr->then_block = then_block;
  expr->else_block = else_block;
  return expr;
}

ast::Expr* Parser::parse_match() {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Match, "match")) {
    return nullptr;
  }
  const ast::Expr* scrutinee = nullptr;
  {
    // Struct literals stay out so the following "{" reads as the arm
    // block; parenthesize to match on a struct value.
    const bool saved = allow_struct_lit_;
    allow_struct_lit_ = false;
    scrutinee = parse_expr();
    allow_struct_lit_ = saved;
  }
  if (scrutinee == nullptr) {
    return nullptr;
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return nullptr;
  }
  std::vector<ast::MatchArm> arms;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Comma) || match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    const ast::Pattern* pattern = parse_pattern();
    if (pattern == nullptr) {
      synchronize();
      continue;
    }
    if (!expect(lexer::TokenKind::FatArrow, "`=>`")) {
      synchronize();
      continue;
    }
    const ast::Expr* body = parse_expr();
    if (body == nullptr) {
      synchronize();
      continue;
    }
    arms.push_back(ast::MatchArm{pattern, body});
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return nullptr;
  }
  ast::MatchExpr* expr = arena_.create<ast::MatchExpr>();
  expr->kind = ast::ExprKind::Match;
  expr->span = span_from(mark);
  expr->scrutinee = scrutinee;
  expr->arms = ast::copy_to_arena(arena_, arms);
  return expr;
}

ast::Expr* Parser::parse_loop() {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Loop, "loop")) {
    return nullptr;
  }
  const ast::Block* body = parse_block();
  if (body == nullptr) {
    return nullptr;
  }
  ast::LoopExpr* expr = arena_.create<ast::LoopExpr>();
  expr->kind = ast::ExprKind::Loop;
  expr->span = span_from(mark);
  expr->body = body;
  return expr;
}

ast::Expr* Parser::parse_while() {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::While, "while")) {
    return nullptr;
  }
  ast::Cond* cond = parse_cond();
  if (cond == nullptr) {
    return nullptr;
  }
  const ast::Block* body = parse_block();
  if (body == nullptr) {
    return nullptr;
  }
  ast::WhileExpr* expr = arena_.create<ast::WhileExpr>();
  expr->kind = ast::ExprKind::While;
  expr->span = span_from(mark);
  expr->cond = cond;
  expr->body = body;
  return expr;
}

ast::Expr* Parser::parse_block_expr() {
  const usize mark = pos_;
  const ast::Block* block = parse_block();
  if (block == nullptr) {
    return nullptr;
  }
  ast::BlockExpr* expr = arena_.create<ast::BlockExpr>();
  expr->kind = ast::ExprKind::Block;
  expr->span = span_from(mark);
  expr->block = block;
  return expr;
}

ast::Block* Parser::parse_block() {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return nullptr;
  }
  std::vector<ast::Stmt*> statements;
  const ast::Expr* value = nullptr;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    ast::Stmt* stmt = parse_stmt();
    if (stmt == nullptr) {
      synchronize();
      continue;
    }
    if (stmt->kind == ast::StmtKind::Expr &&
        (check(lexer::TokenKind::RBrace) || at_end())) {
      const ast::ExprStmt* expr_stmt = static_cast<const ast::ExprStmt*>(stmt);
      value = expr_stmt->value;
      break;
    }
    statements.push_back(stmt);
    if (!match(lexer::TokenKind::Semicolon) &&
        !check(lexer::TokenKind::RBrace) && !at_end()) {
      const u32 index = bag_.emit(diag::Severity::Error, kParserUnexpectedToken,
                                  peek().span, "expected `;`");
      (void)index;
      synchronize();
    }
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return nullptr;
  }
  ast::Block* block = arena_.create<ast::Block>();
  block->span = span_from(mark);
  block->statements = ast::copy_to_arena(arena_, statements);
  block->value = value;
  return block;
}

ast::Stmt* Parser::parse_stmt() {
  const usize mark = pos_;
  const StmtLead lead = scan_lead();
  if (lead == StmtLead::Decl) {
    const ast::Pattern* pattern = parse_pattern();
    if (pattern == nullptr) {
      return nullptr;
    }
    const ast::Type* type = nullptr;
    if (match(lexer::TokenKind::Colon)) {
      type = parse_closed_type();
      if (type == nullptr) {
        return nullptr;
      }
    }
    if (!expect(lexer::TokenKind::ColonEq, "`:=`")) {
      return nullptr;
    }
    const ast::Expr* init = parse_expr();
    if (init == nullptr) {
      return nullptr;
    }
    ast::DeclStmt* stmt = arena_.create<ast::DeclStmt>();
    stmt->kind = ast::StmtKind::Decl;
    stmt->span = span_from(mark);
    stmt->pattern = pattern;
    stmt->type = type;
    stmt->init = init;
    return stmt;
  }
  if (lead == StmtLead::Reassign) {
    ast::Expr* place = parse_expr();
    if (place == nullptr) {
      return nullptr;
    }
    ast::BinaryOp op = ast::BinaryOp::Add;
    bool compound = false;
    if (match(lexer::TokenKind::Eq)) {
      compound = false;
    } else if (is_compound_assign(peek_kind())) {
      op = compound_op(peek_kind());
      compound = true;
      advance();
    } else {
      expect(lexer::TokenKind::Eq, "`=`");
      return nullptr;
    }
    ast::Expr* value = parse_expr();
    if (value == nullptr) {
      return nullptr;
    }
    ast::ReassignStmt* stmt = arena_.create<ast::ReassignStmt>();
    stmt->kind = ast::StmtKind::Reassign;
    stmt->span = span_from(mark);
    stmt->place = place;
    stmt->compound = compound;
    stmt->op = op;
    stmt->value = value;
    return stmt;
  }
  ast::Expr* value = parse_expr();
  if (value == nullptr) {
    return nullptr;
  }
  ast::ExprStmt* stmt = arena_.create<ast::ExprStmt>();
  stmt->kind = ast::StmtKind::Expr;
  stmt->span = span_from(mark);
  stmt->value = value;
  return stmt;
}

}  // namespace parser
