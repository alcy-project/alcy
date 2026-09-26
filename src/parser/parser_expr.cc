// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
#include "parser/parser.h"

namespace parser {

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
    case lexer::TokenKind::Star: {
      advance();
      ast::ExprIdx inner = parse_unary();
      if (!inner.is_valid()) {
        return ast::ExprIdx::invalid();
      }
      ast::ExprNode node;
      node.kind = ast::ExprKind::Deref;
      node.span = span_from(mark);
      node.payload.set(ast::ExprDeref{.inner = inner});
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
    // A turbofish on the base path (`f::<T>(...)`) belongs to the call
    // that follows it.
    std::vector<ast::TypeIdx> type_args;
    if (ast_.exprs[base].kind == ast::ExprKind::Path) {
      const std::span<const ast::TypeIdx> parsed =
          ast_.exprs[base].payload.get<ast::ExprPath>().type_args;
      type_args.assign(parsed.begin(), parsed.end());
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
      ast::ExprNode node;
      node.kind = ast::ExprKind::Call;
      node.span = span_from(mark);
      node.payload.set(ast::ExprCall{
          .callee = base,
          .type_args = ast::copy_to_arena(ast_.spans, type_args),
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
        base::Result<ast::Ident, diag::Reported> name =
            parse_ident("field name");
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
    case lexer::TokenKind::LBracket: {
      return parse_array_literal();
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
  std::vector<ast::TypeIdx> type_args;
  ast::PathIdx path = parse_path(&type_args);
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
        .type_args = ast::copy_to_arena(ast_.spans, type_args),
    });
    ast::ExprNode call_node;
    call_node.kind = ast::ExprKind::Call;
    call_node.span = span_from(mark);
    call_node.payload.set(ast::ExprCall{
        .callee = ast_.exprs.push_back(callee_node),
        .type_args = ast::copy_to_arena(ast_.spans, type_args),
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
      base::Result<ast::Ident, diag::Reported> name = parse_ident("field name");
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
      .type_args = ast::copy_to_arena(ast_.spans, type_args),
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

ast::ExprIdx Parser::parse_array_literal() {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::LBracket, "`[`")) {
    return ast::ExprIdx::invalid();
  }
  if (check(lexer::TokenKind::RBracket)) {
    const u32 index =
        bag_.emit(diag::Severity::Error, PARSER_UNEXPECTED_TOKEN, peek().span,
                  "array literal needs elements or a repeat count");
    (void)index;
    return ast::ExprIdx::invalid();
  }
  ast::ExprIdx first = parse_expr();
  if (!first.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  if (match(lexer::TokenKind::Semicolon)) {
    if (peek_kind() != lexer::TokenKind::Integer) {
      expect(lexer::TokenKind::Integer, "array repeat count");
      return ast::ExprIdx::invalid();
    }
    u64 count = 0;
    if (!parse_decimal_u64(&count)) {
      return ast::ExprIdx::invalid();
    }
    advance();
    if (!expect(lexer::TokenKind::RBracket, "`]`")) {
      return ast::ExprIdx::invalid();
    }
    ast::ExprNode node;
    node.kind = ast::ExprKind::Array;
    node.span = span_from(mark);
    node.payload.set(ast::ExprArray{
        .elements = {},
        .repeat = first,
        .count = count,
    });
    return ast_.exprs.push_back(node);
  }
  std::vector<ast::ExprIdx> elements;
  elements.push_back(first);
  while (!check(lexer::TokenKind::RBracket) && !at_end()) {
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
    if (check(lexer::TokenKind::RBracket)) {
      break;
    }
    ast::ExprIdx element = parse_expr();
    if (!element.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    elements.push_back(element);
  }
  if (!expect(lexer::TokenKind::RBracket, "`]`")) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::Array;
  node.span = span_from(mark);
  node.payload.set(ast::ExprArray{
      .elements = ast::copy_to_arena(ast_.spans, elements),
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
        diag::Severity::Error, PARSER_UNEXPECTED_TOKEN, peek().span,
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

}  // namespace parser
