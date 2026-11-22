// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "base/nesting.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/messages.h"
#include "lexer/token.h"
#include "parser/diag_code.h"
#include "parser/parser.h"

namespace parser {

namespace {

// `..`, `..=`, and `..<` all open a range. A range that names an end
// must say how it is bound, so only `..=` (inclusive) and `..<`
// (exclusive) may be followed by an endpoint.
bool is_range_op(lexer::TokenKind kind) {
  return kind == lexer::TokenKind::DotDot ||
         kind == lexer::TokenKind::DotDotEq ||
         kind == lexer::TokenKind::DotDotLess;
}

// Whether the token after a range operator starts the end endpoint. A
// closer, separator, or end of input leaves the endpoint absent, so
// `a[1..]`, `s[..]`, and `f(..)` parse with an unbounded side.
bool starts_range_end(lexer::TokenKind kind) {
  switch (kind) {
    case lexer::TokenKind::RBrace:
    case lexer::TokenKind::RBracket:
    case lexer::TokenKind::RParen:
    case lexer::TokenKind::Comma:
    case lexer::TokenKind::Semicolon:
    case lexer::TokenKind::Eof: return false;
    default: return true;
  }
}

// The binary operators, indexed by the token that spells them: the binding
// power sits in the low nibble, one step per level of the grammar from `||` to
// `**`, a zero power is a token no binary operator spells, and the high byte is
// the operator the node carries. `**` alone groups to the right; a comparison
// cannot group at all, so `a < b < c` stays a syntax error.
constexpr u16 BINARY_PREC_MASK = 0x0F;
constexpr u16 BINARY_RIGHT_ASSOC = 0x10;
constexpr u16 BINARY_NONASSOC = 0x20;
// The lowest binding power: an operator of higher power is one no binary
// expression leaves for its caller.
constexpr u8 BINARY_PREC_LOWEST = 1;

enum class Assoc : u8 { Left, Right, None };

constexpr u16 assoc_flag(Assoc assoc) {
  return assoc == Assoc::Right  ? BINARY_RIGHT_ASSOC
         : assoc == Assoc::None ? BINARY_NONASSOC
                                : 0;
}

constexpr u16 binary_entry(ast::BinaryOp op, u8 prec, Assoc assoc) {
  return static_cast<u16>(static_cast<u16>(op) << 8) |
         static_cast<u16>(assoc_flag(assoc) | prec);
}

constexpr std::array<u16, 256> binary_op_table() {
  std::array<u16, 256> table{};
  using lexer::TokenKind;
  table[static_cast<u8>(TokenKind::PipePipe)] =
      binary_entry(ast::BinaryOp::Or, 2, Assoc::Left);
  table[static_cast<u8>(TokenKind::AmpAmp)] =
      binary_entry(ast::BinaryOp::And, 3, Assoc::Left);
  table[static_cast<u8>(TokenKind::EqEq)] =
      binary_entry(ast::BinaryOp::Eq, 4, Assoc::None);
  table[static_cast<u8>(TokenKind::BangEq)] =
      binary_entry(ast::BinaryOp::NotEq, 4, Assoc::None);
  table[static_cast<u8>(TokenKind::Less)] =
      binary_entry(ast::BinaryOp::Lt, 4, Assoc::None);
  table[static_cast<u8>(TokenKind::LessEq)] =
      binary_entry(ast::BinaryOp::LtEq, 4, Assoc::None);
  table[static_cast<u8>(TokenKind::Greater)] =
      binary_entry(ast::BinaryOp::Gt, 4, Assoc::None);
  table[static_cast<u8>(TokenKind::GreaterEq)] =
      binary_entry(ast::BinaryOp::GtEq, 4, Assoc::None);
  table[static_cast<u8>(TokenKind::Pipe)] =
      binary_entry(ast::BinaryOp::BitOr, 5, Assoc::Left);
  table[static_cast<u8>(TokenKind::Caret)] =
      binary_entry(ast::BinaryOp::BitXor, 6, Assoc::Left);
  table[static_cast<u8>(TokenKind::Amp)] =
      binary_entry(ast::BinaryOp::BitAnd, 7, Assoc::Left);
  table[static_cast<u8>(TokenKind::LessLess)] =
      binary_entry(ast::BinaryOp::Shl, 8, Assoc::Left);
  table[static_cast<u8>(TokenKind::GreaterGreater)] =
      binary_entry(ast::BinaryOp::Shr, 8, Assoc::Left);
  table[static_cast<u8>(TokenKind::Plus)] =
      binary_entry(ast::BinaryOp::Add, 9, Assoc::Left);
  table[static_cast<u8>(TokenKind::Minus)] =
      binary_entry(ast::BinaryOp::Sub, 9, Assoc::Left);
  table[static_cast<u8>(TokenKind::Star)] =
      binary_entry(ast::BinaryOp::Mul, 10, Assoc::Left);
  table[static_cast<u8>(TokenKind::Slash)] =
      binary_entry(ast::BinaryOp::Div, 10, Assoc::Left);
  table[static_cast<u8>(TokenKind::Percent)] =
      binary_entry(ast::BinaryOp::Mod, 10, Assoc::Left);
  table[static_cast<u8>(TokenKind::StarStar)] =
      binary_entry(ast::BinaryOp::Pow, 11, Assoc::Right);
  return table;
}

constexpr std::array<u16, 256> BINARY_OP_TABLE = binary_op_table();

}  // namespace

ast::ExprIdx Parser::parse_expr() {
  if (nesting_exhausted(peek().span)) {
    return ast::ExprIdx::invalid();
  }
  const base::NestingScope scope(nesting_);
  return parse_range();
}

ast::ExprIdx Parser::parse_range() {
  const usize mark = pos_;
  ast::ExprIdx start = ast::ExprIdx::invalid();
  if (!is_range_op(peek_kind())) {
    bool compared = false;
    start = parse_binary(BINARY_PREC_LOWEST, compared);
    if (!start.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    if (!is_range_op(peek_kind())) {
      return start;
    }
  }
  const lexer::TokenKind kind = peek_kind();
  const diag::Span op = peek().span;
  advance();
  ast::ExprIdx end = ast::ExprIdx::invalid();
  const bool opens_end =
      starts_range_end(peek_kind()) &&
      (allow_brace_range_end_ || peek_kind() != lexer::TokenKind::LBrace);
  if (opens_end) {
    bool compared = false;
    end = parse_binary(BINARY_PREC_LOWEST, compared);
    if (!end.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    if (kind == lexer::TokenKind::DotDot) {
      const u32 index = bag_.emit<i18n::Key::ParserRangeEndNeedsBound>(
          DiagCode::RangeEndUnspelled, op);
      (void)index;
      return ast::ExprIdx::invalid();
    }
  } else if (kind != lexer::TokenKind::DotDot) {
    // `..<` and `..=` promise an endpoint; without one the range
    // would silently read as the unbounded `..`.
    const u32 index = bag_.emit<i18n::Key::ParserRangeBoundRequired>(
        DiagCode::RangeEndUnspelled, op);
    (void)index;
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::Range;
  node.span = span_from(mark);
  node.payload.set(ast::ExprRange{
      .start = start,
      .end = end,
      .inclusive = (kind == lexer::TokenKind::DotDotEq),
  });
  return ast_.exprs.push_back(node);
}

ast::ExprIdx Parser::parse_binary(u8 min_prec, bool& compared) {
  const usize mark = pos_;
  compared = false;
  ast::ExprIdx lhs = parse_cast();
  if (!lhs.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  while (true) {
    const u16 entry = BINARY_OP_TABLE[static_cast<u8>(peek_kind())];
    const u8 prec = static_cast<u8>(entry & BINARY_PREC_MASK);
    if (prec == 0 || prec < min_prec) {
      return lhs;
    }
    // A comparison may stand once in front of the operand it compares: a
    // second one closes the expression it belongs to, leaving the token for
    // whoever holds the operand. The flag comes back from the operands a frame
    // builds, so a comparison at the right edge of an operand counts as well.
    if ((entry & BINARY_NONASSOC) != 0) {
      if (compared) {
        return lhs;
      }
      compared = true;
    }
    const u8 next_prec =
        (entry & BINARY_RIGHT_ASSOC) != 0 ? prec : static_cast<u8>(prec + 1);
    advance();
    bool inner = false;
    ast::ExprIdx rhs = parse_binary(next_prec, inner);
    if (!rhs.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    compared = compared || inner;
    ast::ExprNode node;
    node.kind = ast::ExprKind::Binary;
    node.span = span_from(mark);
    node.payload.set(ast::ExprBinary{
        .op = static_cast<ast::BinaryOp>(entry >> 8),
        .lhs = lhs,
        .rhs = rhs,
    });
    lhs = ast_.exprs.push_back(node);
  }
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
  if (nesting_exhausted(peek().span)) {
    return ast::ExprIdx::invalid();
  }
  const base::NestingScope scope(nesting_);
  const usize mark = pos_;
  // An increment or decrement written in prefix position is a
  // neighboring language's spelling; the message names the
  // statement alcy writes. `--i` would otherwise read as negation
  // twice, which parses and means something else.
  if ((check(lexer::TokenKind::Plus) || check(lexer::TokenKind::Minus)) &&
      peek_kind() == peek_kind(1) && doubled_text(pos_)) {
    advance();
    advance();
    const diag::Span span = span_from(mark);
    (void)bag_.emit<i18n::Key::ParserIncrementSpelling>(DiagCode::Spelling,
                                                        span);
    ast::ExprIdx operand = parse_unary();
    if (!operand.is_valid()) {
      return ast::ExprIdx::invalid();
    }
    return operand;
  }
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
    case lexer::TokenKind::Star:
    case lexer::TokenKind::StarStar: {
      // `*p` dereferences once; `**p` lexes as the power token but a
      // binary `**` needs a left operand, so in prefix position the
      // pair is two dereferences.
      const bool paired = peek_kind() == lexer::TokenKind::StarStar;
      advance();
      ast::ExprIdx operand = parse_unary();
      if (!operand.is_valid()) {
        return ast::ExprIdx::invalid();
      }
      ast::ExprNode first;
      first.kind = ast::ExprKind::Deref;
      first.span = span_from(mark);
      first.payload.set(ast::ExprDeref{.inner = operand});
      const ast::ExprIdx deref = ast_.exprs.push_back(first);
      if (!paired) {
        return deref;
      }
      ast::ExprNode second;
      second.kind = ast::ExprKind::Deref;
      second.span = span_from(mark);
      second.payload.set(ast::ExprDeref{.inner = deref});
      return ast_.exprs.push_back(second);
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
    // A macro call (`name!(...)`) is a neighboring language's spelling;
    // read it as the call it meant.
    if (check(lexer::TokenKind::Bang) &&
        (peek_kind(1) == lexer::TokenKind::LParen ||
         peek_kind(1) == lexer::TokenKind::LBracket)) {
      const u32 index = bag_.emit<i18n::Key::ParserMacroCallSpelling>(
          DiagCode::Spelling, peek().span);
      (void)index;
      advance();  // `!`
      continue;
    }
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
        // A method that declares its own type parameters takes explicit
        // arguments after its name (`v.map::<i32>(f)`); only a call can
        // carry them.
        std::vector<ast::TypeIdx> type_args;
        if (match(lexer::TokenKind::ColonColon)) {
          if (!match(lexer::TokenKind::Less)) {
            (void)expect(lexer::TokenKind::Less, "`<`");
            return ast::ExprIdx::invalid();
          }
          if (!parse_turbofish(type_args)) {
            return ast::ExprIdx::invalid();
          }
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
              .type_args = ast::copy_to_arena(ast_.spans, type_args),
          });
          base = ast_.exprs.push_back(method_node);
        } else {
          if (!type_args.empty()) {
            (void)expect(lexer::TokenKind::LParen, "`(`");
            return ast::ExprIdx::invalid();
          }
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
    } else if (check(lexer::TokenKind::Plus) ||
               check(lexer::TokenKind::Minus)) {
      // A postfix increment or decrement is the same neighboring
      // spelling; the message names the statement alcy writes, and the
      // tokens are consumed so the walk continues. A site is one
      // doubled pair, so a chain (`i++ ++`) reports at each pair the
      // reader wrote.
      if (peek_kind() == peek_kind(1) && doubled_text(pos_)) {
        const usize op_mark = pos_;
        advance();
        advance();
        const diag::Span span = span_from(op_mark);
        (void)bag_.emit<i18n::Key::ParserIncrementSpelling>(DiagCode::Spelling,
                                                            span);
        continue;
      }
      return base;
    } else {
      return base;
    }
  }
}

ast::ExprIdx Parser::parse_primary() {
  if (ast_.exhausted()) {
    return ast::ExprIdx::invalid();
  }
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
      if (scan_closure()) {
        return parse_closure();
      }
      advance();
      // Parentheses lift the struct-literal ban a surrounding condition
      // imposes: the matching `)` still ends any literal before the
      // block, so `(Foo { .. })` is unambiguous. A block-valued range
      // end lifts with it.
      const bool saved_lit = allow_struct_lit_;
      const bool saved_brace = allow_brace_range_end_;
      allow_struct_lit_ = true;
      allow_brace_range_end_ = true;
      const ast::ExprIdx inner = parse_paren_expr(mark);
      allow_struct_lit_ = saved_lit;
      allow_brace_range_end_ = saved_brace;
      return inner;
    }
    case lexer::TokenKind::LBrace: {
      return parse_block_expr();
    }
    case lexer::TokenKind::LBracket: {
      if (scan_closure()) {
        return parse_closure();
      }
      return parse_array_literal();
    }
    case lexer::TokenKind::Comp: {
      return parse_comp_block();
    }
    case lexer::TokenKind::Unsafe: {
      return parse_unsafe_block();
    }
    case lexer::TokenKind::If: return parse_if();
    case lexer::TokenKind::Match: return parse_match();
    case lexer::TokenKind::Loop: return parse_loop();
    case lexer::TokenKind::While: return parse_while();
    case lexer::TokenKind::For: return parse_for();
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
  u32 type_args_segment = base::INVALID_IDX;
  ast::PathIdx path = parse_path(&type_args, &type_args_segment);
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
        .type_args_segment = type_args_segment,
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
        .type_args = ast::copy_to_arena(ast_.spans, type_args),
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
      .type_args_segment = type_args_segment,
  });
  return ast_.exprs.push_back(path_node);
}

std::optional<ast::CondIdx> Parser::parse_spelled_condition() {
  using K = lexer::TokenKind;
  const lexer::Token start = peek();
  if (start.kind != K::Ident ||
      bytes_.substr(start.span.offset, start.span.length) != "let") {
    return std::nullopt;
  }
  const K after = peek_kind(1);
  if (after != K::Ident && after != K::Underscore && after != K::LParen &&
      after != K::Integer && after != K::String && after != K::Char &&
      after != K::True && after != K::False) {
    return std::nullopt;
  }
  // Only `if let PAT = EXPR`: a top-level `=` before the body, and no
  // `:=` (which would be an ordinary pattern condition whose binding
  // is named `let`).
  bool matched = false;
  i32 depth = 0;
  for (usize ahead = 1;; ++ahead) {
    const K kind = peek_kind(ahead);
    if (kind == K::Eof || kind == K::LBrace) {
      break;
    }
    if (depth == 0) {
      if (kind == K::ColonEq || kind == K::Semicolon) {
        break;
      }
      if (kind == K::Eq) {
        matched = true;
        break;
      }
    }
    if (kind == K::LParen || kind == K::LBracket) {
      ++depth;
    } else if ((kind == K::RParen || kind == K::RBracket) && depth > 0) {
      --depth;
    }
  }
  if (!matched) {
    return std::nullopt;
  }
  const u32 index =
      bag_.emit<i18n::Key::ParserIfLetSpelling>(DiagCode::Spelling, start.span);
  (void)index;
  advance();  // `let`
  const ast::PatternIdx pattern = parse_pattern();
  if (!pattern.is_valid()) {
    return ast::CondIdx::invalid();
  }
  if (!match(K::Eq) && !match(K::ColonEq)) {
    expect(K::ColonEq, "`:=`");
    return ast::CondIdx::invalid();
  }
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

ast::CondIdx Parser::parse_cond() {
  // Rust's `if let` reads as the pattern condition it meant
  if (const std::optional<ast::CondIdx> spelled = parse_spelled_condition()) {
    return *spelled;
  }
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

// The body of a `(` after the opener is consumed: unit `()`, a
// parenthesized expression, or a tuple. The caller owns the
// struct-literal flag around it.
ast::ExprIdx Parser::parse_paren_expr(usize mark) {
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

// A closure literal after the scan established the shape:
// `[captures] (params) -> body`. The body is one expression, so
// statements and assignment need braces; a `{` body reuses the
// block parser through the expression parser, and a `Path {`
// body is a struct literal wherever one is allowed.
ast::ExprIdx Parser::parse_closure() {
  const usize mark = pos_;
  if (nesting_exhausted(peek().span)) {
    return ast::ExprIdx::invalid();
  }
  const base::NestingScope scope(nesting_);
  std::vector<ast::Capture> captures;
  if (match(lexer::TokenKind::LBracket)) {
    while (!check(lexer::TokenKind::RBracket) && !at_end()) {
      ast::Capture capture;
      // `&name` borrows shared and `&mut name` exclusively; a bare
      // name takes the value (a move, or a copy for a Copy type).
      if (match(lexer::TokenKind::Amp)) {
        capture.mode = match(lexer::TokenKind::Mut) ? ast::CaptureMode::Mut
                                                    : ast::CaptureMode::Shared;
      }
      // `self` is a keyword, and it is a local like any other: a
      // capture list may name the receiver.
      if (check(lexer::TokenKind::Self)) {
        const diag::Span span = peek().span;
        advance();
        capture.name = ast::Ident{"self", span};
      } else {
        base::Result<ast::Ident, diag::Reported> name =
            parse_ident("capture name");
        if (name.is_err()) {
          return ast::ExprIdx::invalid();
        }
        capture.name = std::move(name).unwrap();
      }
      captures.push_back(capture);
      if (!match(lexer::TokenKind::Comma)) {
        break;
      }
    }
    if (!expect(lexer::TokenKind::RBracket, "`]`")) {
      return ast::ExprIdx::invalid();
    }
  }
  if (!expect(lexer::TokenKind::LParen, "`(`")) {
    return ast::ExprIdx::invalid();
  }
  std::vector<ast::ClosureParam> params;
  while (!check(lexer::TokenKind::RParen) && !at_end()) {
    ast::ClosureParam param;
    param.is_mut = match(lexer::TokenKind::Mut);
    if (match(lexer::TokenKind::Underscore)) {
      param.is_wildcard = true;
      const lexer::Token wildcard = previous();
      param.name = ast::Ident{
          bytes_.substr(wildcard.span.offset, wildcard.span.length),
          wildcard.span,
      };
    } else {
      base::Result<ast::Ident, diag::Reported> name =
          parse_ident("parameter name");
      if (name.is_err()) {
        return ast::ExprIdx::invalid();
      }
      param.name = std::move(name).unwrap();
    }
    if (match(lexer::TokenKind::Colon)) {
      param.type = parse_type();
      if (!param.type.is_valid()) {
        return ast::ExprIdx::invalid();
      }
    }
    params.push_back(param);
    if (!match(lexer::TokenKind::Comma)) {
      break;
    }
  }
  if (!expect(lexer::TokenKind::RParen, "`)`")) {
    return ast::ExprIdx::invalid();
  }
  if (!expect(lexer::TokenKind::Arrow, "`->`")) {
    return ast::ExprIdx::invalid();
  }
  const ast::ExprIdx body = parse_expr();
  if (!body.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode closure_node;
  closure_node.kind = ast::ExprKind::Closure;
  closure_node.span = span_from(mark);
  closure_node.payload.set(ast::ExprClosure{
      .captures = ast::copy_to_arena(ast_.spans, captures),
      .params = ast::copy_to_arena(ast_.spans, params),
      .body = body,
  });
  return ast_.exprs.push_back(closure_node);
}

ast::ExprIdx Parser::parse_if(bool is_comp) {
  const usize mark = pos_;
  if (!expect(lexer::TokenKind::If, "if")) {
    return ast::ExprIdx::invalid();
  }
  ast::CondIdx cond = parse_cond();
  if (!cond.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  const ast::BlockIdx then_block = parse_header_block();
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
      else_block = parse_header_block();
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
      .is_comp = is_comp,
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
  if (!expect_header_lbrace()) {
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
    // A guard follows the pattern and sees its bindings:
    // `Some(v) if v > 0 => ...`.
    ast::ExprIdx guard = ast::ExprIdx::invalid();
    if (match(lexer::TokenKind::If)) {
      guard = parse_expr();
      if (!guard.is_valid()) {
        synchronize();
        continue;
      }
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
    arms.push_back(ast::ExprMatchArm{pattern, body, guard});
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
  const ast::BlockIdx body = parse_header_block();
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
  const ast::BlockIdx body = parse_header_block();
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

ast::ExprIdx Parser::parse_for(bool is_comp) {
  const usize mark = pos_;
  const diag::Span keyword = peek().span;
  if (!expect(lexer::TokenKind::For, "for")) {
    return ast::ExprIdx::invalid();
  }
  const ast::PatternIdx pattern = parse_pattern();
  if (!pattern.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  if (!expect(lexer::TokenKind::In, "`in`")) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprIdx head = ast::ExprIdx::invalid();
  {
    // The `{` after the head always opens the loop body: never a
    // struct literal, and never a block-valued range end, so an open
    // range reads as the head (`for i in 0..`). Parenthesize a
    // struct-valued head, or a range ending in a block.
    const bool saved_lit = allow_struct_lit_;
    const bool saved_brace = allow_brace_range_end_;
    allow_struct_lit_ = false;
    allow_brace_range_end_ = false;
    head = parse_expr();
    allow_struct_lit_ = saved_lit;
    allow_brace_range_end_ = saved_brace;
  }
  if (!head.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  const ast::BlockIdx body = parse_header_block();
  if (!body.is_valid()) {
    return ast::ExprIdx::invalid();
  }

  // The one rule of the desugar: `for pat in head {
  // body }` means
  //
  //   {
  //     mut _it := head.into_iter()
  //     loop {
  //       match _it.next() {
  //         Option::Some(pat) => body,
  //         Option::None => break,
  //       }
  //     }
  //   }
  //
  // Everything is built from ordinary nodes, so resolution, checking,
  // and lowering see only the desugared form. The shadowing pass
  // freshens `_it` wherever it nests, so a body that spells `_it`, or
  // an enclosing `for`, keeps its own cursor.
  const diag::Span span = span_from(mark);
  const ast::Ident it_name{"_it", keyword};

  ast::PatternNode it_binding;
  it_binding.kind = ast::PatternKind::MutIdent;
  it_binding.span = keyword;
  it_binding.payload.mut_ident.name = it_name;
  const ast::PatternIdx cursor = ast_.patterns.push_back(it_binding);

  // `head.into_iter()`
  ast::ExprNode into_iter;
  into_iter.kind = ast::ExprKind::MethodCall;
  into_iter.span = keyword;
  into_iter.payload.set(ast::ExprMethodCall{
      .receiver = head,
      .name = ast::Ident{"into_iter", keyword},
      .args = {},
      .type_args = {},
  });
  const ast::ExprIdx iterator = ast_.exprs.push_back(into_iter);

  ast::StmtNode decl;
  decl.kind = ast::StmtKind::Decl;
  decl.span = keyword;
  decl.payload.set(ast::StmtDecl{
      .pattern = cursor,
      .type = ast::TypeIdx::invalid(),
      .init = iterator,
      .is_const = false,
      .for_cursor = true,
  });
  const ast::StmtIdx iterator_decl = ast_.stmts.push_back(decl);

  // `_it.next()`, resolved through specs only: the cursor must reach
  // `next` through `Iterator`, never through an inherent method.
  const std::vector<ast::Ident> it_segments{it_name};
  ast::Path it_path;
  it_path.span = keyword;
  it_path.segments = ast::copy_to_arena(ast_.spans, it_segments);
  ast::ExprNode it_use;
  it_use.kind = ast::ExprKind::Path;
  it_use.span = keyword;
  it_use.payload.set(ast::ExprPath{
      .idx = ast_.paths.push_back(it_path),
      .type_args = {},
  });
  ast::ExprNode next_call;
  next_call.kind = ast::ExprKind::MethodCall;
  next_call.span = keyword;
  next_call.payload.set(ast::ExprMethodCall{
      .receiver = ast_.exprs.push_back(it_use),
      .name = ast::Ident{"next", keyword},
      .args = {},
      .type_args = {},
      .spec_only = true,
  });
  const ast::ExprIdx next = ast_.exprs.push_back(next_call);

  const std::vector<ast::Ident> some_segments{ast::Ident{"Option", keyword},
                                              ast::Ident{"Some", keyword}};
  ast::Path some_path;
  some_path.span = keyword;
  some_path.segments = ast::copy_to_arena(ast_.spans, some_segments);
  const std::vector<ast::PatternIdx> some_elements{pattern};
  ast::PatternNode some;
  some.kind = ast::PatternKind::Tuple;
  some.span = keyword;
  some.payload.tuple.path = ast_.paths.push_back(some_path);
  some.payload.tuple.elements = ast::copy_to_arena(ast_.spans, some_elements);
  some.for_pattern = true;
  const ast::PatternIdx some_pattern = ast_.patterns.push_back(some);

  ast::ExprNode arm_body;
  arm_body.kind = ast::ExprKind::Block;
  arm_body.span = ast_.blocks[body].span;
  arm_body.payload.set(ast::ExprBlock{
      .block = body,
  });
  const ast::ExprIdx body_expr = ast_.exprs.push_back(arm_body);

  const std::vector<ast::Ident> none_segments{ast::Ident{"Option", keyword},
                                              ast::Ident{"None", keyword}};
  ast::Path none_path;
  none_path.span = keyword;
  none_path.segments = ast::copy_to_arena(ast_.spans, none_segments);
  ast::PatternNode none;
  none.kind = ast::PatternKind::Tuple;
  none.span = keyword;
  none.payload.tuple.path = ast_.paths.push_back(none_path);
  none.payload.tuple.elements = {};
  const ast::PatternIdx none_pattern = ast_.patterns.push_back(none);

  ast::ExprNode stop;
  stop.kind = ast::ExprKind::Break;
  stop.span = keyword;
  const ast::ExprIdx stop_expr = ast_.exprs.push_back(stop);

  const std::vector<ast::ExprMatchArm> arms{
      ast::ExprMatchArm{some_pattern, body_expr},
      ast::ExprMatchArm{none_pattern, stop_expr},
  };
  ast::ExprNode match;
  match.kind = ast::ExprKind::Match;
  match.span = span;
  match.payload.set(ast::ExprMatch{
      .scrutinee = next,
      .arms = ast::copy_to_arena(ast_.spans, arms),
  });
  const ast::ExprIdx match_expr = ast_.exprs.push_back(match);

  ast::Block loop_body;
  loop_body.span = span;
  loop_body.value = match_expr;
  ast::ExprNode loop;
  loop.kind = ast::ExprKind::Loop;
  loop.span = span;
  loop.payload.set(ast::ExprLoop{
      .body = ast_.blocks.push_back(loop_body),
      .is_comp = is_comp,
      .from_for = true,
  });
  const ast::ExprIdx loop_expr = ast_.exprs.push_back(loop);

  const std::vector<ast::StmtIdx> statements{iterator_decl};
  ast::Block desugared;
  desugared.span = span;
  desugared.statements = ast::copy_to_arena(ast_.spans, statements);
  desugared.value = loop_expr;
  ast::ExprNode node;
  node.kind = ast::ExprKind::Block;
  node.span = span;
  node.payload.set(ast::ExprBlock{
      .block = ast_.blocks.push_back(desugared),
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
    const u32 index = bag_.emit<i18n::Key::ParserArrayLiteralWithoutCount>(
        DiagCode::UnexpectedToken, peek().span);
    (void)index;
    return ast::ExprIdx::invalid();
  }
  ast::ExprIdx first = parse_expr();
  if (!first.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  if (match(lexer::TokenKind::Semicolon)) {
    // The repeat count is a literal or a path to a `const` integer
    // value, the same grammar the array type's length takes.
    u64 count = 0;
    ast::PathIdx count_path = ast::PathIdx::invalid();
    if (peek_kind() == lexer::TokenKind::Integer) {
      if (!parse_decimal_u64(&count)) {
        return ast::ExprIdx::invalid();
      }
      advance();
    } else {
      count_path = parse_path();
      if (!count_path.is_valid()) {
        return ast::ExprIdx::invalid();
      }
    }
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
        .count_path = count_path,
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

ast::ExprIdx Parser::parse_unsafe_block() {
  const usize mark = pos_;
  if (!match(lexer::TokenKind::Unsafe)) {
    return ast::ExprIdx::invalid();
  }
  if (!check(lexer::TokenKind::LBrace)) {
    const u32 index = bag_.emit<i18n::Key::ParserUnsafeWithoutBlock>(
        DiagCode::UnexpectedToken, peek().span);
    (void)index;
    return ast::ExprIdx::invalid();
  }
  const ast::BlockIdx block = parse_header_block();
  if (!block.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  ast::ExprNode node;
  node.kind = ast::ExprKind::Block;
  node.span = span_from(mark);
  node.payload.set(ast::ExprBlock{
      .block = block,
      .is_comp = false,
      .is_unsafe = true,
  });
  return ast_.exprs.push_back(node);
}

ast::ExprIdx Parser::parse_comp_block() {
  const usize mark = pos_;
  if (!match(lexer::TokenKind::Comp)) {
    return ast::ExprIdx::invalid();
  }
  if (check(lexer::TokenKind::If)) {
    return parse_if(/*is_comp=*/true);
  }
  if (check(lexer::TokenKind::For)) {
    return parse_for(/*is_comp=*/true);
  }
  if (!check(lexer::TokenKind::LBrace)) {
    const u32 index = bag_.emit<i18n::Key::ParserCompOnlyOnBindings>(
        DiagCode::UnexpectedToken, peek().span);
    (void)index;
    return ast::ExprIdx::invalid();
  }
  const ast::BlockIdx block = parse_header_block();
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
