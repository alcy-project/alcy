// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/ast.h"

#include <span>
#include <vector>

#include "diag/span.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "source/source.h"

namespace ast {

namespace {

struct Fixture {
  AstArena ast;
};

diag::Span test_span(u32 offset = 0, u32 length = 1) {
  return diag::Span{source::UNKNOWN_FILE, offset, length};
}

}  // namespace

TEST_CASE("Arena AST nodes address through indices") {
  Fixture f;
  const LiteralIdx lit = f.ast.literals.push_back(Literal{
      .kind = LiteralKind::Integer, .span = test_span(), .spelling = "42"});

  ExprNode expr;
  expr.kind = ExprKind::Literal;
  expr.span = test_span();
  expr.payload.set(ExprLiteral{
      .value = lit,
  });
  const ExprIdx expr_idx = f.ast.exprs.emplace_back(expr);

  const ExprNode& base = f.ast.exprs[expr_idx];
  CHECK(base.kind == ExprKind::Literal);
  CHECK(f.ast.literals[base.payload.get<ExprLiteral>().value].spelling == "42");
}

TEST_CASE("Arena lists copy scratch buffers") {
  Fixture f;
  const std::vector<ast::Ident> names = {
      ast::Ident{.name = "a", .span = test_span()},
      ast::Ident{.name = "b", .span = test_span(1)}};
  const std::span<const ast::Ident> copied =
      ast::copy_to_arena(f.ast.spans, names);
  CHECK(copied.size() == 2);
  CHECK(copied[0].name == "a");
  CHECK(copied[1].name == "b");

  const std::vector<ast::Ident> empty;
  const std::span<const ast::Ident> copied_empty =
      ast::copy_to_arena(f.ast.spans, empty);
  CHECK(copied_empty.empty());
}

}  // namespace ast
