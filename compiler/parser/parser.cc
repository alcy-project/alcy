// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parser/parser.h"

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "ast/verify.h"
#include "base/nesting.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/messages.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "parser/diag_code.h"
#include "source/source.h"

namespace parser {

namespace {

bool is_reserved(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::Async:
    case lexer::TokenKind::Await:
    case lexer::TokenKind::Union:
    case lexer::TokenKind::Register:
    case lexer::TokenKind::Extern:
    case lexer::TokenKind::Unsafe:
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
      const u32 index = bag_.emit<i18n::Key::ParserReservedName>(
          diag::Severity::Error, diag::Stage::Parser, DiagCode::ReservedWord,
          span, spelling);
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
    const u32 index = bag_.emit<i18n::Key::ParserExpectedFoundEndOfFile>(
        diag::Severity::Error, diag::Stage::Parser, DiagCode::UnexpectedToken,
        span_from(pos_), what);
    (void)index;
    return false;
  }
  const diag::Span span = peek().span;
  const u32 index = bag_.emit<i18n::Key::ParserExpectedFound>(
      diag::Severity::Error, diag::Stage::Parser, DiagCode::UnexpectedToken,
      span, what, bytes_.substr(span.offset, span.length));
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
      // A `{` is not a boundary by itself: a struct pattern in a
      // declaration or a condition carries one before the `:=`, so
      // braces count as nesting here like parens and brackets.
      if (kind == lexer::TokenKind::Semicolon ||
          kind == lexer::TokenKind::RBrace || kind == lexer::TokenKind::Eof) {
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

bool Parser::scan_closure() const {
  using K = lexer::TokenKind;
  usize i = pos_;
  if (i < tokens_.size() && tokens_[i].kind == K::LBracket) {
    // A capture list: match the brackets, then a parameter list
    // must follow.
    i32 depth = 1;
    ++i;
    while (i < tokens_.size()) {
      const lexer::TokenKind kind = tokens_[i].kind;
      if (kind == K::Error || kind == K::DocComment || is_reserved(kind)) {
        ++i;
        continue;
      }
      if (kind == K::Semicolon || kind == K::RBrace || kind == K::Eof) {
        return false;
      }
      if (kind == K::LBracket || kind == K::LBrace || kind == K::LParen) {
        ++depth;
      } else if (kind == K::RBracket || kind == K::RBrace ||
                 kind == K::RParen) {
        --depth;
        if (depth == 0) {
          ++i;
          break;
        }
      }
      ++i;
    }
  }
  // The token cursor skips lexer errors, doc comments, and reserved
  // words, so the scan has to as well: a doc comment between the
  // capture list and the parameters is not a boundary.
  while (i < tokens_.size()) {
    const lexer::TokenKind kind = tokens_[i].kind;
    if (kind == K::Error || kind == K::DocComment || is_reserved(kind)) {
      ++i;
      continue;
    }
    break;
  }
  if (i >= tokens_.size() || tokens_[i].kind != K::LParen) {
    return false;
  }
  // The parameter list: a `:` at depth zero can only ascribe a
  // closure parameter, and an `->` after the matching `)` can
  // only open a closure body.
  i32 depth = 1;
  ++i;
  while (i < tokens_.size()) {
    const lexer::TokenKind kind = tokens_[i].kind;
    if (kind == K::Error || kind == K::DocComment || is_reserved(kind)) {
      ++i;
      continue;
    }
    if (kind == K::Semicolon || kind == K::RBrace || kind == K::Eof) {
      return false;
    }
    if (depth == 0) {
      return kind == K::Arrow;
    }
    // A `:` at the group's own depth can only ascribe a closure
    // parameter: struct fields nest a level deeper, and `::` is
    // its own token.
    if (kind == K::Colon && depth == 1) {
      return true;
    }
    if (kind == K::LBracket || kind == K::LBrace || kind == K::LParen) {
      ++depth;
    } else if (kind == K::RBracket || kind == K::RBrace || kind == K::RParen) {
      --depth;
    }
    ++i;
  }
  return false;
}

base::Result<std::span<const ast::ItemIdx>, diag::Reported> Parser::parse() {
  if (base::Result<void, lexer::TokenStreamError> verified =
          lexer::verify_token_stream(tokens_, file_, bytes_);
      verified.is_err()) {
    const u32 index = bag_.emit<i18n::Key::ParserInvalidTokenStream>(
        diag::Severity::Error, diag::Stage::Parser,
        DiagCode::InvalidTokenStream,
        lexer::describe_token_stream_error(std::move(verified).unwrap_err()));
    (void)index;
    return base::make_err(diag::Reported{});
  }
  std::vector<ast::ItemIdx> items;
  while (!at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    ast::ItemIdx item = parse_item();
    if (!item.is_valid()) {
      if (ast_.exhausted()) {
        break;
      }
      synchronize();
      continue;
    }
    items.push_back(item);
  }
  return base::make_ok(
      std::span<const ast::ItemIdx>(ast::copy_to_arena(ast_.spans, items)));
}

base::Result<void, diag::Reported> verify_trees(ast::AstArena& ast,
                                                diag::DiagBag& bag) {
  if (base::Result<void, ast::VerificationError> verified =
          ast::verify_file(ast);
      verified.is_err()) {
    const u32 index = bag.emit<i18n::Key::ParserInvalidSyntaxTree>(
        diag::Severity::Error, diag::Stage::Parser, DiagCode::InvalidAst,
        ast::describe_verification_error(std::move(verified).unwrap_err()));
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

ast::ItemIdx Parser::parse_item() {
  // A spent arena builds nothing further: what it refused would leave a
  // node half made and a diagnostic about the wrong thing.
  if (ast_.exhausted()) {
    return ast::ItemIdx::invalid();
  }
  const bool is_pub = match(lexer::TokenKind::Pub);
  switch (peek_kind()) {
    case lexer::TokenKind::Fn: return parse_fn(is_pub);
    case lexer::TokenKind::Intrinsic: return parse_intrinsic_fn(is_pub);
    case lexer::TokenKind::Struct: return parse_struct(is_pub);
    case lexer::TokenKind::Enum: return parse_enum(is_pub);
    case lexer::TokenKind::Impl: return parse_impl(is_pub);
    case lexer::TokenKind::Spec: return parse_spec(is_pub);
    case lexer::TokenKind::Static: return parse_static(is_pub);
    case lexer::TokenKind::Const: return parse_const(is_pub);
    case lexer::TokenKind::Use: return parse_use(is_pub);
    default: break;
  }
  if (at_end()) {
    const u32 index = bag_.emit<i18n::Key::ParserExpectedItemFoundEndOfFile>(
        diag::Severity::Error, diag::Stage::Parser, DiagCode::UnexpectedToken,
        span_from(pos_));
    (void)index;
    return ast::ItemIdx::invalid();
  }
  const diag::Span span = peek().span;
  const u32 index = bag_.emit<i18n::Key::ParserExpectedItemFound>(
      diag::Severity::Error, diag::Stage::Parser, DiagCode::UnexpectedToken,
      span, bytes_.substr(span.offset, span.length));
  (void)index;
  return ast::ItemIdx::invalid();
}

base::Result<ast::Ident, diag::Reported> Parser::parse_ident(
    std::string_view what) {
  if (peek_kind() != lexer::TokenKind::Ident) {
    expect(lexer::TokenKind::Ident, what);
    return base::make_err(diag::Reported{});
  }
  const diag::Span span = peek().span;
  ast::Ident id{bytes_.substr(span.offset, span.length), span};
  advance();
  return base::make_ok(id);
}

ast::PathIdx Parser::parse_path(std::vector<ast::TypeIdx>* type_args) {
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
    if (peek_kind() != lexer::TokenKind::ColonColon) {
      break;
    }
    // `::<` is a turbofish type-argument list rather than another
    // segment. Only an expression path can carry one, so a type path
    // stops here and its caller reads the arguments.
    if (pos_ + 1 >= tokens_.size() ||
        tokens_[pos_ + 1].kind != lexer::TokenKind::Less) {
      advance();
      if (at_end()) {
        expect(lexer::TokenKind::Ident, "path segment");
        return ast::PathIdx::invalid();
      }
      continue;
    }
    if (type_args == nullptr || !type_args->empty()) {
      break;
    }
    advance();
    if (!match(lexer::TokenKind::Less)) {
      expect(lexer::TokenKind::Less, "`<`");
      return ast::PathIdx::invalid();
    }
    if (!parse_turbofish(*type_args)) {
      return ast::PathIdx::invalid();
    }
    // `Name::<T>::member` continues with the member segment.
    if (peek_kind() != lexer::TokenKind::ColonColon) {
      break;
    }
    advance();
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

bool Parser::parse_turbofish(std::vector<ast::TypeIdx>& type_args) {
  while (!check(lexer::TokenKind::Greater) &&
         !check(lexer::TokenKind::GreaterGreater) && !at_end()) {
    ast::TypeIdx arg = parse_type();
    if (!arg.is_valid()) {
      return false;
    }
    type_args.push_back(arg);
    // A nested generic argument absorbs the `>` that closes the list as
    // part of its own `>>`; `consume_gt` drains the banked one.
    if (banked_gt_ > 0) {
      break;
    }
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  if (!consume_gt()) {
    expect(lexer::TokenKind::Greater, "`>`");
    return false;
  }
  return true;
}

bool Parser::parse_fn_signature(FnSignature& signature) {
  if (!expect(lexer::TokenKind::Fn, "function")) {
    return false;
  }
  base::Result<ast::Ident, diag::Reported> name = parse_ident("function name");
  if (name.is_err()) {
    return false;
  }
  signature.name = std::move(name).unwrap();
  if (!parse_generic_params(signature.generic)) {
    return false;
  }
  if (!expect(lexer::TokenKind::LParen, "`(`")) {
    return false;
  }
  if (!parse_fn_params(signature.params)) {
    return false;
  }
  if (!expect(lexer::TokenKind::RParen, "`)`")) {
    return false;
  }
  if (match(lexer::TokenKind::Arrow)) {
    signature.return_type = parse_closed_type();
    if (!signature.return_type.is_valid()) {
      return false;
    }
  }
  return true;
}

ast::ItemIdx Parser::parse_fn(bool is_pub) {
  const usize mark = pos_;
  FnSignature signature;
  if (!parse_fn_signature(signature)) {
    return ast::ItemIdx::invalid();
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
      .name = signature.name,
      .generic = ast::copy_to_arena(ast_.spans, signature.generic),
      .params = ast::copy_to_arena(ast_.spans, signature.params),
      .return_type = signature.return_type,
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
  base::Result<ast::Ident, diag::Reported> name = parse_ident("function name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::Ident> generic;
  if (!parse_generic_params(generic)) {
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
      .generic = ast::copy_to_arena(ast_.spans, generic),
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
  base::Result<ast::Ident, diag::Reported> name = parse_ident("struct name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::Ident> params;
  if (!parse_generic_params(params)) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemStructField> fields;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    base::Result<ast::Ident, diag::Reported> field_name =
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
      .params = ast::copy_to_arena(ast_.spans, params),
      .fields = ast::copy_to_arena(ast_.spans, fields),
  });
  return ast_.items.push_back(node);
}

ast::ItemIdx Parser::parse_enum(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Enum, "enum")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Reported> name = parse_ident("enum name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::Ident> params;
  if (!parse_generic_params(params)) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemEnumVariant> variants;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    base::Result<ast::Ident, diag::Reported> variant_name =
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
      .params = ast::copy_to_arena(ast_.spans, params),
      .variants = ast::copy_to_arena(ast_.spans, variants),
  });
  return ast_.items.push_back(node);
}

bool Parser::parse_generic_params(std::vector<ast::Ident>& params) {
  if (!match(lexer::TokenKind::Less)) {
    return true;
  }
  while (!check(lexer::TokenKind::Greater) && !at_end()) {
    base::Result<ast::Ident, diag::Reported> param =
        parse_ident("type parameter");
    if (param.is_err()) {
      return false;
    }
    ast::Ident name = std::move(param).unwrap();
    for (const ast::Ident& existing : params) {
      if (existing.name == name.name) {
        const u32 index = bag_.emit<i18n::Key::ParserDuplicateTypeParameter>(
            diag::Severity::Error, diag::Stage::Parser,
            DiagCode::UnexpectedToken, name.span, name.name);
        (void)index;
        return false;
      }
    }
    params.emplace_back(name);
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  if (!expect(lexer::TokenKind::Greater, "`>`")) {
    return false;
  }
  return true;
}

ast::ItemIdx Parser::parse_impl(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Impl, "impl")) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::Ident> params;
  if (!parse_generic_params(params)) {
    return ast::ItemIdx::invalid();
  }
  ast::TypeIdx type = parse_closed_type();
  if (!type.is_valid()) {
    return ast::ItemIdx::invalid();
  }
  ast::TypeIdx spec = ast::TypeIdx::invalid();
  if (match(lexer::TokenKind::For)) {
    // `impl S for T`: the type just parsed is the spec side, so it
    // must be a plain path. Spec arguments (`Iterator<T>`) ride the
    // same type grammar as the target.
    if (ast_.types[type].kind != ast::TypeKind::Path) {
      const diag::Span span = ast_.types[type].span;
      const u32 index = bag_.emit<i18n::Key::ParserExpectedFound>(
          diag::Severity::Error, diag::Stage::Parser, DiagCode::UnexpectedToken,
          span, "a spec", bytes_.substr(span.offset, span.length));
      (void)index;
      return ast::ItemIdx::invalid();
    }
    spec = type;
    type = parse_closed_type();
    if (!type.is_valid()) {
      return ast::ItemIdx::invalid();
    }
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::ItemIdx> methods;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    ast::ItemIdx method = parse_fn(match(lexer::TokenKind::Pub));
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
      .params = ast::copy_to_arena(ast_.spans, params),
      .type = type,
      .spec = spec,
      .methods = ast::copy_to_arena(ast_.spans, methods),
  });
  return ast_.items.push_back(node);
}

bool Parser::parse_spec_method(ast::SpecMethod& method) {
  FnSignature signature;
  if (!parse_fn_signature(signature)) {
    return false;
  }
  // No default bodies in v1: a spec declares signatures, and every
  // implementation supplies each one.
  if (!expect(lexer::TokenKind::Semicolon, "`;`")) {
    return false;
  }
  method = ast::SpecMethod{
      .name = signature.name,
      .generic = ast::copy_to_arena(ast_.spans, signature.generic),
      .params = ast::copy_to_arena(ast_.spans, signature.params),
      .return_type = signature.return_type,
  };
  return true;
}

ast::ItemIdx Parser::parse_spec(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Spec, "spec")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Reported> name = parse_ident("spec name");
  if (name.is_err()) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::Ident> params;
  if (!parse_generic_params(params)) {
    return ast::ItemIdx::invalid();
  }
  if (!expect(lexer::TokenKind::LBrace, "`{`")) {
    return ast::ItemIdx::invalid();
  }
  std::vector<ast::SpecMethod> methods;
  while (!check(lexer::TokenKind::RBrace) && !at_end()) {
    if (match(lexer::TokenKind::Semicolon)) {
      continue;
    }
    if (match(lexer::TokenKind::Pub)) {
      // Method visibility is not checked; accept the marker for
      // consistency with inherent impl blocks.
    }
    ast::SpecMethod method;
    if (!parse_spec_method(method)) {
      synchronize();
      continue;
    }
    methods.push_back(method);
  }
  if (!expect(lexer::TokenKind::RBrace, "`}`")) {
    return ast::ItemIdx::invalid();
  }
  ast::ItemNode node;
  node.kind = ast::ItemKind::Spec;
  node.span = span_from(mark);
  node.is_pub = is_pub;
  node.payload.set(ast::ItemSpec{
      .name = std::move(name).unwrap(),
      .params = ast::copy_to_arena(ast_.spans, params),
      .methods = ast::copy_to_arena(ast_.spans, methods),
  });
  return ast_.items.push_back(node);
}

ast::ItemIdx Parser::parse_static(bool is_pub) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::Static, "static")) {
    return ast::ItemIdx::invalid();
  }
  base::Result<ast::Ident, diag::Reported> name = parse_ident("static name");
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
  base::Result<ast::Ident, diag::Reported> name = parse_ident("const name");
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
    base::Result<ast::Ident, diag::Reported> alias = parse_ident("alias");
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

bool Parser::parse_decimal_u64(u64* out) {
  const diag::Span span = peek().span;
  const std::string_view spelling = bytes_.substr(span.offset, span.length);
  u64 value = 0;
  for (char c : spelling) {
    if (c == '_') {
      continue;
    }
    if (c < '0' || c > '9') {
      const u32 index = bag_.emit<i18n::Key::ParserArrayLengthNotDecimal>(
          diag::Severity::Error, diag::Stage::Parser, DiagCode::UnexpectedToken,
          span);
      (void)index;
      return false;
    }
    // Reject rather than wrap: a wrapped length would silently become a
    // smaller array, or zero.
    const u64 digit = static_cast<u64>(c - '0');
    if (value > (~0ull - digit) / 10) {
      const u32 index = bag_.emit<i18n::Key::ParserArrayLengthTooLarge>(
          diag::Severity::Error, diag::Stage::Parser, DiagCode::UnexpectedToken,
          span);
      (void)index;
      return false;
    }
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

bool Parser::nesting_exhausted(diag::Span span) {
  if (!nesting_.exhausted()) {
    return false;
  }
  if (!reported_too_deep_) {
    reported_too_deep_ = true;
    const u32 index = bag_.emit<i18n::Key::ParserNestingTooDeep>(
        diag::Severity::Error, diag::Stage::Parser, DiagCode::TooDeep, span,
        nesting_.limit());
    (void)index;
  }
  return true;
}

ast::BlockIdx Parser::parse_block() {
  if (ast_.exhausted()) {
    return ast::BlockIdx::invalid();
  }
  if (nesting_exhausted(peek().span)) {
    return ast::BlockIdx::invalid();
  }
  const base::NestingScope scope(nesting_);
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
      if (ast_.exhausted()) {
        break;
      }
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
      const u32 index = bag_.emit<i18n::Key::ParserExpectedSemicolon>(
          diag::Severity::Error, diag::Stage::Parser, DiagCode::UnexpectedToken,
          peek().span);
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
  if (ast_.exhausted()) {
    return ast::StmtIdx::invalid();
  }
  const usize mark = pos_;
  // Control-flow heads own their `:=` (if-let/while-let conditions);
  // scanning for a declaration lead would misread them as patterns.
  StmtLead lead = scan_lead();
  switch (peek_kind()) {
    case lexer::TokenKind::If:
    case lexer::TokenKind::While:
    case lexer::TokenKind::Loop:
    case lexer::TokenKind::Match:
    case lexer::TokenKind::For: lead = StmtLead::None; break;
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
