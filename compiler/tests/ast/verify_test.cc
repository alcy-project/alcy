// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/verify.h"

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
  base::Result<void, VerifyError> result = verify_file(f.ast);
  CHECK(result.is_err());
  if (result.is_err()) {
    CHECK(std::move(result).unwrap_err() == VerifyError::DanglingExpr);
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
  base::Result<void, VerifyError> result = verify_file(f.ast);
  CHECK(result.is_err());
  if (result.is_err()) {
    CHECK(std::move(result).unwrap_err() == VerifyError::DanglingItem);
  }
}

TEST_CASE("Verification rejects a dangling type edge") {
  Fixture f;
  TypeNode node;
  node.kind = TypeKind::Array;
  node.span = test_span();
  node.payload.set(TypeArray{.element = TypeIdx(4), .count = 2});
  f.ast.types.emplace_back(node);
  base::Result<void, VerifyError> result = verify_file(f.ast);
  CHECK(result.is_err());
  if (result.is_err()) {
    CHECK(std::move(result).unwrap_err() == VerifyError::DanglingType);
  }
}

}  // namespace ast
