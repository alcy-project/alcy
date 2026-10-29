// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <optional>
#include <string_view>

#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/type.h"
#include "linter/context.h"
#include "linter/rules/rules.h"
#include "linter/types.h"

namespace linter::rules {

namespace {

// Whether moving an expression out of the construct that wraps it
// cannot change how its neighbours group it. A literal, a place, or a
// call binds tighter than every operator around it; a binary, an if, a
// match, or a closure does not, so those keep the wrapping operator and
// the finding is reported without a repair.
bool binds_tightly(ast::ExprKind kind) {
  using K = ast::ExprKind;
  switch (kind) {
    case K::Literal:
    case K::Path:
    case K::Struct:
    case K::Tuple:
    case K::Array:
    case K::Call:
    case K::MethodCall:
    case K::Field:
    case K::Index:
    case K::Unary:
    case K::Borrow:
    case K::Deref:
    case K::Question: return true;
    default: return false;
  }
}

// The source bytes one expression covers, or nullopt when the source
// manager cannot resolve them.
std::optional<std::string_view> source_of(const LintContext& ctx,
                                          const ast::ExprNode& node) {
  const std::optional<std::string_view> bytes =
      ctx.sources.bytes(node.span.file);
  if (!bytes.has_value()) {
    return std::nullopt;
  }
  const usize begin = node.span.offset;
  const usize end = begin + node.span.length;
  if (begin > bytes->size() || end > bytes->size() || end < begin) {
    return std::nullopt;
  }
  return bytes->substr(node.span.offset, node.span.length);
}

// Offers the repair that replaces outer with the bytes of inner, when
// that cannot change the grouping. The replacement is the inner
// expression exactly as the reader wrote it, so the fix reads as the
// construct removed rather than as a rewrite.
void replace_with(LintContext& ctx,
                  u32 finding,
                  const ast::ExprNode& outer,
                  const ast::ExprNode& inner) {
  if (!binds_tightly(inner.kind)) {
    return;
  }
  const std::optional<std::string_view> text = source_of(ctx, inner);
  if (text.has_value()) {
    ctx.fix(finding, outer.span, *text);
  }
}

void check_useless_cast(LintContext& ctx, ast::ExprIdx expr) {
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  if (node.kind != ast::ExprKind::Cast) {
    return;
  }
  const ast::ExprCast& cast = node.payload.get<ast::ExprCast>();
  if (!cast.inner.is_valid()) {
    return;
  }
  const ast::ExprNode& inner = ctx.ast.exprs[cast.inner];
  // A literal that has no type of its own takes the cast's, so the two
  // agree by construction and the cast is how the type was written.
  if (inner.kind == ast::ExprKind::Literal) {
    return;
  }
  const std::optional<ir::TypeIdx> from = expression_type(ctx, cast.inner);
  const std::optional<ir::TypeIdx> to = expression_type(ctx, expr);
  if (!from.has_value() || !to.has_value() || *from != *to) {
    return;
  }
  const u32 finding = ctx.report<i18n::Key::LintUselessCast>(node.span);
  replace_with(ctx, finding, node, inner);
}

void check_float_equality(LintContext& ctx, ast::ExprIdx expr) {
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  if (node.kind != ast::ExprKind::Binary) {
    return;
  }
  const ast::ExprBinary& binary = node.payload.get<ast::ExprBinary>();
  if (binary.op != ast::BinaryOp::Eq && binary.op != ast::BinaryOp::NotEq) {
    return;
  }
  const std::optional<ir::TypeIdx> lhs = expression_type(ctx, binary.lhs);
  const std::optional<ir::TypeIdx> rhs = expression_type(ctx, binary.rhs);
  const bool floating = (lhs.has_value() && is_float_type(ctx, *lhs)) ||
                        (rhs.has_value() && is_float_type(ctx, *rhs));
  if (floating) {
    ctx.report<i18n::Key::LintFloatEquality>(node.span);
  }
}

void check_needless_borrow(LintContext& ctx, ast::ExprIdx expr) {
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  if (node.kind != ast::ExprKind::Borrow) {
    return;
  }
  const ast::ExprBorrow& borrow = node.payload.get<ast::ExprBorrow>();
  if (borrow.is_mut || !borrow.inner.is_valid()) {
    return;
  }
  const ast::ExprNode& inner = ctx.ast.exprs[borrow.inner];
  if (inner.kind != ast::ExprKind::Deref) {
    return;
  }
  const ast::ExprDeref& deref = inner.payload.get<ast::ExprDeref>();
  if (!deref.inner.is_valid()) {
    return;
  }
  // A mutable reference needs the shared reborrow the outer ampersand
  // spells; only a shared reference is the same value without it.
  const std::optional<ir::TypeIdx> type = expression_type(ctx, deref.inner);
  if (!type.has_value() || !is_shared_reference(ctx, *type)) {
    return;
  }
  const ast::ExprNode& source = ctx.ast.exprs[deref.inner];
  const u32 finding = ctx.report<i18n::Key::LintNeedlessBorrow>(node.span);
  replace_with(ctx, finding, node, source);
}

// The bit width of an integer or float tag, or zero for a type the
// rule does not measure.
u32 scalar_bits(ir::TypeTag tag) {
  using T = ir::TypeTag;
  switch (tag) {
    case T::I8:
    case T::U8: return 8;
    case T::I16:
    case T::U16: return 16;
    case T::I32:
    case T::U32:
    case T::F32: return 32;
    case T::I64:
    case T::U64:
    case T::F64: return 64;
    default: return 0;
  }
}

bool is_integer_tag(ir::TypeTag tag) {
  using T = ir::TypeTag;
  return tag == T::I8 || tag == T::U8 || tag == T::I16 || tag == T::U16 ||
         tag == T::I32 || tag == T::U32 || tag == T::I64 || tag == T::U64;
}

bool is_float_tag(ir::TypeTag tag) {
  return tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64;
}

void check_lossy_cast(LintContext& ctx, ast::ExprIdx expr) {
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  if (node.kind != ast::ExprKind::Cast) {
    return;
  }
  const ast::ExprCast& cast = node.payload.get<ast::ExprCast>();
  if (!cast.inner.is_valid()) {
    return;
  }
  // A literal's type comes from the cast, so its width says nothing
  // about a value that exists before the cast.
  if (ctx.ast.exprs[cast.inner].kind == ast::ExprKind::Literal) {
    return;
  }
  const std::optional<ir::TypeIdx> from_type = expression_type(ctx, cast.inner);
  const std::optional<ir::TypeIdx> to_type = expression_type(ctx, expr);
  if (!from_type.has_value() || !to_type.has_value()) {
    return;
  }
  const ir::TypeNode* from = type_node(ctx, *from_type);
  const ir::TypeNode* to = type_node(ctx, *to_type);
  if (from == nullptr || to == nullptr) {
    return;
  }
  const u32 from_bits = scalar_bits(from->tag);
  const u32 to_bits = scalar_bits(to->tag);
  if (from_bits == 0 || to_bits == 0 || from_bits <= to_bits) {
    return;
  }
  // A narrowing between different kinds is a conversion the reader
  // asked for; only the same kind narrowing claims the value fits.
  const bool integer_to_integer =
      is_integer_tag(from->tag) && is_integer_tag(to->tag);
  const bool float_to_float = is_float_tag(from->tag) && is_float_tag(to->tag);
  if (!integer_to_integer && !float_to_float) {
    return;
  }
  ctx.report<i18n::Key::LintLossyCast>(node.span, ir::type_to_str(from->tag),
                                       ir::type_to_str(to->tag));
}

}  // namespace

void UselessCast::check_expr(LintContext& ctx, ast::ExprIdx expr) {
  check_useless_cast(ctx, expr);
}

void FloatEquality::check_expr(LintContext& ctx, ast::ExprIdx expr) {
  check_float_equality(ctx, expr);
}

void NeedlessBorrow::check_expr(LintContext& ctx, ast::ExprIdx expr) {
  check_needless_borrow(ctx, expr);
}

void LossyCast::check_expr(LintContext& ctx, ast::ExprIdx expr) {
  check_lossy_cast(ctx, expr);
}

}  // namespace linter::rules
