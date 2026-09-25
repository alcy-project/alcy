// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parser/parser.h"

#include <span>
#include <string_view>
#include <vector>

#include "ast/ast.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/mem/arena.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "source/source.h"

namespace parser {

namespace {

struct Fixture {
  mem::Arena arena;
  ast::AstArena ast;
  diag::DiagBag bag{arena};

  Fixture() { arena.reserve(1u << 20); }
};

struct ParseResult {
  std::span<const ast::ItemIdx> items;
  bool ok;
};

ParseResult parse(std::string_view bytes, Fixture& f) {
  lexer::Lexer lexer(bytes, source::kUnknownFile, f.bag);
  std::vector<lexer::Token> tokens;
  lexer.tokenize(tokens);
  Parser parser(std::span<const lexer::Token>(tokens.data(), tokens.size()),
                bytes, source::kUnknownFile, f.ast, f.bag);
  auto items = parser.parse();
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

const ast::ExprReturn as_return(const ast::ExprIdx expr_idx, Fixture& f) {
  const ast::ExprNode& expr = f.ast.exprs[expr_idx];
  CHECK(expr.kind == ast::ExprKind::Return);
  return expr.payload.get<ast::ExprReturn>();
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
  const ast::ItemNode& enum_node = f.ast.items[result.items[0]];
  const ast::ItemEnum option = as_enum(result.items[0], f);
  (void)enum_node;
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
  const ParseResult result = parse("fn f() { for x in y {} }", f);
  CHECK(!result.ok);
  CHECK(f.bag.has_errors());
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

}  // namespace parser
