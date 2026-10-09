// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/verify.h"

#include <span>
#include <utility>

#include "ast/ast.h"
#include "diag/span.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "source/source.h"

namespace ast {

namespace {

struct Fixture {
  AstArena ast;
};

diag::Span test_span(u32 offset = 0, u32 length = 1) {
  return diag::Span{source::UNKNOWN_FILE, offset, length};
}

ExprNode return_none() {
  ExprNode expr;
  expr.kind = ExprKind::Return;
  expr.span = test_span();
  expr.payload.set(ExprReturn{.value = ExprIdx::invalid()});
  return expr;
}

}  // namespace

TEST_CASE("Verification accepts an empty arena") {
  Fixture f;
  CHECK(verify_file(f.ast).is_ok());
}

TEST_CASE("Verification allows absent edges") {
  Fixture f;
  f.ast.exprs.emplace_back(return_none());
  CHECK(verify_file(f.ast).is_ok());
}

TEST_CASE("Verification accepts a hand-built valid file") {
  Fixture f;
  const LiteralIdx lit = f.ast.literals.push_back(Literal{
      .kind = LiteralKind::Integer, .span = test_span(), .spelling = "1"});
  ExprNode expr;
  expr.kind = ExprKind::Literal;
  expr.span = test_span();
  expr.payload.set(ExprLiteral{.value = lit});
  const ExprIdx expr_idx = f.ast.exprs.emplace_back(expr);
  const BlockIdx block = f.ast.blocks.emplace_back(
      Block{.span = test_span(), .statements = {}, .value = expr_idx});
  ItemNode item;
  item.kind = ItemKind::Fn;
  item.span = test_span();
  item.is_pub = false;
  item.payload.set(ItemFn{.name = {.name = "main", .span = test_span()},
                          .generic = {},
                          .params = {},
                          .return_type = TypeIdx::invalid(),
                          .body = block});
  f.ast.items.emplace_back(item);
  CHECK(verify_file(f.ast).is_ok());
}

TEST_CASE("Verification rejects a dangling expression edge") {
  Fixture f;
  ExprNode expr;
  expr.kind = ExprKind::Binary;
  expr.span = test_span();
  expr.payload.set(ExprBinary{
      .op = BinaryOp::Add, .lhs = ExprIdx(7), .rhs = ExprIdx::invalid()});
  f.ast.exprs.emplace_back(expr);
  base::Result<void, VerificationError> result = verify_file(f.ast);
  CHECK(result.is_err());
  if (result.is_err()) {
    CHECK(std::move(result).unwrap_err() == VerificationError::DanglingExpr);
  }
}

TEST_CASE("Verification rejects a dangling item edge") {
  Fixture f;
  ItemNode item;
  item.kind = ItemKind::Fn;
  item.span = test_span();
  item.is_pub = false;
  item.payload.set(ItemFn{.name = {.name = "main", .span = test_span()},
                          .generic = {},
                          .params = {},
                          .return_type = TypeIdx::invalid(),
                          .body = BlockIdx(3)});
  f.ast.items.emplace_back(item);
  base::Result<void, VerificationError> result = verify_file(f.ast);
  CHECK(result.is_err());
  if (result.is_err()) {
    CHECK(std::move(result).unwrap_err() == VerificationError::DanglingItem);
  }
}

TEST_CASE("Verification rejects a dangling type edge") {
  Fixture f;
  TypeNode node;
  node.kind = TypeKind::Array;
  node.span = test_span();
  node.payload.set(TypeArray{.element = TypeIdx(4), .count = 2});
  f.ast.types.emplace_back(node);
  base::Result<void, VerificationError> result = verify_file(f.ast);
  CHECK(result.is_err());
  if (result.is_err()) {
    CHECK(std::move(result).unwrap_err() == VerificationError::DanglingType);
  }
}

TEST_CASE("Verification rejects a dangling array length path") {
  Fixture f;
  TypeNode element;
  element.kind = TypeKind::Primitive;
  element.span = test_span();
  element.payload.set(TypePrimitive{.primitive = PrimitiveKind::Usize});
  const TypeIdx element_idx = f.ast.types.emplace_back(element);
  TypeNode node;
  node.kind = TypeKind::Array;
  node.span = test_span();
  node.payload.set(
      TypeArray{.element = element_idx, .count = 0, .length_path = PathIdx(9)});
  f.ast.types.emplace_back(node);
  base::Result<void, VerificationError> result = verify_file(f.ast);
  CHECK(result.is_err());
  if (result.is_err()) {
    CHECK(std::move(result).unwrap_err() == VerificationError::DanglingType);
  }
}

TEST_CASE("Verification accepts a value parameter annotation") {
  Fixture f;
  TypeNode annotation;
  annotation.kind = TypeKind::Primitive;
  annotation.span = test_span();
  annotation.payload.set(TypePrimitive{.primitive = PrimitiveKind::Usize});
  const TypeIdx annotation_idx = f.ast.types.emplace_back(annotation);
  const GenericParam param{.name = {.name = "N", .span = test_span()},
                           .type = annotation_idx};
  ItemNode item;
  item.kind = ItemKind::Fn;
  item.span = test_span();
  item.is_pub = false;
  item.payload.set(ItemFn{.name = {.name = "fill", .span = test_span()},
                          .generic = std::span<const GenericParam>(&param, 1),
                          .params = {},
                          .return_type = TypeIdx::invalid(),
                          .body = BlockIdx::invalid()});
  f.ast.items.emplace_back(item);
  CHECK(verify_file(f.ast).is_ok());
}

TEST_CASE("Verification rejects a dangling value parameter annotation") {
  Fixture f;
  const GenericParam param{.name = {.name = "N", .span = test_span()},
                           .type = TypeIdx(6)};
  ItemNode item;
  item.kind = ItemKind::Fn;
  item.span = test_span();
  item.is_pub = false;
  item.payload.set(ItemFn{.name = {.name = "fill", .span = test_span()},
                          .generic = std::span<const GenericParam>(&param, 1),
                          .params = {},
                          .return_type = TypeIdx::invalid(),
                          .body = BlockIdx::invalid()});
  f.ast.items.emplace_back(item);
  base::Result<void, VerificationError> result = verify_file(f.ast);
  CHECK(result.is_err());
  if (result.is_err()) {
    CHECK(std::move(result).unwrap_err() == VerificationError::DanglingItem);
  }
}

}  // namespace ast
