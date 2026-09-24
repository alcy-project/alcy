// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parser/parser.h"

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
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
               ast::AstArena& ast,
               diag::DiagBag& bag)
    : tokens_(tokens),
      bytes_(bytes),
      file_(file),
      ast_(ast),
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
  i32 depth = 0;
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
  i32 depth = 0;
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

std::span<const ast::ItemIdx> Parser::parse() {
  std::vector<ast::ItemIdx> items;
  while (!at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    ast::ItemIdx item = parse_item();
    if (!item.is_valid()) {
      synchronize();
      continue;
    }
    items.push_back(item);
  }
  return ast::copy_to_arena(ast_.spans, items);
}

ast::ItemIdx Parser::parse_item() {
  const bool is_pub = match(lexer::TokenKind::Pub);
  switch (peek_kind()) {
    case lexer::TokenKind::Fn: return parse_fn(is_pub);
    case lexer::TokenKind::Intrinsic: return parse_intrinsic_fn(is_pub);
    case lexer::TokenKind::Struct: return parse_struct(is_pub);
    case lexer::TokenKind::Enum: return parse_enum(is_pub);
    case lexer::TokenKind::Impl: return parse_impl(is_pub);
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
    return ast::ItemIdx::invalid();
  }
  const diag::Span span = peek().span;
  const u32 index = bag_.emit(diag::Severity::Error, kParserUnexpectedToken,
                              span, "expected item, found `{}`",
                              bytes_.substr(span.offset, span.length));
  (void)index;
  return ast::ItemIdx::invalid();
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

ast::PathIdx Parser::parse_path() {
  const usize mark = pos_;
  std::vector<ast::Ident> segments;
  while (true) {
    if (!is_path_segment(peek_kind())) {
      if (segments.empty()) {
        expect(lexer::TokenKind::Ident, "path");
        return ast::PathIdx::invalid();
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
      return ast::PathIdx::invalid();
    }
  }
  ast::Path path;
  path.segments = ast::copy_to_arena(ast_.spans, segments);
  path.span = span_from(mark);
  return ast_.paths.push_back(path);
}

ast::ItemIdx Parser::parse_fn(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Fn, "function")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Fatal> name = parse_ident("function name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::LParen, "`(`")) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemFnParam> params;
  if (!parse_fn_params(params)) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::RParen, "`)`")) {
    return ast::ItemIdx::invalid();
  }
  ast::TypeIdx return_type = ast::TypeIdx::invalid();
  if (match(lexer::TokenKind::Arrow)) {
    return_type = parse_closed_type();
    if (!return_type.is_valid()) {
      return ast::ItemIdx::invalid();
    }
  }
  ast::BlockIdx body = parse_block();
  if (!body.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Fn;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemFn{
      .name = std::move(name).unwrap(),
      .params = ast::copy_to_arena(ast_.spans, params),
      .return_type = return_type,
      .body = body,
  });
  return ast_.items.push_back(node);
}

bool Parser::parse_fn_params(std::vector<ast::ItemFnParam>& params) {
  if (!check(lexer::TokenKind::RParen)) {
    while (true) {
      const bool is_comp = match(lexer::TokenKind::Comp);
      ast::PatternIdx pattern = parse_pattern();
      if (!pattern.is_valid()) {
        return false;
      }
      if (!expect(lexer::TokenKind::Colon, "`:`")) {
        return false;
      }
      ast::TypeIdx type = parse_closed_type();
      if (!type.is_valid()) {
        return false;
      }
      params.push_back(ast::ItemFnParam{pattern, type, is_comp});
      if (!match(lexer::TokenKind::Comma)) {
        break;
      }
      if (check(lexer::TokenKind::RParen)) {
        break;
      }
    }
  }
  return true;
}

ast::ItemIdx Parser::parse_intrinsic_fn(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Intrinsic, "`intrinsic`")) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::Fn, "function")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Fatal> name = parse_ident("function name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::LParen, "`(`")) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemFnParam> params;
  if (!parse_fn_params(params)) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::RParen, "`)`")) {
    return ast::ItemIdx::invalid();
  }
  ast::TypeIdx return_type = ast::TypeIdx::invalid();
  if (match(lexer::TokenKind::Arrow)) {
    return_type = parse_closed_type();
    if (!return_type.is_valid()) {
      return ast::ItemIdx::invalid();
    }
  }
  // Intrinsic declarations carry no body: the signature ends here.
  if (!expect(lexer::TokenKind::Semicolon, "`;`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Intrinsic;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemIntrinsic{
      .name = std::move(name).unwrap(),
      .params = ast::copy_to_arena(ast_.spans, params),
      .return_type = return_type,
  });
  return ast_.items.push_back(node);
}

ast::ItemIdx Parser::parse_struct(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Struct, "struct")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Fatal> name = parse_ident("struct name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemStructField> fields;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    base::Result<ast::Ident, diag::Fatal> field_name =
        parse_ident("field name");
    if (field_name.is_err()) {
      return ast::ItemIdx::invalid();
    }
    if (!expect(lexer::TokenKind::Colon, "`:`")) {
      return ast::ItemIdx::invalid();
    }
    ast::TypeIdx type = parse_closed_type();
    if (!type.is_valid()) {
      return ast::ItemIdx::invalid();
    }
    fields.push_back(
        ast::ItemStructField{std::move(field_name).unwrap(), type});
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Struct;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemStruct{
      .name = std::move(name).unwrap(),
      .fields = ast::copy_to_arena(ast_.spans, fields),
  });
  return ast_.items.push_back(node);
}

ast::ItemIdx Parser::parse_enum(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Enum, "enum")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Fatal> name = parse_ident("enum name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemEnumVariant> variants;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    base::Result<ast::Ident, diag::Fatal> variant_name =
        parse_ident("variant name");
    if (variant_name.is_err()) {
      return ast::ItemIdx::invalid();
    }
    std::vector<ast::TypeIdx> fields;
    if (match(lexer::TokenKind::LParen)) {
      if (!check(lexer::TokenKind::RParen)) {
        while (true) {
          ast::TypeIdx field = parse_type();
          if (!field.is_valid()) {
            return ast::ItemIdx::invalid();
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
        return ast::ItemIdx::invalid();
      }
    }
    variants.push_back(
        ast::ItemEnumVariant{std::move(variant_name).unwrap(),
                             ast::copy_to_arena(ast_.spans, fields)});
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Enum;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemEnum{
      .name = std::move(name).unwrap(),
      .variants = ast::copy_to_arena(ast_.spans, variants),
  });
  return ast_.items.push_back(node);
}

ast::ItemIdx Parser::parse_impl(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Impl, "impl")) {
    return ast::ItemIdx::invalid();
  }
  ast::TypeIdx type = parse_closed_type();
  if (!type.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemIdx> methods;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    ast::ItemIdx method = parse_fn(false);
    if (!method.is_valid()) {
      synchronize();
      continue;
    }
    methods.push_back(method);
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Impl;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemImpl{
      .type = type,
      .methods = ast::copy_to_arena(ast_.spans, methods),
  });
  return ast_.items.push_back(node);
}

ast::ItemIdx Parser::parse_static(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Static, "static")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Fatal> name = parse_ident("static name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::Colon, "`:`")) {
    return ast::ItemIdx::invalid();
  }
  ast::TypeIdx type = parse_closed_type();
  if (!type.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::Eq, "`=`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ExprIdx init = parse_expr();
  if (!init.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Static;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemStatic{
      .name = std::move(name).unwrap(),
      .type = type,
      .init = init,
  });
  return ast_.items.push_back(node);
}

ast::ItemIdx Parser::parse_const(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Const, "const")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Fatal> name = parse_ident("const name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::Colon, "`:`")) {
    return ast::ItemIdx::invalid();
  }
  ast::TypeIdx type = parse_closed_type();
  if (!type.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::Eq, "`=`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ExprIdx init = parse_expr();
  if (!init.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Const;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemConst{
      .name = std::move(name).unwrap(),
      .type = type,
      .init = init,
  });
  return ast_.items.push_back(node);
}

ast::ItemIdx Parser::parse_use(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Use, "use")) {
    return ast::ItemIdx::invalid();
  }
  ast::PathIdx path = parse_path();
  if (!path.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Use;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemUse{
      .path = path,
      .has_alias = false,
      .alias = ast::Ident{},
  });
  if (match(lexer::TokenKind::As)) {
    base::Result<ast::Ident, diag::Fatal> alias = parse_ident("alias");
    if (alias.is_err()) {
      return ast::ItemIdx::invalid();
    }
    node.payload.set(ast::ItemUse{
        .path = node.payload.get<ast::ItemUse>().path,
        .has_alias = true,
        .alias = std::move(alias).unwrap(),
    });
  }
  if (!expect(lexer::TokenKind::Semicolon, "`;`")) {
    return ast::ItemIdx::invalid();
  }
  return ast_.items.push_back(node);
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

ast::TypeIdx Parser::parse_type() {
  const usize mark = pos_;
  switch (peek_kind()) {
    using T = lexer::TokenKind;
    case T::LParen: {
      advance();
      if (match(T::RParen)) {
        ast::TypeNode node;
        node.kind = ast::TypeKind::Unit;
        node.span = span_from(mark);
        return ast_.types.push_back(node);
      }
      ast::TypeIdx first = parse_type();
      if (!first.is_valid()) {
        return ast::TypeIdx::invalid();
      }
      if (!match(T::Comma)) {
        if (!expect(T::RParen, "`)`")) {
          return ast::TypeIdx::invalid();
        }
        return first;
      }
      std::vector<ast::TypeIdx> elements;
      elements.push_back(first);
      while (!check(T::RParen) && !at_end()) {
        ast::TypeIdx element = parse_type();
        if (!element.is_valid()) {
          return ast::TypeIdx::invalid();
        }
        elements.push_back(element);
        if (!match(T::Comma)) {
          break;
        }
      }
      if (!expect(T::RParen, "`)`")) {
        return ast::TypeIdx::invalid();
      }
      ast::TypeNode node;
      node.kind = ast::TypeKind::Tuple;
      node.span = span_from(mark);
      node.payload.set(ast::TypeTuple{
          .elements = ast::copy_to_arena(ast_.spans, elements),
      });
      return ast_.types.push_back(node);
    }
    case T::Bang: {
      advance();
      ast::TypeNode node;
      node.kind = ast::TypeKind::Never;
      node.span = span_from(mark);
      return ast_.types.push_back(node);
    }
    case T::Amp: {
      advance();
      const bool is_mut = match(T::Mut);
      ast::TypeIdx inner = parse_type();
      if (!inner.is_valid()) {
        return ast::TypeIdx::invalid();
      }
      ast::TypeNode node;
      node.kind = ast::TypeKind::Ref;
      node.span = span_from(mark);
      node.payload.set(ast::TypeRef{
          .is_mut = is_mut,
          .inner = inner,
      });
      return ast_.types.push_back(node);
    }
    case T::Str: {
      advance();
      ast::TypeNode node;
      node.kind = ast::TypeKind::Str;
      node.span = span_from(mark);
      return ast_.types.push_back(node);
    }
    case T::I8:
    case T::I16:
    case T::I32:
    case T::I64:
    case T::I128:
    case T::Isize:
    case T::U8:
    case T::U16:
    case T::U32:
    case T::U64:
    case T::U128:
    case T::Usize:
    case T::F32:
    case T::F64:
    case T::Bool: {
      const T kind = peek_kind();
      advance();
      ast::TypeNode node;
      node.kind = ast::TypeKind::Primitive;
      node.span = span_from(mark);

      using P = ast::PrimitiveKind;
      P type_kind = P::I8;
      switch (kind) {
        case T::I8: type_kind = P::I8; break;
        case T::I16: type_kind = P::I16; break;
        case T::I32: type_kind = P::I32; break;
        case T::I64: type_kind = P::I64; break;
        case T::I128: type_kind = P::I128; break;
        case T::Isize: type_kind = P::Isize; break;
        case T::U8: type_kind = P::U8; break;
        case T::U16: type_kind = P::U16; break;
        case T::U32: type_kind = P::U32; break;
        case T::U64: type_kind = P::U64; break;
        case T::U128: type_kind = P::U128; break;
        case T::Usize: type_kind = P::Usize; break;
        case T::F32: type_kind = P::F32; break;
        case T::F64: type_kind = P::F64; break;
        default: type_kind = P::Bool; break;
      }
      node.payload.set(ast::TypePrimitive{
          .primitive = type_kind,
      });
      return ast_.types.push_back(node);
    }
    default: break;
  }
  ast::PathIdx path = parse_path();
  if (!path.is_valid()) {
    return ast::TypeIdx::invalid();
  }
  ast::TypeNode node;
  node.kind = ast::TypeKind::Path;
  const ast::PathIdx path_idx = path;
  if (match(lexer::TokenKind::Less)) {
    std::vector<ast::TypeIdx> args;
    if (!check(lexer::TokenKind::Greater) &&
        !check(lexer::TokenKind::GreaterGreater) && !at_end()) {
      while (true) {
        ast::TypeIdx arg = parse_type();
        if (!arg.is_valid()) {
          return ast::TypeIdx::invalid();
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
      return ast::TypeIdx::invalid();
    }
    node.payload.set(ast::TypePath{
        .path = path_idx,
        .args = ast::copy_to_arena(ast_.spans, args),
    });
  } else {
    node.payload.set(ast::TypePath{
        .path = path_idx,
        .args = {},
    });
  }
  node.span = span_from(mark);
  return ast_.types.push_back(node);
}

ast::TypeIdx Parser::parse_closed_type() {
  ast::TypeIdx type = parse_type();
  if (!type.is_valid()) {
    return ast::TypeIdx::invalid();
  }
  if (banked_gt_ > 0) {
    banked_gt_ = 0;
    expect(lexer::TokenKind::Greater, "`>`");
    return ast::TypeIdx::invalid();
  }
  return type;
}

ast::PatternIdx Parser::parse_pattern() {
  return parse_or_pattern();
}

ast::PatternIdx Parser::parse_or_pattern() {
  const usize mark = pos_;
  ast::PatternIdx first = parse_primary_pattern();
  if (!first.is_valid()) {
    return ast::PatternIdx::invalid();
  }
  if (!match(lexer::TokenKind::Pipe)) {
    return first;
  }
  std::vector<ast::PatternIdx> alternatives;
  alternatives.push_back(first);
  while (true) {
    ast::PatternIdx alternative = parse_primary_pattern();
    if (!alternative.is_valid()) {
      return ast::PatternIdx::invalid();
    }
    alternatives.push_back(alternative);
    if (!match(lexer::TokenKind::Pipe)) {
      break;
    }
  }
  ast::PatternNode node;
  node.kind = ast::PatternKind::Or;
  node.span = span_from(mark);
  node.payload.or_pat.alternatives =
      ast::copy_to_arena(ast_.spans, alternatives);
  return ast_.patterns.push_back(node);
}

ast::PatternIdx Parser::parse_primary_pattern() {
  const usize mark = pos_;
  switch (peek_kind()) {
    case lexer::TokenKind::Underscore: {
      advance();
      ast::PatternNode node;
      node.kind = ast::PatternKind::Wildcard;
      node.span = span_from(mark);
      return ast_.patterns.push_back(node);
    }
    case lexer::TokenKind::Mut: {
      advance();
      base::Result<ast::Ident, diag::Fatal> name = parse_ident("pattern name");
      if (name.is_err()) {
        return ast::PatternIdx::invalid();
      }
      ast::PatternNode node;
      node.kind = ast::PatternKind::MutIdent;
      node.span = span_from(mark);
      node.payload.mut_ident.name = std::move(name).unwrap();
      return ast_.patterns.push_back(node);
    }
    case lexer::TokenKind::Integer:
    case lexer::TokenKind::Float:
    case lexer::TokenKind::String:
    case lexer::TokenKind::Char:
    case lexer::TokenKind::True:
    case lexer::TokenKind::False: {
      ast::LiteralKind kind = ast::LiteralKind::Integer;
      switch (peek_kind()) {
        case lexer::TokenKind::Integer: kind = ast::LiteralKind::Integer; break;
        case lexer::TokenKind::Float: kind = ast::LiteralKind::Float; break;
        case lexer::TokenKind::String: kind = ast::LiteralKind::String; break;
        case lexer::TokenKind::Char: kind = ast::LiteralKind::Char; break;
        default: kind = ast::LiteralKind::Bool; break;
      }
      const diag::Span span = peek().span;
      const ast::LiteralIdx value = ast_.literals.push_back(
          ast::Literal{kind, span, bytes_.substr(span.offset, span.length)});
      advance();
      ast::PatternNode node;
      node.kind = ast::PatternKind::Literal;
      node.span = span_from(mark);
      node.payload.literal.value = value;
      return ast_.patterns.push_back(node);
    }
    case lexer::TokenKind::Minus: {
      // Negative number patterns ("-1"): the span covers the sign so
      // later stages read the value with its sign.
      advance();
      if (peek_kind() != lexer::TokenKind::Integer &&
          peek_kind() != lexer::TokenKind::Float) {
        expect(lexer::TokenKind::Integer, "number literal");
        return ast::PatternIdx::invalid();
      }
      const bool is_float = peek_kind() == lexer::TokenKind::Float;
      const diag::Span span = peek().span;
      const ast::LiteralIdx value = ast_.literals.push_back(ast::Literal{
          is_float ? ast::LiteralKind::Float : ast::LiteralKind::Integer, span,
          bytes_.substr(span.offset, span.length)});
      advance();
      ast::PatternNode node;
      node.kind = ast::PatternKind::Literal;
      node.span = span_from(mark);
      node.payload.literal.value = value;
      return ast_.patterns.push_back(node);
    }
    case lexer::TokenKind::Amp: {
      advance();
      const bool is_mut = match(lexer::TokenKind::Mut);
      ast::PatternIdx inner = parse_primary_pattern();
      if (!inner.is_valid()) {
        return ast::PatternIdx::invalid();
      }
      ast::PatternNode node;
      node.kind = ast::PatternKind::Ref;
      node.span = span_from(mark);
      node.payload.ref.is_mut = is_mut;
      node.payload.ref.inner = inner;
      return ast_.patterns.push_back(node);
    }
    case lexer::TokenKind::LParen: {
      advance();
      ast::PatternIdx first = parse_pattern();
      if (!first.is_valid()) {
        return ast::PatternIdx::invalid();
      }
      if (!match(lexer::TokenKind::Comma)) {
        if (!expect(lexer::TokenKind::RParen, "`)`")) {
          return ast::PatternIdx::invalid();
        }
        return first;
      }
      std::vector<ast::PatternIdx> elements;
      elements.push_back(first);
      while (!check(lexer::TokenKind::RParen) && !at_end()) {
        ast::PatternIdx element = parse_pattern();
        if (!element.is_valid()) {
          return ast::PatternIdx::invalid();
        }
        elements.push_back(element);
        if (!match(lexer::TokenKind::Comma)) {
          break;
        }
      }
      if (!expect(lexer::TokenKind::RParen, "`)`")) {
        return ast::PatternIdx::invalid();
      }
      ast::PatternNode node;
      node.kind = ast::PatternKind::Tuple;
      node.span = span_from(mark);
      node.payload.tuple.path = ast::PathIdx::invalid();
      node.payload.tuple.elements = ast::copy_to_arena(ast_.spans, elements);
      return ast_.patterns.push_back(node);
    }
    default: break;
  }
  ast::PathIdx path = parse_path();
  if (!path.is_valid()) {
    return ast::PatternIdx::invalid();
  }
  const std::span<const ast::Ident> segments = ast_.paths[path].segments;
  // A lone lowercase identifier binds a variable; anything else names a
  // unit variant (uppercase `None`, qualified `Option::None`). Name
  // resolution refines this, but the parser must commit to a shape.
  if (segments.size() == 1) {
    const std::string_view name = segments[0].name;
    if (!name.empty() &&
        (name.front() == '_' || (name.front() >= 'a' && name.front() <= 'z'))) {
      ast::PatternNode node;
      node.kind = ast::PatternKind::Ident;
      node.span = span_from(mark);
      node.payload.ident.name = segments[0];
      return ast_.patterns.push_back(node);
    }
  }
  if (match(lexer::TokenKind::LParen)) {
    std::vector<ast::PatternIdx> elements;
    if (!check(lexer::TokenKind::RParen)) {
      while (true) {
        ast::PatternIdx element = parse_pattern();
        if (!element.is_valid()) {
          return ast::PatternIdx::invalid();
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
      return ast::PatternIdx::invalid();
    }
    ast::PatternNode node;
    node.kind = ast::PatternKind::Tuple;
    node.span = span_from(mark);
    node.payload.tuple.path = path;
    node.payload.tuple.elements = ast::copy_to_arena(ast_.spans, elements);
    return ast_.patterns.push_back(node);
  }
  if (match(lexer::TokenKind::LBrace)) {
    std::vector<ast::FieldPattern> fields;
    while (!check(lexer::TokenKind::RBrace) && !at_end()) {
      base::Result<ast::Ident, diag::Fatal> name = parse_ident("field name");
      if (name.is_err()) {
        return ast::PatternIdx::invalid();
      }
      ast::PatternIdx sub = ast::PatternIdx::invalid();
      if (match(lexer::TokenKind::Colon)) {
        sub = parse_pattern();
        if (!sub.is_valid()) {
          return ast::PatternIdx::invalid();
        }
      }
      ast::Ident bound = std::move(name).unwrap();
      if (!sub.is_valid()) {
        // Shorthand `Foo { x }` binds the field to a variable of the
        // same name.
        ast::PatternNode shorthand;
        shorthand.kind = ast::PatternKind::Ident;
        shorthand.span = bound.span;
        shorthand.payload.ident.name = bound;
        sub = ast_.patterns.push_back(shorthand);
      }
      fields.push_back(ast::FieldPattern{bound, sub});
      if (!match(lexer::TokenKind::Comma)) {
        break;
      }
    }
    if (!expect(lexer::TokenKind::RBrace, "`}`")) {
      return ast::PatternIdx::invalid();
    }
    ast::PatternNode node;
    node.kind = ast::PatternKind::Struct;
    node.span = span_from(mark);
    node.payload.strukt.path = path;
    node.payload.strukt.fields = ast::copy_to_arena(ast_.spans, fields);
    return ast_.patterns.push_back(node);
  }
  // A bare path is a unit-variant pattern.
  ast::PatternNode node;
  node.kind = ast::PatternKind::Tuple;
  node.span = span_from(mark);
  node.payload.tuple.path = path;
  node.payload.tuple.elements = {};
  return ast_.patterns.push_back(node);
}

ast::ExprIdx Parser::parse_expr() {
  return parse_range();
}

ast::ExprIdx Parser::parse_range() {
  const usize mark = pos_;
  if (check(lexer::TokenKind::DotDot) || check(lexer::TokenKind::DotDotEq) ||
      check(lexer::TokenKind::DotDotLess)) {
    const lexer::TokenKind kind = peek_kind();
    advance();
    ast::ExprIdx end = ast::ExprIdx::invalid();
    if (kind != lexer::TokenKind::DotDot && !at_end() &&
        !check(lexer::TokenKind::RBrace) &&
        !check(lexer::TokenKind::RBracket) &&
        !check(lexer::TokenKind::RParen) && !check(lexer::TokenKind::Comma) &&
        !check(lexer::TokenKind::Semicolon)) {
      end = parse_or();
      if (!end.is_valid()) {
        return ast::ExprIdx::invalid();
      }
    }
    ast::ExprNode node;
    node.kind = ast::ExprKind::Range;
    node.span = span_from(mark);
    node.payload.set(ast::ExprRange{
        .start = ast::ExprIdx::invalid(),
        .end = end,
        .inclusive = (kind == lexer::TokenKind::DotDotEq),
    });
    return ast_.exprs.push_back(node);
  }
  ast::ExprIdx lhs = parse_or();
  if (!lhs.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  if (!check(lexer::TokenKind::DotDot) && !check(lexer::TokenKind::DotDotEq) &&
      !check(lexer::TokenKind::DotDotLess)) {
    return lhs;
  }
  const lexer::TokenKind kind = peek_kind();
  advance();
  ast::ExprIdx end = ast::ExprIdx::invalid();
  if (kind != lexer::TokenKind::DotDot && !at_end() &&
      !check(lexer::TokenKind::RBrace) && !check(lexer::TokenKind::RBracket) &&
      !check(lexer::TokenKind::RParen) && !check(lexer::TokenKind::Comma) &&
      !check(lexer::TokenKind::Semicolon)) {
    end = parse_or();
    if (!end.is_valid()) {
      return ast::ExprIdx::invalid();
    }
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::Range;
  node.span = span_from(mark);
  node.payload.set(ast::ExprRange{
      .start = lhs,
      .end = end,
      .inclusive = (kind == lexer::TokenKind::DotDotEq),
  });
  return ast_.exprs.push_back(node);
}

ast::ExprIdx Parser::parse_or() {
  const usize mark = pos_;
  ast::ExprIdx lhs = parse_and();
  if (!lhs.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  while (match(lexer::TokenKind::PipePipe)) {
    ast::ExprIdx rhs = parse_and();
    if (!rhs.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    ast::ExprNode node;
    node.kind = ast::ExprKind::Binary;
    node.span = span_from(mark);
    node.payload.set(ast::ExprBinary{
        .op = ast::BinaryOp::Or,
        .lhs = lhs,
        .rhs = rhs,
    });
    lhs = ast_.exprs.push_back(node);
  }
  return lhs;
}

ast::ExprIdx Parser::parse_and() {
  const usize mark = pos_;
  ast::ExprIdx lhs = parse_cmp();
  if (!lhs.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  while (match(lexer::TokenKind::AmpAmp)) {
    ast::ExprIdx rhs = parse_cmp();
    if (!rhs.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    ast::ExprNode node;
    node.kind = ast::ExprKind::Binary;
    node.span = span_from(mark);
    node.payload.set(ast::ExprBinary{
        .op = ast::BinaryOp::And,
        .lhs = lhs,
        .rhs = rhs,
    });
    lhs = ast_.exprs.push_back(node);
  }
  return lhs;
}

ast::ExprIdx Parser::parse_cmp() {
  const usize mark = pos_;
  ast::ExprIdx lhs = parse_bitor();
  if (!lhs.is_valid()) {
    return ast::ExprIdx::invalid();
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
  ast::ExprIdx rhs = parse_bitor();
  if (!rhs.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::Binary;
  node.span = span_from(mark);
  node.payload.set(ast::ExprBinary{
      .op = op,
      .lhs = lhs,
      .rhs = rhs,
  });
  return ast_.exprs.push_back(node);
}

ast::ExprIdx Parser::parse_bitor() {
  const usize mark = pos_;
  ast::ExprIdx lhs = parse_bitxor();
  if (!lhs.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  while (match(lexer::TokenKind::Pipe)) {
    ast::ExprIdx rhs = parse_bitxor();
    if (!rhs.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    ast::ExprNode node;
    node.kind = ast::ExprKind::Binary;
    node.span = span_from(mark);
    node.payload.set(ast::ExprBinary{
        .op = ast::BinaryOp::BitOr,
        .lhs = lhs,
        .rhs = rhs,
    });
    lhs = ast_.exprs.push_back(node);
  }
  return lhs;
}

ast::ExprIdx Parser::parse_bitxor() {
  const usize mark = pos_;
  ast::ExprIdx lhs = parse_bitand();
  if (!lhs.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  while (match(lexer::TokenKind::Caret)) {
    ast::ExprIdx rhs = parse_bitand();
    if (!rhs.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    ast::ExprNode node;
    node.kind = ast::ExprKind::Binary;
    node.span = span_from(mark);
    node.payload.set(ast::ExprBinary{
        .op = ast::BinaryOp::BitXor,
        .lhs = lhs,
        .rhs = rhs,
    });
    lhs = ast_.exprs.push_back(node);
  }
  return lhs;
}

ast::ExprIdx Parser::parse_bitand() {
  const usize mark = pos_;
  ast::ExprIdx lhs = parse_shift();
  if (!lhs.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  while (match(lexer::TokenKind::Amp)) {
    ast::ExprIdx rhs = parse_shift();
    if (!rhs.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    ast::ExprNode node;
    node.kind = ast::ExprKind::Binary;
    node.span = span_from(mark);
    node.payload.set(ast::ExprBinary{
        .op = ast::BinaryOp::BitAnd,
        .lhs = lhs,
        .rhs = rhs,
    });
    lhs = ast_.exprs.push_back(node);
  }
  return lhs;
}

ast::ExprIdx Parser::parse_shift() {
  const usize mark = pos_;
  ast::ExprIdx lhs = parse_add();
  if (!lhs.is_valid()) {
    return ast::ExprIdx::invalid();
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
    ast::ExprIdx rhs = parse_add();
    if (!rhs.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    ast::ExprNode node;
    node.kind = ast::ExprKind::Binary;
    node.span = span_from(mark);
    node.payload.set(ast::ExprBinary{
        .op = op,
        .lhs = lhs,
        .rhs = rhs,
    });
    lhs = ast_.exprs.push_back(node);
  }
}

ast::ExprIdx Parser::parse_add() {
  const usize mark = pos_;
  ast::ExprIdx lhs = parse_mul();
  if (!lhs.is_valid()) {
    return ast::ExprIdx::invalid();
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
    ast::ExprIdx rhs = parse_mul();
    if (!rhs.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    ast::ExprNode node;
    node.kind = ast::ExprKind::Binary;
    node.span = span_from(mark);
    node.payload.set(ast::ExprBinary{
        .op = op,
        .lhs = lhs,
        .rhs = rhs,
    });
    lhs = ast_.exprs.push_back(node);
  }
}

ast::ExprIdx Parser::parse_mul() {
  const usize mark = pos_;
  ast::ExprIdx lhs = parse_pow();
  if (!lhs.is_valid()) {
    return ast::ExprIdx::invalid();
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
    ast::ExprIdx rhs = parse_pow();
    if (!rhs.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    ast::ExprNode node;
    node.kind = ast::ExprKind::Binary;
    node.span = span_from(mark);
    node.payload.set(ast::ExprBinary{
        .op = op,
        .lhs = lhs,
        .rhs = rhs,
    });
    lhs = ast_.exprs.push_back(node);
  }
}

ast::ExprIdx Parser::parse_pow() {
  const usize mark = pos_;
  ast::ExprIdx lhs = parse_cast();
  if (!lhs.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  if (!match(lexer::TokenKind::StarStar)) {
    return lhs;
  }
  ast::ExprIdx rhs = parse_pow();
  if (!rhs.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::Binary;
  node.span = span_from(mark);
  node.payload.set(ast::ExprBinary{
      .op = ast::BinaryOp::Pow,
      .lhs = lhs,
      .rhs = rhs,
  });
  return ast_.exprs.push_back(node);
}

ast::ExprIdx Parser::parse_cast() {
  const usize mark = pos_;
  ast::ExprIdx inner = parse_unary();
  if (!inner.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  if (!match(lexer::TokenKind::As)) {
    return inner;
  }
  ast::TypeIdx type = parse_closed_type();
  if (!type.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::Cast;
  node.span = span_from(mark);
  node.payload.set(ast::ExprCast{
      .inner = inner,
      .type = type,
  });
  return ast_.exprs.push_back(node);
}

ast::ExprIdx Parser::parse_unary() {
  const usize mark = pos_;
  ast::UnaryOp op = ast::UnaryOp::Neg;
  switch (peek_kind()) {
    case lexer::TokenKind::Minus: op = ast::UnaryOp::Neg; break;
    case lexer::TokenKind::Bang: op = ast::UnaryOp::Not; break;
    case lexer::TokenKind::Tilde: op = ast::UnaryOp::BitNot; break;
    case lexer::TokenKind::Amp: {
      advance();
      const bool is_mut = match(lexer::TokenKind::Mut);
      ast::ExprIdx inner = parse_unary();
      if (!inner.is_valid()) {
        return ast::ExprIdx::invalid();
      }
      ast::ExprNode node;
      node.kind = ast::ExprKind::Borrow;
      node.span = span_from(mark);
      node.payload.set(ast::ExprBorrow{
          .is_mut = is_mut,
          .inner = inner,
      });
      return ast_.exprs.push_back(node);
    }
    default: return parse_postfix();
  }
  advance();
  ast::ExprIdx inner = parse_unary();
  if (!inner.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::Unary;
  node.span = span_from(mark);
  node.payload.set(ast::ExprUnary{
      .op = op,
      .inner = inner,
  });
  return ast_.exprs.push_back(node);
}

ast::ExprIdx Parser::parse_postfix() {
  const usize mark = pos_;
  ast::ExprIdx base = parse_primary();
  if (!base.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  while (true) {
    if (match(lexer::TokenKind::LParen)) {
      std::vector<ast::ExprIdx> args;
      if (!check(lexer::TokenKind::RParen)) {
        while (true) {
          ast::ExprIdx arg = parse_expr();
          if (!arg.is_valid()) {
            return ast::ExprIdx::invalid();
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
        return ast::ExprIdx::invalid();
      }
      ast::ExprNode node;
      node.kind = ast::ExprKind::Call;
      node.span = span_from(mark);
      node.payload.set(ast::ExprCall{
          .callee = base,
          .args = ast::copy_to_arena(ast_.spans, args),
      });
      base = ast_.exprs.push_back(node);
    } else if (match(lexer::TokenKind::Dot)) {
      if (peek_kind() == lexer::TokenKind::Integer) {
        const diag::Span span = peek().span;
        ast::Ident name{bytes_.substr(span.offset, span.length), span};
        advance();
        ast::ExprNode node;
        node.kind = ast::ExprKind::Field;
        node.span = span_from(mark);
        node.payload.set(ast::ExprField{
            .receiver = base,
            .name = name,
        });
        base = ast_.exprs.push_back(node);
      } else {
        base::Result<ast::Ident, diag::Fatal> name = parse_ident("field name");
        if (name.is_err()) {
          return ast::ExprIdx::invalid();
        }
        if (match(lexer::TokenKind::LParen)) {
          std::vector<ast::ExprIdx> args;
          if (!check(lexer::TokenKind::RParen)) {
            while (true) {
              ast::ExprIdx arg = parse_expr();
              if (!arg.is_valid()) {
                return ast::ExprIdx::invalid();
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
            return ast::ExprIdx::invalid();
          }
          ast::ExprNode method_node;
          method_node.kind = ast::ExprKind::MethodCall;
          method_node.span = span_from(mark);
          method_node.payload.set(ast::ExprMethodCall{
              .receiver = base,
              .name = std::move(name).unwrap(),
              .args = ast::copy_to_arena(ast_.spans, args),
          });
          base = ast_.exprs.push_back(method_node);
        } else {
          ast::ExprNode field_node;
          field_node.kind = ast::ExprKind::Field;
          field_node.span = span_from(mark);
          field_node.payload.set(ast::ExprField{
              .receiver = base,
              .name = std::move(name).unwrap(),
          });
          base = ast_.exprs.push_back(field_node);
        }
      }
    } else if (match(lexer::TokenKind::LBracket)) {
      ast::ExprIdx index = parse_expr();
      if (!index.is_valid()) {
        return ast::ExprIdx::invalid();
      }
      if (!expect(lexer::TokenKind::RBracket, "`]`")) {
        return ast::ExprIdx::invalid();
      }
      ast::ExprNode index_node;
      index_node.kind = ast::ExprKind::Index;
      index_node.span = span_from(mark);
      index_node.payload.set(ast::ExprIndex{
          .receiver = base,
          .index = index,
      });
      base = ast_.exprs.push_back(index_node);
    } else if (match(lexer::TokenKind::Question)) {
      ast::ExprNode question_node;
      question_node.kind = ast::ExprKind::Question;
      question_node.span = span_from(mark);
      question_node.payload.set(ast::ExprQuestion{
          .inner = base,
      });
      base = ast_.exprs.push_back(question_node);
    } else {
      return base;
    }
  }
}

ast::ExprIdx Parser::parse_primary() {
  const usize mark = pos_;
  switch (peek_kind()) {
    case lexer::TokenKind::Integer:
    case lexer::TokenKind::Float:
    case lexer::TokenKind::String:
    case lexer::TokenKind::Char:
    case lexer::TokenKind::True:
    case lexer::TokenKind::False: {
      ast::LiteralKind kind = ast::LiteralKind::Bool;
      switch (peek_kind()) {
        case lexer::TokenKind::Integer: kind = ast::LiteralKind::Integer; break;
        case lexer::TokenKind::Float: kind = ast::LiteralKind::Float; break;
        case lexer::TokenKind::String: kind = ast::LiteralKind::String; break;
        case lexer::TokenKind::Char: kind = ast::LiteralKind::Char; break;
        default: kind = ast::LiteralKind::Bool; break;
      }
      const diag::Span span = peek().span;
      const ast::LiteralIdx value = ast_.literals.push_back(
          ast::Literal{kind, span, bytes_.substr(span.offset, span.length)});
      advance();
      ast::ExprNode lit_node;
      lit_node.kind = ast::ExprKind::Literal;
      lit_node.span = span_from(mark);
      lit_node.payload.set(ast::ExprLiteral{
          .value = value,
      });
      return ast_.exprs.push_back(lit_node);
    }
    case lexer::TokenKind::LParen: {
      advance();
      if (check(lexer::TokenKind::RParen)) {
        advance();
        ast::ExprNode unit_node;
        unit_node.kind = ast::ExprKind::Tuple;
        unit_node.span = span_from(mark);
        unit_node.payload.set(ast::ExprTuple{
            .elements = {},
        });
        return ast_.exprs.push_back(unit_node);
      }
      ast::ExprIdx first = parse_expr();
      if (!first.is_valid()) {
        return ast::ExprIdx::invalid();
      }
      if (!match(lexer::TokenKind::Comma)) {
        if (!expect(lexer::TokenKind::RParen, "`)`")) {
          return ast::ExprIdx::invalid();
        }
        return first;
      }
      std::vector<ast::ExprIdx> elements;
      elements.push_back(first);
      while (!check(lexer::TokenKind::RParen) && !at_end()) {
        ast::ExprIdx element = parse_expr();
        if (!element.is_valid()) {
          return ast::ExprIdx::invalid();
        }
        elements.push_back(element);
        if (!match(lexer::TokenKind::Comma)) {
          break;
        }
      }
      if (!expect(lexer::TokenKind::RParen, "`)`")) {
        return ast::ExprIdx::invalid();
      }
      ast::ExprNode tuple_node;
      tuple_node.kind = ast::ExprKind::Tuple;
      tuple_node.span = span_from(mark);
      tuple_node.payload.set(ast::ExprTuple{
          .elements = ast::copy_to_arena(ast_.spans, elements),
      });
      return ast_.exprs.push_back(tuple_node);
    }
    case lexer::TokenKind::LBrace: {
      return parse_block_expr();
    }
    case lexer::TokenKind::Comp: {
      return parse_comp_block();
    }
    case lexer::TokenKind::If: return parse_if();
    case lexer::TokenKind::Match: return parse_match();
    case lexer::TokenKind::Loop: return parse_loop();
    case lexer::TokenKind::While: return parse_while();
    case lexer::TokenKind::Ret: {
      advance();
      ast::ExprIdx ret_val = ast::ExprIdx::invalid();
      if (!check(lexer::TokenKind::Semicolon) &&
          !check(lexer::TokenKind::RBrace) && !at_end()) {
        ret_val = parse_expr();
        if (!ret_val.is_valid()) {
          return ast::ExprIdx::invalid();
        }
      }
      ast::ExprNode ret_node;
      ret_node.kind = ast::ExprKind::Return;
      ret_node.span = span_from(mark);
      ret_node.payload.set(ast::ExprReturn{
          .value = ret_val,
      });
      return ast_.exprs.push_back(ret_node);
    }
    case lexer::TokenKind::Break: {
      advance();
      ast::ExprNode stop_node;
      stop_node.kind = ast::ExprKind::Break;
      stop_node.span = span_from(mark);
      return ast_.exprs.push_back(stop_node);
    }
    case lexer::TokenKind::Continue: {
      advance();
      ast::ExprNode next_node;
      next_node.kind = ast::ExprKind::Continue;
      next_node.span = span_from(mark);
      return ast_.exprs.push_back(next_node);
    }
    default: break;
  }
  ast::PathIdx path = parse_path();
  if (!path.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  if (match(lexer::TokenKind::LParen)) {
    std::vector<ast::ExprIdx> args;
    if (!check(lexer::TokenKind::RParen)) {
      while (true) {
        ast::ExprIdx arg = parse_expr();
        if (!arg.is_valid()) {
          return ast::ExprIdx::invalid();
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
      return ast::ExprIdx::invalid();
    }
    ast::ExprNode callee_node;
    callee_node.kind = ast::ExprKind::Path;
    callee_node.span = ast_.paths[path].span;
    callee_node.payload.set(ast::ExprPath{
        .idx = path,
    });
    ast::ExprNode call_node;
    call_node.kind = ast::ExprKind::Call;
    call_node.span = span_from(mark);
    call_node.payload.set(ast::ExprCall{
        .callee = ast_.exprs.push_back(callee_node),
        .args = ast::copy_to_arena(ast_.spans, args),
    });
    return ast_.exprs.push_back(call_node);
  }
  if (allow_struct_lit_ && match(lexer::TokenKind::LBrace)) {
    std::vector<ast::ExprFieldInit> fields;
    ast::ExprIdx base_expr = ast::ExprIdx::invalid();
    while (!check(lexer::TokenKind::RBrace) && !at_end()) {
      if (match(lexer::TokenKind::DotDot)) {
        base_expr = parse_expr();
        if (!base_expr.is_valid()) {
          return ast::ExprIdx::invalid();
        }
        break;
      }
      base::Result<ast::Ident, diag::Fatal> name = parse_ident("field name");
      if (name.is_err()) {
        return ast::ExprIdx::invalid();
      }
      if (!expect(lexer::TokenKind::Colon, "`:`")) {
        return ast::ExprIdx::invalid();
      }
      const ast::ExprIdx value = parse_expr();
      if (!value.is_valid()) {
        return ast::ExprIdx::invalid();
      }
      fields.push_back(ast::ExprFieldInit{std::move(name).unwrap(), value});
      if (!match(lexer::TokenKind::Comma)) {
        break;
      }
    }
    if (!base_expr.is_valid() && match(lexer::TokenKind::DotDot)) {
      base_expr = parse_expr();
      if (!base_expr.is_valid()) {
        return ast::ExprIdx::invalid();
      }
    }
    if (!expect(lexer::TokenKind::RBrace, "`}`")) {
      return ast::ExprIdx::invalid();
    }
    ast::ExprNode init_node;
    init_node.kind = ast::ExprKind::Struct;
    init_node.span = span_from(mark);
    init_node.payload.set(ast::ExprStruct{
        .path = path,
        .init = ast::copy_to_arena(ast_.spans, fields),
        .base_expr = base_expr,
    });
    return ast_.exprs.push_back(init_node);
  }
  ast::ExprNode path_node;
  path_node.kind = ast::ExprKind::Path;
  path_node.span = span_from(mark);
  path_node.payload.set(ast::ExprPath{
      .idx = path,
  });
  return ast_.exprs.push_back(path_node);
}

ast::CondIdx Parser::parse_cond() {
  // Pattern declarations (`if pat := expr`) are detected by scanning
  // for ":=" at bracket depth zero; everything else is an expression.
  if (scan_lead() != StmtLead::Decl) {
    // Struct literals stay out so a following "{" reads as the body.
    const bool saved = allow_struct_lit_;
    allow_struct_lit_ = false;
    ast::ExprIdx value = parse_expr();
    allow_struct_lit_ = saved;
    if (!value.is_valid()) {
      return ast::CondIdx::invalid();
    }
    ast::Cond cond;
    cond.is_pattern = false;
    cond.value = value;
    return ast_.conds.push_back(cond);
  }
  const ast::PatternIdx pattern = parse_pattern();
  if (!pattern.is_valid()) {
    return ast::CondIdx::invalid();
  }
  if (!expect(lexer::TokenKind::ColonEq, "`:=`")) {
    return ast::CondIdx::invalid();
  }
  // Struct literals stay out so a following "{" reads as the body.
  const bool saved = allow_struct_lit_;
  allow_struct_lit_ = false;
  const ast::ExprIdx init = parse_expr();
  allow_struct_lit_ = saved;
  if (!init.is_valid()) {
    return ast::CondIdx::invalid();
  }
  ast::Cond cond;
  cond.is_pattern = true;
  cond.pattern = pattern;
  cond.init = init;
  return ast_.conds.push_back(cond);
}

ast::ExprIdx Parser::parse_if() {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::If, "if")) {
    return ast::ExprIdx::invalid();
  }
  ast::CondIdx cond = parse_cond();
  if (!cond.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  const ast::BlockIdx then_block = parse_block();
  if (!then_block.is_valid()) {
    return ast::ExprIdx::invalid();
  }

  ast::BlockIdx else_block = ast::BlockIdx::invalid();
  if (match(lexer::TokenKind::Else)) {
    if (check(lexer::TokenKind::If)) {
      ast::ExprIdx nested_if = parse_if();
      if (!nested_if.is_valid()) {
        return ast::ExprIdx::invalid();
      }
      ast::Block block;
      block.span = ast_.exprs[nested_if].span;
      block.value = nested_if;
      else_block = ast_.blocks.push_back(block);
    } else {
      else_block = parse_block();
      if (!else_block.is_valid()) {
        return ast::ExprIdx::invalid();
      }
    }
  }

  ast::ExprNode node;
  node.kind = ast::ExprKind::If;
  node.span = span_from(mark);
  node.payload.set(ast::ExprIf{
      .cond = cond,
      .then_block = then_block,
      .else_block = else_block,
  });
  return ast_.exprs.push_back(node);
}

ast::ExprIdx Parser::parse_match() {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Match, "match")) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprIdx scrutinee = ast::ExprIdx::invalid();
  {
    // Struct literals stay out so the following "{" reads as the arm
    // block; parenthesize to match on a struct value.
    const bool saved = allow_struct_lit_;
    allow_struct_lit_ = false;
    scrutinee = parse_expr();
    allow_struct_lit_ = saved;
  }
  if (!scrutinee.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return ast::ExprIdx::invalid();
  }
  std::vector<ast::ExprMatchArm> arms;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Comma) || match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    const ast::PatternIdx pattern = parse_pattern();
    if (!pattern.is_valid()) {
      synchronize();
      continue;
    }
    if (!expect(lexer::TokenKind::FatArrow, "`=>`")) {
      synchronize();
      continue;
    }
    const ast::ExprIdx body = parse_expr();
    if (!body.is_valid()) {
      synchronize();
      continue;
    }
    arms.push_back(ast::ExprMatchArm{pattern, body});
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::Match;
  node.span = span_from(mark);
  node.payload.set(ast::ExprMatch{
      .scrutinee = scrutinee,
      .arms = ast::copy_to_arena(ast_.spans, arms),
  });
  return ast_.exprs.push_back(node);
}

ast::ExprIdx Parser::parse_loop() {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Loop, "loop")) {
    return ast::ExprIdx::invalid();
  }
  const ast::BlockIdx body = parse_block();
  if (!body.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::Loop;
  node.span = span_from(mark);
  node.payload.set(ast::ExprLoop{
      .body = body,
  });
  return ast_.exprs.push_back(node);
}

ast::ExprIdx Parser::parse_while() {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::While, "while")) {
    return ast::ExprIdx::invalid();
  }
  ast::CondIdx cond = parse_cond();
  if (!cond.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  const ast::BlockIdx body = parse_block();
  if (!body.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::While;
  node.span = span_from(mark);
  node.payload.set(ast::ExprWhile{
      .cond = cond,
      .body = body,
  });
  return ast_.exprs.push_back(node);
}

ast::ExprIdx Parser::parse_block_expr() {
  const usize mark = pos_;
  const ast::BlockIdx block = parse_block();
  if (!block.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::Block;
  node.span = span_from(mark);
  node.payload.set(ast::ExprBlock{
      .block = block,
  });
  return ast_.exprs.push_back(node);
}

ast::ExprIdx Parser::parse_comp_block() {
  const usize mark = pos_;
  if (!match(lexer::TokenKind::Comp)) {
    return ast::ExprIdx::invalid();
  }
  if (!check(lexer::TokenKind::LBrace)) {
    const u32 index = bag_.emit(
        diag::Severity::Error, kParserUnexpectedToken, peek().span,
        "`comp` is only allowed on parameters, declarations, and blocks");
    (void)index;
    return ast::ExprIdx::invalid();
  }
  const ast::BlockIdx block = parse_block();
  if (!block.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::Block;
  node.span = span_from(mark);
  node.payload.set(ast::ExprBlock{
      .block = block,
      .is_comp = true,
  });
  return ast_.exprs.push_back(node);
}

ast::BlockIdx Parser::parse_block() {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return ast::BlockIdx::invalid();
  }
  std::vector<ast::StmtIdx> statements;
  ast::ExprIdx value = ast::ExprIdx::invalid();
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    ast::StmtIdx stmt = parse_stmt();
    if (!stmt.is_valid()) {
      synchronize();
      continue;
    }
    if (ast_.stmts[stmt].kind == ast::StmtKind::Expr &&
        (check(lexer::TokenKind::RBrace) || at_end())) {
      value = ast_.stmts[stmt].payload.get<ast::StmtExpr>().value;
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
    return ast::BlockIdx::invalid();
  }
  ast::Block block;
  block.span = span_from(mark);
  block.statements = ast::copy_to_arena(ast_.spans, statements);
  block.value = value;
  return ast_.blocks.push_back(block);
}

ast::StmtIdx Parser::parse_stmt() {
  const usize mark = pos_;
  // Control-flow heads own their `:=` (if-let/while-let conditions);
  // scanning for a declaration lead would misread them as patterns.
  StmtLead lead = scan_lead();
  switch (peek_kind()) {
    case lexer::TokenKind::If:
    case lexer::TokenKind::While:
    case lexer::TokenKind::Loop:
    case lexer::TokenKind::Match: lead = StmtLead::None; break;
    default: break;
  }
  if (lead == StmtLead::Decl) {
    const bool is_comp = match(lexer::TokenKind::Comp);
    const ast::PatternIdx pattern = parse_pattern();
    if (!pattern.is_valid()) {
      return ast::StmtIdx::invalid();
    }
    ast::TypeIdx type = ast::TypeIdx::invalid();
    if (match(lexer::TokenKind::Colon)) {
      type = parse_closed_type();
      if (!type.is_valid()) {
        return ast::StmtIdx::invalid();
      }
    }
    if (!expect(lexer::TokenKind::ColonEq, "`:=`")) {
      return ast::StmtIdx::invalid();
    }
    const ast::ExprIdx init = parse_expr();
    if (!init.is_valid()) {
      return ast::StmtIdx::invalid();
    }
    ast::StmtNode node;
    node.kind = ast::StmtKind::Decl;
    node.span = span_from(mark);
    node.payload.set(ast::StmtDecl{
        .pattern = pattern,
        .type = type,
        .init = init,
        .is_comp = is_comp,
    });
    return ast_.stmts.push_back(node);
  }
  if (lead == StmtLead::Reassign) {
    ast::ExprIdx place = parse_expr();
    if (!place.is_valid()) {
      return ast::StmtIdx::invalid();
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
      return ast::StmtIdx::invalid();
    }
    ast::ExprIdx value = parse_expr();
    if (!value.is_valid()) {
      return ast::StmtIdx::invalid();
    }
    ast::StmtNode node;
    node.kind = ast::StmtKind::Reassign;
    node.span = span_from(mark);
    node.payload.set(ast::StmtReassign{
        .place = place,
        .compound = compound,
        .op = op,
        .value = value,
    });
    return ast_.stmts.push_back(node);
  }
  ast::ExprIdx value = parse_expr();
  if (!value.is_valid()) {
    return ast::StmtIdx::invalid();
  }
  ast::StmtNode node;
  node.kind = ast::StmtKind::Expr;
  node.span = span_from(mark);
  node.payload.set(ast::StmtExpr{
      .value = value,
  });
  return ast_.stmts.push_back(node);
}

}  // namespace parser
