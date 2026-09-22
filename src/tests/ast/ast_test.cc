// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/ast.h"

#include <span>
#include <string_view>
#include <vector>

#include "diag/span.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "source/source.h"

namespace ast {

namespace {

struct Fixture {
  mem::Arena arena;

  Fixture() { arena.reserve(1u << 20); }
};

diag::Span test_span(u32 offset = 0, u32 length = 1) {
  return diag::Span{source::kUnknownFile, offset, length};
}

}  // namespace

TEST_CASE("Arena AST nodes downcast through their base") {
  Fixture f;
  Literal* lit = f.arena.create<Literal>();
  lit->kind = LiteralKind::Integer;
  lit->span = test_span();
  lit->spelling = "42";

  LiteralExpr* expr = f.arena.create<LiteralExpr>();
  expr->kind = ExprKind::Literal;
  expr->span = test_span();
  expr->value = lit;

  const Expr* base = expr;
  CHECK(base->kind == ExprKind::Literal);
  const LiteralExpr* back = static_cast<const LiteralExpr*>(base);
  CHECK(back->value->spelling == "42");
}

TEST_CASE("Arena lists copy scratch buffers") {
  Fixture f;
  const std::vector<ast::Ident> names = {
      ast::Ident{.name = "a", .span = test_span()},
      ast::Ident{.name = "b", .span = test_span(1)}};
  const std::span<const ast::Ident> copied = ast::copy_to_arena(f.arena, names);
  CHECK(copied.size() == 2);
  CHECK(copied[0].name == "a");
  CHECK(copied[1].name == "b");

  const std::vector<ast::Ident> empty;
  const std::span<const ast::Ident> copied_empty =
      ast::copy_to_arena(f.arena, empty);
  CHECK(copied_empty.empty());
}

}  // namespace ast
