// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "lexer/token.h"
#include "parser/parser.h"

namespace parser {

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
    case T::LBracket: {
      advance();
      ast::TypeIdx element = parse_type();
      if (!element.is_valid()) {
        return ast::TypeIdx::invalid();
      }
      if (!expect(T::Semicolon, "`;`")) {
        return ast::TypeIdx::invalid();
      }
      if (peek_kind() != T::Integer) {
        expect(T::Integer, "array length");
        return ast::TypeIdx::invalid();
      }
      u64 count = 0;
      if (!parse_decimal_u64(&count)) {
        return ast::TypeIdx::invalid();
      }
      advance();
      if (!expect(T::RBracket, "`]`")) {
        return ast::TypeIdx::invalid();
      }
      ast::TypeNode node;
      node.kind = ast::TypeKind::Array;
      node.span = span_from(mark);
      node.payload.set(ast::TypeArray{
          .element = element,
          .count = count,
      });
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
      // `mut self` receivers spell the name with the module keyword;
      // accept it like an identifier.
      if (peek_kind() == lexer::TokenKind::Self) {
        const diag::Span span = peek().span;
        advance();
        ast::PatternNode node;
        node.kind = ast::PatternKind::MutIdent;
        node.span = span_from(mark);
        node.payload.mut_ident.name = ast::Ident{"self", span};
        return ast_.patterns.push_back(node);
      }
      base::Result<ast::Ident, diag::Reported> name =
          parse_ident("pattern name");
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
      base::Result<ast::Ident, diag::Reported> name = parse_ident("field name");
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

}  // namespace parser
