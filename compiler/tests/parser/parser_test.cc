// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parser/parser.h"

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "source/source.h"

namespace parser {

namespace {

struct Fixture {
  mem::Arena arena;
  ast::AstArena ast;
  diag::DiagBag bag{arena, i18n::Language::EnUs};

  Fixture() { arena.reserve(1u << 20); }
};

struct ParseResult {
  std::span<const ast::ItemIdx> items;
  bool ok;
};

ParseResult parse(std::string_view bytes, Fixture& f) {
  lexer::Lexer lexer(bytes, source::UNKNOWN_FILE, f.bag);
  std::vector<lexer::Token> tokens;
  lexer.tokenize(tokens);
  Parser parser(std::span<const lexer::Token>(tokens.data(), tokens.size()),
                bytes, source::UNKNOWN_FILE, f.ast, f.bag);
  base::Result<std::span<const ast::ItemIdx>, diag::Reported> parsed =
      parser.parse();
  if (parsed.is_err()) {
    return {{}, false};
  }
  auto items = std::move(parsed).unwrap();
  return {items, !f.bag.has_errors()};
}

const ast::ItemFn as_fn(const ast::ItemIdx item_idx, Fixture& f) {
  const ast::ItemNode& item = f.ast.items[item_idx];
  CHECK(item.kind == ast::ItemKind::Fn);
  return item.payload.get<ast::ItemFn>();
}

const ast::ExprBinary as_binary(const ast::ExprIdx expr_idx, Fixture& f) {
  const ast::ExprNode& expr = f.ast.exprs[expr_idx];
  CHECK(expr.kind == ast::ExprKind::Binary);
  return expr.payload.get<ast::ExprBinary>();
}

const ast::ExprRange as_range(const ast::ExprIdx expr_idx, Fixture& f) {
  const ast::ExprNode& expr = f.ast.exprs[expr_idx];
  CHECK(expr.kind == ast::ExprKind::Range);
  return expr.payload.get<ast::ExprRange>();
}

const ast::ExprReturn as_return(const ast::ExprIdx expr_idx, Fixture& f) {
  const ast::ExprNode& expr = f.ast.exprs[expr_idx];
  CHECK(expr.kind == ast::ExprKind::Return);
  return expr.payload.get<ast::ExprReturn>();
}

const ast::ExprClosure as_closure(const ast::ExprIdx expr_idx, Fixture& f) {
  const ast::ExprNode& expr = f.ast.exprs[expr_idx];
  CHECK(expr.kind == ast::ExprKind::Closure);
  return expr.payload.get<ast::ExprClosure>();
}

// The trailing expression of a one-expression function body.
ast::ExprIdx body_value(std::string_view bytes, Fixture& f) {
  const ParseResult result = parse(bytes, f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 1) {
    return ast::ExprIdx::invalid();
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::Block& body = f.ast.blocks[fn.body];
  if (!body.value.is_valid()) {
    return ast::ExprIdx::invalid();
  }
  return body.value;
}

const ast::ItemStruct as_struct(const ast::ItemIdx item_idx, Fixture& f) {
  const ast::ItemNode& item = f.ast.items[item_idx];
  CHECK(item.kind == ast::ItemKind::Struct);
  return item.payload.get<ast::ItemStruct>();
}

const ast::ItemEnum as_enum(const ast::ItemIdx item_idx, Fixture& f) {
  const ast::ItemNode& item = f.ast.items[item_idx];
  CHECK(item.kind == ast::ItemKind::Enum);
  return item.payload.get<ast::ItemEnum>();
}

const ast::ItemStatic as_static(const ast::ItemIdx item_idx, Fixture& f) {
  const ast::ItemNode& item = f.ast.items[item_idx];
  CHECK(item.kind == ast::ItemKind::Static);
  return item.payload.get<ast::ItemStatic>();
}

const ast::ItemConst as_const(const ast::ItemIdx item_idx, Fixture& f) {
  const ast::ItemNode& item = f.ast.items[item_idx];
  CHECK(item.kind == ast::ItemKind::Const);
  return item.payload.get<ast::ItemConst>();
}

const ast::ItemImpl as_impl(const ast::ItemIdx item_idx, Fixture& f) {
  const ast::ItemNode& item = f.ast.items[item_idx];
  CHECK(item.kind == ast::ItemKind::Impl);
  return item.payload.get<ast::ItemImpl>();
}

const ast::ItemSpec as_spec(const ast::ItemIdx item_idx, Fixture& f) {
  const ast::ItemNode& item = f.ast.items[item_idx];
  CHECK(item.kind == ast::ItemKind::Spec);
  return item.payload.get<ast::ItemSpec>();
}

const ast::ItemUse as_use(const ast::ItemIdx item_idx, Fixture& f) {
  const ast::ItemNode& item = f.ast.items[item_idx];
  CHECK(item.kind == ast::ItemKind::Use);
  return item.payload.get<ast::ItemUse>();
}

const ast::ExprTuple as_tuple(const ast::ExprIdx expr_idx, Fixture& f) {
  const ast::ExprNode& expr = f.ast.exprs[expr_idx];
  CHECK(expr.kind == ast::ExprKind::Tuple);
  return expr.payload.get<ast::ExprTuple>();
}

const ast::StmtDecl as_decl(const ast::StmtIdx stmt_idx, Fixture& f) {
  const ast::StmtNode& stmt = f.ast.stmts[stmt_idx];
  CHECK(stmt.kind == ast::StmtKind::Decl);
  return stmt.payload.get<ast::StmtDecl>();
}

const ast::StmtReassign as_reassign(const ast::StmtIdx stmt_idx, Fixture& f) {
  const ast::StmtNode& stmt = f.ast.stmts[stmt_idx];
  CHECK(stmt.kind == ast::StmtKind::Reassign);
  return stmt.payload.get<ast::StmtReassign>();
}

}  // namespace

TEST_CASE("Parser accepts empty files") {
  Fixture f;
  const ParseResult result = parse("", f);
  CHECK(result.ok);
  CHECK(result.items.empty());
}

TEST_CASE("Parser builds an empty main function") {
  Fixture f;
  const ParseResult result = parse("fn main() { }", f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  CHECK(fn.name.name == "main");
  CHECK(fn.params.empty());
  CHECK(!fn.return_type.is_valid());
  const ast::Block& block = f.ast.blocks[fn.body];
  CHECK(block.statements.empty());
  CHECK(!block.value.is_valid());
  CHECK(f.ast.items[result.items[0]].span.length == 13);
}

TEST_CASE("Parser builds functions with params and return types") {
  Fixture f;
  const ParseResult result =
      parse("fn add(a: i32, b: i32) -> i32 { ret a + b }", f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  CHECK(fn.params.size() == 2);
  CHECK(fn.return_type.is_valid());
  // Note: resolve_type requires analyzer context, skipping type check here
  CHECK(fn.body.is_valid());
  const ast::Block& block = f.ast.blocks[fn.body];
  CHECK(block.value.is_valid());
  const ast::ExprIdx ret_idx = block.value;
  const ast::ExprReturn& ret = as_return(ret_idx, f);
  const ast::ExprIdx add_idx = ret.value;
  const ast::ExprBinary& add = as_binary(add_idx, f);
  CHECK(add.op == ast::BinaryOp::Add);
}

TEST_CASE("Parser builds module items") {
  Fixture f;
  const ParseResult result = parse(
      "pub struct Point { x: i32, y: i32 }\n"
      "enum Choice { Yes, No }\n"
      "use package::other;\n"
      "static answer: i32 = 42\n"
      "const limit: i32 = 7\n"
      "impl Point { fn get(self: Self) -> i32 { ret 0 } }\n",
      f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  CHECK(result.items.size() == 6);
  if (result.items.size() != 6) {
    return;
  }
  const ast::ItemStruct& point = as_struct(result.items[0], f);
  CHECK(f.ast.items[result.items[0]].is_pub);
  CHECK(point.fields.size() == 2);
  const ast::ItemEnum& choice = as_enum(result.items[1], f);
  CHECK(choice.variants.size() == 2);
  const ast::ItemUse& use = as_use(result.items[2], f);
  CHECK(!use.has_alias);
  const ast::ItemStatic& answer = as_static(result.items[3], f);
  CHECK(answer.init.is_valid());
  const ast::ItemConst& limit = as_const(result.items[4], f);
  CHECK(limit.init.is_valid());
  const ast::ItemImpl& impl = as_impl(result.items[5], f);
  CHECK(impl.methods.size() == 1);
}

TEST_CASE("Parser builds public methods and mut self receivers") {
  Fixture f;
  const ParseResult result = parse(
      "struct S { x: i32 }\n"
      "impl S {\n"
      "  pub fn get(self: &Self) -> i32 { ret 0 }\n"
      "  pub fn set(mut self: &mut Self, v: i32) { }\n"
      "}\n",
      f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 2) {
    return;
  }
  const ast::ItemImpl& impl = as_impl(result.items[1], f);
  CHECK(impl.methods.size() == 2);
  if (impl.methods.size() != 2) {
    return;
  }
  CHECK(f.ast.items[impl.methods[0]].is_pub);
  CHECK(f.ast.items[impl.methods[1]].is_pub);
}

TEST_CASE("Parser builds generic enum and impl params") {
  Fixture f;
  const ParseResult result = parse(
      "enum Option<T> { Some(T), None }\n"
      "impl<T> Option<T> {\n"
      "  fn unwrap(self: Self) -> T { ret self }\n"
      "}\n",
      f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 2) {
    return;
  }
  const ast::ItemEnum option = as_enum(result.items[0], f);
  CHECK(option.params.size() == 1);
  if (option.params.size() != 1) {
    return;
  }
  CHECK(option.params[0].name == "T");
  CHECK(option.variants.size() == 2);
  const ast::ItemImpl impl = as_impl(result.items[1], f);
  CHECK(impl.params.size() == 1);
  if (impl.params.size() != 1) {
    return;
  }
  CHECK(impl.params[0].name == "T");
  CHECK(impl.methods.size() == 1);
}

TEST_CASE("Parser builds generic struct params") {
  Fixture f;
  const ParseResult result = parse("struct Pair<A, B> { a: A, b: B }\n", f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemStruct pair = as_struct(result.items[0], f);
  CHECK(pair.params.size() == 2);
  if (pair.params.size() != 2) {
    return;
  }
  CHECK(pair.params[0].name == "A");
  CHECK(pair.params[1].name == "B");
  CHECK(pair.fields.size() == 2);
}

TEST_CASE("Parser rejects duplicate type parameters") {
  Fixture f;
  const ParseResult result = parse("enum Pair<T, T> { Both(T, T) }\n", f);
  CHECK(!result.ok);
}

TEST_CASE("Parser rejects module declarations") {
  // Modules come from the manifest; `mod` is not an item.
  Fixture f;
  const ParseResult result = parse("mod inner;\nfn main() {}\n", f);
  CHECK(!result.ok);
}

TEST_CASE("Parser builds empty tuples") {
  Fixture f;
  const ParseResult result = parse("fn f() { _ := () }", f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::Block& block = f.ast.blocks[fn.body];
  CHECK(block.statements.size() == 1);
  const ast::StmtDecl& decl = as_decl(block.statements[0], f);
  CHECK(decl.init.is_valid());
  const ast::ExprIdx tuple_idx = decl.init;
  const ast::ExprTuple& tuple = as_tuple(tuple_idx, f);
  CHECK(tuple.elements.empty());
}

TEST_CASE("Parser separates declaration reassignment and comparison") {
  Fixture f;
  const ParseResult result = parse(
      "fn f() {\n"
      "  x := 1\n"
      "  mut y: f64 := 2.0\n"
      "  x = 3\n"
      "  x += 4\n"
      "  x == 5\n"
      "}\n",
      f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::Block& block = f.ast.blocks[fn.body];
  CHECK(block.statements.size() == 4);
  if (block.statements.size() != 4) {
    return;
  }
  const ast::StmtReassign& plain = as_reassign(block.statements[2], f);
  CHECK(!plain.compound);
  const ast::StmtReassign& compound = as_reassign(block.statements[3], f);
  CHECK(compound.compound);
  CHECK(compound.op == ast::BinaryOp::Add);
  CHECK(block.value.is_valid());
  const ast::ExprIdx cmp_idx = block.value;
  const ast::ExprBinary& cmp = as_binary(cmp_idx, f);
  CHECK(cmp.op == ast::BinaryOp::Eq);
}

TEST_CASE("Parser keeps control heads with pattern conditions") {
  Fixture f;
  const ParseResult result = parse(
      "fn f() {\n"
      "  if Some(v) := o {\n"
      "    w := v\n"
      "  }\n"
      "  while Some(n) := cur {\n"
      "    m := n\n"
      "  }\n"
      "}\n",
      f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::Block& block = f.ast.blocks[fn.body];
  CHECK(block.statements.size() == 1);
  if (block.statements.size() != 1) {
    return;
  }
  const ast::StmtIdx stmt_idx = block.statements[0];
  const ast::StmtNode& stmt = f.ast.stmts[stmt_idx];
  CHECK(stmt.kind == ast::StmtKind::Expr);
  const ast::StmtExpr& expr_stmt = stmt.payload.get<ast::StmtExpr>();
  CHECK(expr_stmt.value.is_valid());
  const ast::ExprIdx expr_idx = expr_stmt.value;
  const ast::ExprNode& expr = f.ast.exprs[expr_idx];
  CHECK(expr.kind == ast::ExprKind::If);
  // A trailing control expression is the block value, like any tail.
  CHECK(block.value.is_valid());
  const ast::ExprIdx while_idx = block.value;
  const ast::ExprNode& while_expr = f.ast.exprs[while_idx];
  CHECK(while_expr.kind == ast::ExprKind::While);
}

TEST_CASE("Parser respects operator precedence") {
  Fixture f;
  const ParseResult result = parse("fn f() { 1 + 2 * 3 }", f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::Block& block = f.ast.blocks[fn.body];
  CHECK(block.value.is_valid());
  const ast::ExprIdx add_idx = block.value;
  const ast::ExprBinary& add = as_binary(add_idx, f);
  CHECK(add.op == ast::BinaryOp::Add);
  const ast::ExprIdx mul_idx = add.rhs;
  const ast::ExprBinary& mul = as_binary(mul_idx, f);
  CHECK(mul.op == ast::BinaryOp::Mul);
}

TEST_CASE("Parser reads range expressions") {
  Fixture f;
  {
    const ParseResult result = parse("fn f() { 1..=3 }", f);
    CHECK(result.ok);
    if (!result.ok) {
      return;
    }
    const ast::Block& block = f.ast.blocks[as_fn(result.items[0], f).body];
    const ast::ExprRange range = as_range(block.value, f);
    CHECK(range.start.is_valid());
    CHECK(range.end.is_valid());
    CHECK(range.inclusive);
  }
  {
    const ParseResult result = parse("fn f() { 1..<3 }", f);
    CHECK(result.ok);
    if (!result.ok) {
      return;
    }
    const ast::Block& block = f.ast.blocks[as_fn(result.items[0], f).body];
    const ast::ExprRange range = as_range(block.value, f);
    CHECK(range.start.is_valid());
    CHECK(range.end.is_valid());
    CHECK(!range.inclusive);
  }
  {
    const ParseResult result = parse("fn f() { 1.. }", f);
    CHECK(result.ok);
    if (!result.ok) {
      return;
    }
    const ast::Block& block = f.ast.blocks[as_fn(result.items[0], f).body];
    const ast::ExprRange range = as_range(block.value, f);
    CHECK(range.start.is_valid());
    CHECK(!range.end.is_valid());
  }
  {
    const ParseResult result = parse("fn f() { ..<3 }", f);
    CHECK(result.ok);
    if (!result.ok) {
      return;
    }
    const ast::Block& block = f.ast.blocks[as_fn(result.items[0], f).body];
    const ast::ExprRange range = as_range(block.value, f);
    CHECK(!range.start.is_valid());
    CHECK(range.end.is_valid());
  }
  {
    const ParseResult result = parse("fn f() { .. }", f);
    CHECK(result.ok);
    if (!result.ok) {
      return;
    }
    const ast::Block& block = f.ast.blocks[as_fn(result.items[0], f).body];
    const ast::ExprRange range = as_range(block.value, f);
    CHECK(!range.start.is_valid());
    CHECK(!range.end.is_valid());
  }
  // Endpoints are `or` expressions: the range binds loosest.
  {
    const ParseResult result = parse("fn f() { 1 + 2..<3 * 4 }", f);
    CHECK(result.ok);
    if (!result.ok) {
      return;
    }
    const ast::Block& block = f.ast.blocks[as_fn(result.items[0], f).body];
    const ast::ExprRange range = as_range(block.value, f);
    CHECK(as_binary(range.start, f).op == ast::BinaryOp::Add);
    CHECK(as_binary(range.end, f).op == ast::BinaryOp::Mul);
  }
  // A newline closes an open-ended range like any complete value.
  {
    const ParseResult result = parse("fn f() {\n  x := 1..\n  ret x\n}", f);
    CHECK(result.ok);
    if (!result.ok) {
      return;
    }
    const ast::Block& block = f.ast.blocks[as_fn(result.items[0], f).body];
    CHECK(block.statements.size() == 1);
    CHECK(block.value.is_valid());
    CHECK(f.ast.exprs[block.value].kind == ast::ExprKind::Return);
  }
  CHECK(!f.bag.has_errors());
}

// A range that names an end says how it is bound, so only `..<` and
// `..=` may carry an endpoint; bare `..` is the unbounded spelling.
TEST_CASE("Parser requires a spelled range end") {
  for (const std::string_view source :
       {"fn f() { 1..3 }", "fn f() { ..3 }", "fn f() { a[1..2] }",
        "fn f() { 1..2.5 }", "fn f() { 1..= }", "fn f() { 1..< }",
        "fn f() { ..= }", "fn f() { ..< }"}) {
    Fixture f;
    const ParseResult result = parse(source, f);
    CHECK_MESSAGE(!result.ok, source);
    CHECK_MESSAGE(f.bag.has_errors(), source);
  }
  // The unbounded spellings keep bare `..`.
  for (const std::string_view source :
       {"fn f() { 1.. }", "fn f() { .. }", "fn f() { a[..] }",
        "fn f() { a[1..] }"}) {
    Fixture f;
    const ParseResult result = parse(source, f);
    CHECK_MESSAGE(result.ok, source);
    CHECK_MESSAGE(!f.bag.has_errors(), source);
  }
}

// In a `for` head the `{` opens the body, so an open range reads as
// the head; anywhere else a block still opens a range end.
TEST_CASE("Parser reads an open range as a for head") {
  Fixture f;
  const ParseResult result = parse("fn f() { for i in 0.. {}\n 1 }", f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::Block& body = f.ast.blocks[as_fn(result.items[0], f).body];
  if (body.statements.size() != 1) {
    return;
  }
  const ast::StmtNode& stmt = f.ast.stmts[body.statements[0]];
  if (stmt.kind != ast::StmtKind::Expr) {
    return;
  }
  const ast::ExprNode& block_expr =
      f.ast.exprs[stmt.payload.get<ast::StmtExpr>().value];
  if (block_expr.kind != ast::ExprKind::Block) {
    return;
  }
  const ast::Block& block =
      f.ast.blocks[block_expr.payload.get<ast::ExprBlock>().block];
  if (block.statements.size() != 1) {
    return;
  }
  const ast::StmtNode& decl = f.ast.stmts[block.statements[0]];
  if (decl.kind != ast::StmtKind::Decl) {
    return;
  }
  const ast::ExprNode& init =
      f.ast.exprs[decl.payload.get<ast::StmtDecl>().init];
  CHECK(init.kind == ast::ExprKind::MethodCall);
  if (init.kind != ast::ExprKind::MethodCall) {
    return;
  }
  const ast::ExprNode& head =
      f.ast.exprs[init.payload.get<ast::ExprMethodCall>().receiver];
  CHECK(head.kind == ast::ExprKind::Range);
  if (head.kind == ast::ExprKind::Range) {
    const ast::ExprRange range = head.payload.get<ast::ExprRange>();
    CHECK(range.start.is_valid());
    CHECK(!range.end.is_valid());
  }
}

TEST_CASE("Parser treats power as right associative") {
  Fixture f;
  const ParseResult result = parse("fn f() { 2 ** 3 ** 2 }", f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::Block& block = f.ast.blocks[fn.body];
  CHECK(block.value.is_valid());
  const ast::ExprIdx outer_idx = block.value;
  const ast::ExprBinary& outer = as_binary(outer_idx, f);
  CHECK(outer.op == ast::BinaryOp::Pow);
  const ast::ExprNode& inner = f.ast.exprs[outer.rhs];
  CHECK(inner.kind == ast::ExprKind::Binary);
}

TEST_CASE("Parser binds unary minus tighter than as-casts") {
  Fixture f;
  const ParseResult result = parse("fn f() { -x as i32 }", f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::Block& block = f.ast.blocks[fn.body];
  CHECK(block.value.is_valid());
  const ast::ExprIdx cast_idx = block.value;
  const ast::ExprNode& cast_expr = f.ast.exprs[cast_idx];
  CHECK(cast_expr.kind == ast::ExprKind::Cast);
  const ast::ExprCast& cast = cast_expr.payload.get<ast::ExprCast>();
  CHECK(cast.inner.is_valid());
  const ast::ExprIdx inner_idx = cast.inner;
  const ast::ExprNode& inner = f.ast.exprs[inner_idx];
  CHECK(inner.kind == ast::ExprKind::Unary);
}

TEST_CASE("Parser rejects chained comparisons") {
  Fixture f;
  const ParseResult result = parse("fn f() { a < b < c }", f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser reads borrow expressions") {
  {
    Fixture f;
    const ParseResult result = parse("fn f() { &x }", f);
    CHECK(result.ok);
    if (!result.ok) {
      return;
    }
    const ast::ItemFn& fn = as_fn(result.items[0], f);
    const ast::Block& block = f.ast.blocks[fn.body];
    CHECK(block.value.is_valid());
    const ast::ExprIdx borrow_idx = block.value;
    const ast::ExprNode& borrow_expr = f.ast.exprs[borrow_idx];
    CHECK(borrow_expr.kind == ast::ExprKind::Borrow);
    const ast::ExprBorrow& borrow = borrow_expr.payload.get<ast::ExprBorrow>();
    CHECK(!borrow.is_mut);
    CHECK(borrow.inner.is_valid());
    const ast::ExprIdx inner_idx = borrow.inner;
    const ast::ExprNode& inner = f.ast.exprs[inner_idx];
    CHECK(inner.kind == ast::ExprKind::Path);
  }
  {
    Fixture f;
    const ParseResult result = parse("fn f() { &mut x }", f);
    CHECK(result.ok);
    if (!result.ok) {
      return;
    }
    const ast::ItemFn& fn = as_fn(result.items[0], f);
    const ast::Block& block = f.ast.blocks[fn.body];
    CHECK(block.value.is_valid());
    const ast::ExprIdx borrow_idx = block.value;
    const ast::ExprNode& borrow_expr = f.ast.exprs[borrow_idx];
    CHECK(borrow_expr.kind == ast::ExprKind::Borrow);
    const ast::ExprBorrow& borrow = borrow_expr.payload.get<ast::ExprBorrow>();
    CHECK(borrow.is_mut);
  }
}

TEST_CASE("Parser builds control flow") {
  Fixture f;
  const ParseResult result = parse(
      "fn f(x: i32) -> i32 {\n"
      "  if x == 1 {\n"
      "    ret 10\n"
      "  } else {\n"
      "    ret match x {\n"
      "      0 => 100,\n"
      "      _ => 200\n"
      "    }\n"
      "  }\n"
      "}\n",
      f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::Block& block = f.ast.blocks[fn.body];
  CHECK(block.value.is_valid());
  const ast::ExprIdx if_idx = block.value;
  const ast::ExprNode& if_expr = f.ast.exprs[if_idx];
  CHECK(if_expr.kind == ast::ExprKind::If);
}

TEST_CASE("Parser reads nested generic type arguments") {
  Fixture f;
  const ParseResult result =
      parse("fn f(x: Result<Option<i32>, bool>) -> i32 { ret 0 }", f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  CHECK(fn.params.size() == 1);
  if (fn.params.empty()) {
    return;
  }
  const ast::TypeIdx param_type = fn.params[0].type;
  // Type resolution requires analyzer context - skip deep check
  CHECK(param_type.is_valid());
}

TEST_CASE("Parser reports errors without stopping at the first") {
  Fixture f;
  const ParseResult result = parse(
      "fn broken( { }\n"
      "fn fine() { }\n",
      f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
  CHECK(result.items.size() == 1);
}

TEST_CASE("Parser rejects reserved words with guidance") {
  Fixture f;
  const ParseResult result = parse("fn f() { where x := y }", f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser accepts an unsafe function") {
  Fixture f;
  const ParseResult result = parse("unsafe fn f() { }", f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (result.items.size() != 1) {
    return;
  }
  const ast::ItemFn fn = as_fn(result.items[0], f);
  CHECK(fn.is_unsafe);
}

TEST_CASE("Parser accepts an unsafe block") {
  Fixture f;
  const ParseResult result = parse("fn f() { unsafe { } }", f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (result.items.size() != 1) {
    return;
  }
  const ast::ItemFn fn = as_fn(result.items[0], f);
  const ast::Block& body = f.ast.blocks[fn.body];
  CHECK(body.value.is_valid());
  if (!body.value.is_valid()) {
    return;
  }
  const ast::ExprNode& block_expr = f.ast.exprs[body.value];
  CHECK(block_expr.kind == ast::ExprKind::Block);
  if (block_expr.kind != ast::ExprKind::Block) {
    return;
  }
  const ast::ExprBlock& block = block_expr.payload.get<ast::ExprBlock>();
  CHECK(block.is_unsafe);
  CHECK(!block.is_comp);
}

TEST_CASE("Parser rejects unsafe outside functions and blocks") {
  Fixture f;
  const ParseResult result = parse("unsafe struct S { }", f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser keeps a block's brace on its header line") {
  Fixture f;
  const ParseResult result = parse(
      "fn f() {\n"
      "  unsafe\n"
      "  { }\n"
      "}\n",
      f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser reads a header whose closing bracket ends the line") {
  Fixture f;
  const ParseResult result = parse(
      "struct S<T> { a: T }\n"
      "fn f() -> S<i32>\n"
      "{\n"
      "  ret S { a: 1 }\n"
      "}\n",
      f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser reads a headerless block on its own line") {
  Fixture f;
  const ParseResult result = parse(
      "fn f() -> i32 {\n"
      "  x :=\n"
      "  { 1 }\n"
      "  ret x\n"
      "}\n",
      f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Parser reads a closure body on the line after the arrow") {
  Fixture f;
  const ParseResult result = parse(
      "fn main() {\n"
      "  g := (x: i32) ->\n"
      "  { x }\n"
      "  _ := g(1)\n"
      "}\n",
      f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Parentheses lift the struct-literal ban") {
  Fixture f;
  const ParseResult result = parse(
      "struct Foo { x: i32 }\n"
      "fn f() { if (Foo { x: 1 }).x > 0 {} }",
      f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Parser desugars for into a loop over into_iter") {
  Fixture f;
  const ParseResult result = parse("fn f() { for x in xs {}\n 1 }", f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (result.items.size() != 1) {
    return;
  }
  const ast::ItemFn fn = as_fn(result.items[0], f);
  const ast::Block& body = f.ast.blocks[fn.body];
  CHECK(body.statements.size() == 1);
  if (body.statements.size() != 1) {
    return;
  }
  const ast::StmtNode& stmt = f.ast.stmts[body.statements[0]];
  CHECK(stmt.kind == ast::StmtKind::Expr);
  if (stmt.kind != ast::StmtKind::Expr) {
    return;
  }
  // The statement is the desugared block: one cursor declaration whose
  // initializer calls `into_iter`, and a loop as its value.
  const ast::ExprNode& block_expr =
      f.ast.exprs[stmt.payload.get<ast::StmtExpr>().value];
  CHECK(block_expr.kind == ast::ExprKind::Block);
  if (block_expr.kind != ast::ExprKind::Block) {
    return;
  }
  const ast::Block& block =
      f.ast.blocks[block_expr.payload.get<ast::ExprBlock>().block];
  CHECK(block.statements.size() == 1);
  CHECK(block.value.is_valid());
  if (block.statements.size() != 1 || !block.value.is_valid()) {
    return;
  }
  const ast::StmtNode& decl = f.ast.stmts[block.statements[0]];
  CHECK(decl.kind == ast::StmtKind::Decl);
  if (decl.kind != ast::StmtKind::Decl) {
    return;
  }
  const ast::ExprNode& init =
      f.ast.exprs[decl.payload.get<ast::StmtDecl>().init];
  CHECK(init.kind == ast::ExprKind::MethodCall);
  if (init.kind == ast::ExprKind::MethodCall) {
    CHECK(init.payload.get<ast::ExprMethodCall>().name.name == "into_iter");
  }
  const ast::ExprNode& loop = f.ast.exprs[block.value];
  CHECK(loop.kind == ast::ExprKind::Loop);
  if (loop.kind != ast::ExprKind::Loop) {
    return;
  }
  const ast::Block& loop_body =
      f.ast.blocks[loop.payload.get<ast::ExprLoop>().body];
  CHECK(loop_body.value.is_valid());
  if (!loop_body.value.is_valid()) {
    return;
  }
  const ast::ExprNode& match = f.ast.exprs[loop_body.value];
  CHECK(match.kind == ast::ExprKind::Match);
  if (match.kind != ast::ExprKind::Match) {
    return;
  }
  const ast::ExprMatch arms = match.payload.get<ast::ExprMatch>();
  CHECK(arms.arms.size() == 2);
  if (arms.arms.size() != 2) {
    return;
  }
  // The `Some` arm carries the `for` marker, so a refutable item
  // pattern is a check-time error; the second arm breaks.
  CHECK(f.ast.patterns[arms.arms[0].pattern].for_pattern);
  CHECK(!f.ast.patterns[arms.arms[1].pattern].for_pattern);
  // The scrutinee is the spec-only `next` call; the second arm breaks.
  const ast::ExprNode& scrutinee = f.ast.exprs[arms.scrutinee];
  CHECK(scrutinee.kind == ast::ExprKind::MethodCall);
  if (scrutinee.kind == ast::ExprKind::MethodCall) {
    const ast::ExprMethodCall& call =
        scrutinee.payload.get<ast::ExprMethodCall>();
    CHECK(call.name.name == "next");
    CHECK(call.spec_only);
  }
  CHECK(f.ast.exprs[arms.arms[1].body].kind == ast::ExprKind::Break);
}

TEST_CASE("Parser rejects struct field assignment syntax") {
  Fixture f;
  const ParseResult result = parse("fn f() { Foo { x = 1 } }", f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser accepts comp parameters") {
  Fixture f;
  const ParseResult result =
      parse("fn repeat(comp n: i32, x: i32) -> i32 { ret x }", f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  CHECK(fn.params.size() == 2);
  CHECK(fn.params[0].is_comp);
  CHECK(!fn.params[1].is_comp);
}

TEST_CASE("Parser accepts comp declarations") {
  Fixture f;
  const ParseResult result = parse("fn f() { comp n := 3\n _ := n }", f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::Block& block = f.ast.blocks[fn.body];
  CHECK(block.statements.size() == 2);
  const ast::StmtNode& stmt = f.ast.stmts[block.statements[0]];
  CHECK(stmt.kind == ast::StmtKind::Decl);
  CHECK(stmt.payload.get<ast::StmtDecl>().is_comp);
}

TEST_CASE("Parser accepts comp blocks") {
  Fixture f;
  const ParseResult result = parse("fn f() -> i32 { ret comp { 1 + 2 } }", f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::Block& block = f.ast.blocks[fn.body];
  const ast::ExprReturn& ret = as_return(block.value, f);
  const ast::ExprNode& inner = f.ast.exprs[ret.value];
  CHECK(inner.kind == ast::ExprKind::Block);
  CHECK(inner.payload.get<ast::ExprBlock>().is_comp);
}

TEST_CASE("Parser rejects misplaced comp with guidance") {
  Fixture f;
  const ParseResult result = parse("fn f() { _ := comp + 1 }", f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser reads array types and literals") {
  Fixture f;
  const ParseResult result =
      parse("fn f(a: [u8; 4]) -> [i32; 2] { ret [1i32, 2i32] }", f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  CHECK(fn.params.size() == 1);
  const ast::TypeNode& param_type = f.ast.types[fn.params[0].type];
  CHECK(param_type.kind == ast::TypeKind::Array);
  CHECK(param_type.payload.get<ast::TypeArray>().count == 4);
  CHECK(fn.return_type.is_valid());
  const ast::Block& block = f.ast.blocks[fn.body];
  const ast::ExprNode& ret = f.ast.exprs[block.value];
  CHECK(ret.kind == ast::ExprKind::Return);
  const ast::ExprNode& value =
      f.ast.exprs[ret.payload.get<ast::ExprReturn>().value];
  CHECK(value.kind == ast::ExprKind::Array);
  CHECK(value.payload.get<ast::ExprArray>().elements.size() == 2);
}

TEST_CASE("Parser reads array repeats") {
  Fixture f;
  const ParseResult result = parse("fn f() { _ := [0u8; 4] }", f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::Block& block = f.ast.blocks[fn.body];
  const ast::StmtNode& stmt = f.ast.stmts[block.statements[0]];
  const ast::ExprNode& init =
      f.ast.exprs[stmt.payload.get<ast::StmtDecl>().init];
  CHECK(init.kind == ast::ExprKind::Array);
  const ast::ExprArray& array = init.payload.get<ast::ExprArray>();
  CHECK(array.repeat.is_valid());
  CHECK(array.count == 4);
}

TEST_CASE("Parser rejects empty array literals") {
  Fixture f;
  const ParseResult result = parse("fn f() { _ := [] }", f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser accepts intrinsic declarations") {
  Fixture f;
  const ParseResult result =
      parse("intrinsic fn memcopy(dst: &mut u8, src: &u8, n: usize);", f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemNode& item = f.ast.items[result.items[0]];
  CHECK(item.kind == ast::ItemKind::Intrinsic);
  const ast::ItemIntrinsic& intrinsic = item.payload.get<ast::ItemIntrinsic>();
  CHECK(intrinsic.name.name == "memcopy");
  CHECK(intrinsic.params.size() == 3);
  CHECK(!intrinsic.return_type.is_valid());
}

TEST_CASE("Parser rejects intrinsic declarations with bodies") {
  Fixture f;
  const ParseResult result = parse("intrinsic fn memcopy(dst: &mut u8) { }", f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser rejects intrinsic methods") {
  Fixture f;
  const ParseResult result =
      parse("struct S { x: i32 }\nimpl S { intrinsic fn f(); }", f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser keeps a negative literal pattern's sign off the spelling") {
  Fixture f;
  const ParseResult result =
      parse("fn f(n: i32) { match n { -1 => {}, _ => {} } }", f);
  CHECK(result.ok);
  if (!result.ok) {
    return;
  }
  // The magnitude parses the same either way, so the sign travels as a
  // flag rather than as text a number parser would have to strip.
  bool found = false;
  for (const ast::Literal& lit : f.ast.literals) {
    if (lit.is_negative) {
      found = true;
      CHECK(lit.spelling == "1");
      CHECK(lit.span.length == 1);
    }
  }
  CHECK(found);
  CHECK(!f.bag.has_errors());
}

TEST_CASE("Parser rejects an array length that overflows") {
  Fixture f;
  // Wrapping would silently turn this into a length of zero.
  const ParseResult result =
      parse("fn f() -> [u8; 18446744073709551616] { [0u8; 0] }", f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser builds spec declarations and impl blocks") {
  Fixture f;
  const ParseResult result = parse(
      "spec Iterator<T> {\n"
      "  fn next(mut self: &mut Self) -> Option<T>;\n"
      "  fn done(self: &Self) -> bool;\n"
      "}\n"
      "struct Counter { n: i32 }\n"
      "impl Iterator<i32> for Counter {\n"
      "  fn next(mut self: &mut Self) -> Option<i32> { ret Option::None }\n"
      "}\n",
      f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 3) {
    return;
  }
  const ast::ItemSpec spec = as_spec(result.items[0], f);
  CHECK(spec.name.name == "Iterator");
  CHECK(spec.params.size() == 1);
  CHECK(spec.methods.size() == 2);
  if (spec.methods.size() != 2) {
    return;
  }
  CHECK(spec.methods[0].name.name == "next");
  CHECK(spec.methods[0].return_type.is_valid());
  CHECK(spec.methods[1].name.name == "done");
  const ast::ItemImpl impl = as_impl(result.items[2], f);
  CHECK(impl.spec.is_valid());
  CHECK(impl.methods.size() == 1);
}

TEST_CASE("Parser keeps inherent impl blocks spec-free") {
  Fixture f;
  const ParseResult result = parse(
      "struct S { x: i32 }\n"
      "impl S {\n"
      "  fn get(self: &Self) -> i32 { ret 0 }\n"
      "}\n",
      f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 2) {
    return;
  }
  const ast::ItemImpl impl = as_impl(result.items[1], f);
  CHECK(!impl.spec.is_valid());
}

TEST_CASE("Parser rejects spec method bodies") {
  Fixture f;
  const ParseResult result = parse(
      "spec S {\n"
      "  fn f(self: &Self) -> i32 { ret 0 }\n"
      "}\n",
      f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser reads a super-spec path") {
  Fixture f;
  const ParseResult result = parse(
      "spec Eq: PartialEq {\n"
      "}\n"
      "spec PartialEq {\n"
      "  fn eq(self: &Self, other: &Self) -> bool;\n"
      "}\n",
      f);
  CHECK(result.ok);
  if (!result.ok || result.items.size() != 2) {
    return;
  }
  const ast::ItemSpec eq = as_spec(result.items[0], f);
  CHECK(eq.super.is_valid());
  if (!eq.super.is_valid()) {
    return;
  }
  const ast::TypeNode& super = f.ast.types[eq.super];
  CHECK(super.kind == ast::TypeKind::Path);
  const ast::TypePath& path = super.payload.get<ast::TypePath>();
  const std::span<const ast::Ident> segments = f.ast.paths[path.path].segments;
  CHECK(segments.size() == 1);
  if (segments.size() == 1) {
    CHECK(segments[0].name == "PartialEq");
  }
  const ast::ItemSpec plain = as_spec(result.items[1], f);
  CHECK(!plain.super.is_valid());
}

TEST_CASE("Parser rejects a super-spec without a name") {
  Fixture f;
  const ParseResult result = parse("spec Eq: {\n}\n", f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser reads a closure with captures and typed params") {
  Fixture f;
  const ast::ExprIdx value =
      body_value("fn f() { [t] (a: i32, mut b, _) -> { a } }", f);
  CHECK(value.is_valid());
  if (!value.is_valid()) {
    return;
  }
  const ast::ExprClosure& closure = as_closure(value, f);
  CHECK(closure.captures.size() == 1);
  if (closure.captures.size() == 1) {
    CHECK(closure.captures[0].name.name == "t");
    CHECK(closure.captures[0].mode == ast::CaptureMode::Move);
  }
  CHECK(closure.params.size() == 3);
  if (closure.params.size() != 3) {
    return;
  }
  CHECK(closure.params[0].name.name == "a");
  CHECK(!closure.params[0].is_mut);
  CHECK(!closure.params[0].is_wildcard);
  CHECK(closure.params[0].type.is_valid());
  CHECK(closure.params[1].name.name == "b");
  CHECK(closure.params[1].is_mut);
  CHECK(!closure.params[1].type.is_valid());
  CHECK(closure.params[2].is_wildcard);
  CHECK(f.ast.exprs[closure.body].kind == ast::ExprKind::Block);
}

TEST_CASE("Parser reads capture modes") {
  Fixture f;
  const ast::ExprIdx value =
      body_value("fn f() { [a, &b, &mut c, &self] () -> 0 }", f);
  CHECK(value.is_valid());
  if (!value.is_valid()) {
    return;
  }
  const ast::ExprClosure& closure = as_closure(value, f);
  CHECK(closure.captures.size() == 4);
  if (closure.captures.size() != 4) {
    return;
  }
  CHECK(closure.captures[0].name.name == "a");
  CHECK(closure.captures[0].mode == ast::CaptureMode::Move);
  CHECK(closure.captures[1].name.name == "b");
  CHECK(closure.captures[1].mode == ast::CaptureMode::Shared);
  CHECK(closure.captures[2].name.name == "c");
  CHECK(closure.captures[2].mode == ast::CaptureMode::Mut);
  CHECK(closure.captures[3].name.name == "self");
  CHECK(closure.captures[3].mode == ast::CaptureMode::Shared);
}

// `self` is a keyword and a local like any other, so a capture list may
// name the receiver.
TEST_CASE("Parser takes self in a capture list") {
  Fixture f;
  const ast::ExprIdx value =
      body_value("fn f() -> i32 {\n  [self] () -> 0\n}\n", f);
  CHECK(value.is_valid());
  if (!value.is_valid()) {
    return;
  }
  const ast::ExprClosure& closure = as_closure(value, f);
  CHECK(closure.captures.size() == 1);
  if (closure.captures.size() == 1) {
    CHECK(closure.captures[0].name.name == "self");
  }
}

// The cursor advances past doc comments; one after a closure's body
// must not disturb the trailing value.
TEST_CASE("Parser reads a closure with a trailing doc comment") {
  Fixture f;
  const ast::ExprIdx value = body_value(
      "fn f() -> i32 {\n"
      "  [t] (a: i32) -> a /// trailing note\n"
      "}\n",
      f);
  CHECK(value.is_valid());
  if (!value.is_valid()) {
    return;
  }
  const ast::ExprClosure& closure = as_closure(value, f);
  CHECK(closure.captures.size() == 1);
  CHECK(closure.params.size() == 1);
}

TEST_CASE("Parser reads a bare closure body as one expression") {
  Fixture f;
  const ast::ExprIdx value = body_value("fn f() { (a) -> a + 1 }", f);
  CHECK(value.is_valid());
  if (!value.is_valid()) {
    return;
  }
  const ast::ExprClosure& closure = as_closure(value, f);
  CHECK(closure.captures.empty());
  CHECK(closure.params.size() == 1);
  const ast::ExprNode& body = f.ast.exprs[closure.body];
  CHECK(body.kind == ast::ExprKind::Binary);
}

TEST_CASE("Parser reads empty captures and empty params") {
  Fixture f;
  const ast::ExprIdx value = body_value("fn f() { [] () -> 42 }", f);
  CHECK(value.is_valid());
  if (!value.is_valid()) {
    return;
  }
  const ast::ExprClosure& closure = as_closure(value, f);
  CHECK(closure.captures.empty());
  CHECK(closure.params.empty());
}

TEST_CASE("Parser reads function types without disturbing parens") {
  Fixture f;
  const ParseResult result = parse(
      "fn apply(f: (i32) -> i32, g: (i32, i32) -> i32, h: &(i32) -> i32) {}",
      f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  CHECK(fn.params.size() == 3);
  if (fn.params.size() != 3) {
    return;
  }
  const ast::TypeNode& one = f.ast.types[fn.params[0].type];
  CHECK(one.kind == ast::TypeKind::Func);
  if (one.kind == ast::TypeKind::Func) {
    const ast::TypeFunc& func = one.payload.get<ast::TypeFunc>();
    CHECK(func.params.size() == 1);
  }
  const ast::TypeNode& two = f.ast.types[fn.params[1].type];
  CHECK(two.kind == ast::TypeKind::Func);
  if (two.kind == ast::TypeKind::Func) {
    CHECK(two.payload.get<ast::TypeFunc>().params.size() == 2);
  }
  const ast::TypeNode& behind_ref = f.ast.types[fn.params[2].type];
  CHECK(behind_ref.kind == ast::TypeKind::Ref);
  if (behind_ref.kind == ast::TypeKind::Ref) {
    const ast::TypeNode& inner =
        f.ast.types[behind_ref.payload.get<ast::TypeRef>().inner];
    CHECK(inner.kind == ast::TypeKind::Func);
  }
}

TEST_CASE("Parser reads raw pointer types") {
  Fixture f;
  const ParseResult result = parse("fn f(p: *i32, q: *mut u8, r: **i32) {}", f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  CHECK(fn.params.size() == 3);
  if (fn.params.size() != 3) {
    return;
  }
  const ast::TypeNode& shared = f.ast.types[fn.params[0].type];
  CHECK(shared.kind == ast::TypeKind::RawPtr);
  if (shared.kind == ast::TypeKind::RawPtr) {
    const ast::TypeRawPtr& ptr = shared.payload.get<ast::TypeRawPtr>();
    CHECK(!ptr.is_mut);
    CHECK(f.ast.types[ptr.inner].kind == ast::TypeKind::Primitive);
  }
  const ast::TypeNode& exclusive = f.ast.types[fn.params[1].type];
  CHECK(exclusive.kind == ast::TypeKind::RawPtr);
  if (exclusive.kind == ast::TypeKind::RawPtr) {
    CHECK(exclusive.payload.get<ast::TypeRawPtr>().is_mut);
  }
  const ast::TypeNode& nested = f.ast.types[fn.params[2].type];
  CHECK(nested.kind == ast::TypeKind::RawPtr);
  if (nested.kind == ast::TypeKind::RawPtr) {
    const ast::TypeNode& inner =
        f.ast.types[nested.payload.get<ast::TypeRawPtr>().inner];
    CHECK(inner.kind == ast::TypeKind::RawPtr);
  }
}

TEST_CASE("Parser reads an extern block") {
  Fixture f;
  const ParseResult result = parse(
      "extern \"C\" {\n"
      "  fn abs(x: i32) -> i32;\n"
      "  fn exit(code: i32);\n"
      "}\n",
      f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemNode& item = f.ast.items[result.items[0]];
  CHECK(item.kind == ast::ItemKind::Extern);
  if (item.kind != ast::ItemKind::Extern) {
    return;
  }
  const ast::ItemExtern& block = item.payload.get<ast::ItemExtern>();
  CHECK(block.convention == "C");
  CHECK(block.fns.size() == 2);
  if (block.fns.size() != 2) {
    return;
  }
  CHECK(block.fns[0].name.name == "abs");
  CHECK(block.fns[0].params.size() == 1);
  CHECK(block.fns[0].return_type.is_valid());
  CHECK(block.fns[1].name.name == "exit");
  CHECK(!block.fns[1].return_type.is_valid());
}

TEST_CASE("Parser rejects an unknown calling convention") {
  Fixture f;
  const ParseResult result = parse(
      "extern \"stdcall\" {\n"
      "  fn abs(x: i32) -> i32;\n"
      "}\n",
      f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser keeps a parenthesized type a type") {
  Fixture f;
  const ParseResult result = parse("fn f(x: (i32)) {}", f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::TypeNode& type = f.ast.types[fn.params[0].type];
  CHECK(type.kind == ast::TypeKind::Primitive);
}

// The comma is what makes the parens a type of their own, so the
// one-element tuple a value can spell (`(1,)`) is spellable in a
// signature too.
TEST_CASE("Parser keeps a one-element tuple type a tuple") {
  Fixture f;
  const ParseResult result = parse("fn f(x: (i32,)) {}", f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (!result.ok || result.items.size() != 1) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[0], f);
  const ast::TypeNode& type = f.ast.types[fn.params[0].type];
  CHECK(type.kind == ast::TypeKind::Tuple);
  if (type.kind == ast::TypeKind::Tuple) {
    CHECK(type.payload.get<ast::TypeTuple>().elements.size() == 1);
  }
}

TEST_CASE("Parser keeps parens arrays unit and tuples") {
  Fixture f;
  const ast::ExprIdx paren = body_value("fn f() { (1) + 2 }", f);
  CHECK(paren.is_valid());
  if (paren.is_valid()) {
    CHECK(f.ast.exprs[paren].kind == ast::ExprKind::Binary);
  }
  Fixture g;
  const ast::ExprIdx array = body_value("fn f() { [1, 2] }", g);
  CHECK(array.is_valid());
  if (array.is_valid()) {
    CHECK(g.ast.exprs[array].kind == ast::ExprKind::Array);
  }
  Fixture h;
  const ast::ExprIdx unit = body_value("fn f() { () }", h);
  CHECK(unit.is_valid());
  if (unit.is_valid()) {
    CHECK(h.ast.exprs[unit].kind == ast::ExprKind::Tuple);
  }
  Fixture i;
  const ast::ExprIdx tuple = body_value("fn f() { (1, 2) }", i);
  CHECK(tuple.is_valid());
  if (tuple.is_valid()) {
    CHECK(i.ast.exprs[tuple].kind == ast::ExprKind::Tuple);
  }
}

TEST_CASE("Parser reads a closure as a call argument") {
  Fixture f;
  const ast::ExprIdx value = body_value("fn f() { g((a) -> a, b) }", f);
  CHECK(value.is_valid());
  if (!value.is_valid()) {
    return;
  }
  const ast::ExprNode& call = f.ast.exprs[value];
  CHECK(call.kind == ast::ExprKind::Call);
  if (call.kind != ast::ExprKind::Call) {
    return;
  }
  const auto args = call.payload.get<ast::ExprCall>().args;
  CHECK(args.size() == 2);
  if (args.size() != 2) {
    return;
  }
  CHECK(f.ast.exprs[args[0]].kind == ast::ExprKind::Closure);
  CHECK(f.ast.exprs[args[1]].kind == ast::ExprKind::Path);
}

TEST_CASE("Parser reads a struct literal as a closure body") {
  Fixture f;
  const ParseResult result =
      parse("struct P { x: i32 }\nfn f() { (p) -> P { x: p } }", f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
  if (!result.ok || result.items.size() != 2) {
    return;
  }
  const ast::ItemFn& fn = as_fn(result.items[1], f);
  const ast::Block& body = f.ast.blocks[fn.body];
  CHECK(body.value.is_valid());
  if (!body.value.is_valid()) {
    return;
  }
  const ast::ExprClosure& closure = as_closure(body.value, f);
  CHECK(f.ast.exprs[closure.body].kind == ast::ExprKind::Struct);
}

TEST_CASE("Parser reads a parenthesized closure as a callee") {
  Fixture f;
  const ast::ExprIdx value = body_value("fn f() { ((a) -> a)(1) }", f);
  CHECK(value.is_valid());
  if (!value.is_valid()) {
    return;
  }
  const ast::ExprNode& call = f.ast.exprs[value];
  CHECK(call.kind == ast::ExprKind::Call);
  if (call.kind != ast::ExprKind::Call) {
    return;
  }
  const ast::ExprIdx callee = call.payload.get<ast::ExprCall>().callee;
  CHECK(f.ast.exprs[callee].kind == ast::ExprKind::Closure);
}

TEST_CASE("Parser requires an arrow after closure params") {
  Fixture f;
  const ParseResult result = parse("fn f() { (x: i32) }", f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
}

TEST_CASE("Parser rejects a token stream without Eof") {
  Fixture f;
  constexpr std::string_view bytes = "fn main() {}\n";
  lexer::Lexer lexer(bytes, source::UNKNOWN_FILE, f.bag);
  std::vector<lexer::Token> tokens;
  lexer.tokenize(tokens);
  tokens.pop_back();
  Parser parser(std::span<const lexer::Token>(tokens.data(), tokens.size()),
                bytes, source::UNKNOWN_FILE, f.ast, f.bag);
  CHECK(parser.parse().is_err());
  CHECK(f.bag.has_errors());
}

// A struct pattern carries a `{` before its `:=`, so the declaration
// scan must treat braces as nesting rather than a statement boundary.
TEST_CASE("Parser reads a struct pattern in a declaration and condition") {
  Fixture f;
  const ParseResult result = parse(
      "struct Foo { x: i32 }\n"
      "fn f(o: Foo) -> i32 {\n"
      "  Foo { x } := o\n"
      "  if Foo { y } := o {\n"
      "    ret y\n"
      "  }\n"
      "  ret x\n"
      "}\n",
      f);
  CHECK(result.ok);
  CHECK(!f.bag.has_errors());
}

}  // namespace parser
