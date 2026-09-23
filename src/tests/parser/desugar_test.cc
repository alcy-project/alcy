// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parser/desugar.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ast/ast.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/mem/arena.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "parser/parser.h"
#include "source/source.h"

namespace parser {

namespace {

struct Fixture {
  mem::Arena arena;
  ast::AstArena ast;
  diag::DiagBag bag{arena};

  Fixture() { arena.reserve(1u << 20); }
};

std::span<const ast::ItemIdx> parse(std::string_view bytes, Fixture& f) {
  lexer::Lexer lexer(bytes, source::kUnknownFile, f.bag);
  std::vector<lexer::Token> tokens;
  lexer.tokenize(tokens);
  Parser parser(std::span<const lexer::Token>(tokens.data(), tokens.size()),
                bytes, source::kUnknownFile, f.ast, f.bag);
  return parser.parse();
}

// Collects binding and use spellings in source order: IdentPattern and
// MutIdentPattern declarations, single-segment path uses.
struct NameCollector {
  std::vector<std::string> names;
  ast::AstArena& ast;

  explicit NameCollector(ast::AstArena& ast) : ast(ast) {}

  void run(std::span<const ast::ItemIdx> items) {
    for (ast::ItemIdx item_idx : items) {
      visit_item(item_idx);
    }
  }

  void visit_item(ast::ItemIdx item_idx) {
    const ast::ItemNode& item = ast.items[item_idx];
    switch (item.kind) {
      case ast::ItemKind::Fn: {
        const ast::ItemFn& fn = item.payload.get<ast::ItemFn>();
        for (const ast::ItemFnParam& param : fn.params) {
          visit_pattern(param.pattern);
        }
        visit_block(fn.body);
        break;
      }
      case ast::ItemKind::Static: {
        const ast::ItemStatic& decl = item.payload.get<ast::ItemStatic>();
        visit_expr(decl.init);
        break;
      }
      case ast::ItemKind::Const: {
        const ast::ItemConst& decl = item.payload.get<ast::ItemConst>();
        visit_expr(decl.init);
        break;
      }
      case ast::ItemKind::Impl: {
        const ast::ItemImpl& impl = item.payload.get<ast::ItemImpl>();
        for (ast::ItemIdx method_idx : impl.methods) {
          visit_item(method_idx);
        }
        break;
      }
      case ast::ItemKind::Struct:
      case ast::ItemKind::Enum:
      case ast::ItemKind::Use: break;
    }
  }

  void visit_block(ast::BlockIdx block_idx) {
    if (!block_idx.is_valid()) {
      return;
    }
    const ast::Block& block = ast.blocks[block_idx];
    for (ast::StmtIdx stmt_idx : block.statements) {
      visit_stmt(stmt_idx);
    }
    visit_expr(block.value);
  }

  void visit_stmt(ast::StmtIdx stmt_idx) {
    const ast::StmtNode& stmt = ast.stmts[stmt_idx];
    switch (stmt.kind) {
      case ast::StmtKind::Decl: {
        const ast::StmtDecl& decl = stmt.payload.get<ast::StmtDecl>();
        visit_pattern(decl.pattern);
        visit_expr(decl.init);
        break;
      }
      case ast::StmtKind::Reassign: {
        const ast::StmtReassign& reassign =
            stmt.payload.get<ast::StmtReassign>();
        visit_expr(reassign.place);
        visit_expr(reassign.value);
        break;
      }
      case ast::StmtKind::Expr: {
        const ast::StmtExpr& expr_stmt = stmt.payload.get<ast::StmtExpr>();
        visit_expr(expr_stmt.value);
        break;
      }
    }
  }

  void visit_pattern(ast::PatternIdx pat_idx) {
    const ast::PatternNode& pat = ast.patterns[pat_idx];
    switch (pat.kind) {
      case ast::PatternKind::Wildcard:
      case ast::PatternKind::Literal: break;
      case ast::PatternKind::Ident: {
        const ast::PatternIdent& ident = pat.payload.ident;
        names.emplace_back(ident.name.name);
        break;
      }
      case ast::PatternKind::MutIdent: {
        const ast::PatternIdent& ident = pat.payload.mut_ident;
        names.emplace_back(ident.name.name);
        break;
      }
      case ast::PatternKind::Tuple: {
        const ast::PatternTuple& tuple = pat.payload.tuple;
        for (ast::PatternIdx element_idx : tuple.elements) {
          visit_pattern(element_idx);
        }
        break;
      }
      case ast::PatternKind::Struct: {
        const ast::PatternStruct record = pat.payload.strukt;
        for (const ast::FieldPattern& field : record.fields) {
          visit_pattern(field.pattern);
        }
        break;
      }
      case ast::PatternKind::Ref: {
        const ast::PatternRef ref = pat.payload.ref;
        visit_pattern(ref.inner);
        break;
      }
      case ast::PatternKind::Or: {
        const ast::PatternOr or_pattern = pat.payload.or_pat;
        for (ast::PatternIdx alternative_idx : or_pattern.alternatives) {
          visit_pattern(alternative_idx);
        }
        break;
      }
    }
  }

  void visit_expr(ast::ExprIdx expr_idx) {
    if (!expr_idx.is_valid()) {
      return;
    }
    const ast::ExprNode& expr = ast.exprs[expr_idx];
    switch (expr.kind) {
      case ast::ExprKind::Literal: break;
      case ast::ExprKind::Path: {
        const ast::ExprPath& path = expr.payload.get<ast::ExprPath>();
        if (path.idx.is_valid()) {
          const ast::Path& p = ast.paths[path.idx];
          if (p.segments.size() == 1) {
            names.emplace_back(p.segments[0].name);
          } else {
            std::string joined;
            for (const ast::Ident& segment : p.segments) {
              if (!joined.empty()) {
                joined.push_back(':');
                joined.push_back(':');
              }
              joined.append(segment.name);
            }
            names.push_back(joined);
          }
        }
        break;
      }
      case ast::ExprKind::Struct: {
        const ast::ExprStruct& init = expr.payload.get<ast::ExprStruct>();
        for (const ast::ExprFieldInit& field : init.init) {
          visit_expr(field.value);
        }
        visit_expr(init.base_expr);
        break;
      }
      case ast::ExprKind::Tuple: {
        const ast::ExprTuple& tuple = expr.payload.get<ast::ExprTuple>();
        for (ast::ExprIdx element_idx : tuple.elements) {
          visit_expr(element_idx);
        }
        break;
      }
      case ast::ExprKind::Unary: {
        const ast::ExprUnary& unary = expr.payload.get<ast::ExprUnary>();
        visit_expr(unary.inner);
        break;
      }
      case ast::ExprKind::Binary: {
        const ast::ExprBinary& binary = expr.payload.get<ast::ExprBinary>();
        visit_expr(binary.lhs);
        visit_expr(binary.rhs);
        break;
      }
      case ast::ExprKind::Cast: {
        const ast::ExprCast& cast = expr.payload.get<ast::ExprCast>();
        visit_expr(cast.inner);
        break;
      }
      case ast::ExprKind::Call: {
        const ast::ExprCall& call = expr.payload.get<ast::ExprCall>();
        visit_expr(call.callee);
        for (ast::ExprIdx arg_idx : call.args) {
          visit_expr(arg_idx);
        }
        break;
      }
      case ast::ExprKind::MethodCall: {
        const ast::ExprMethodCall& call =
            expr.payload.get<ast::ExprMethodCall>();
        visit_expr(call.receiver);
        for (ast::ExprIdx arg_idx : call.args) {
          visit_expr(arg_idx);
        }
        break;
      }
      case ast::ExprKind::Field: {
        const ast::ExprField& field = expr.payload.get<ast::ExprField>();
        visit_expr(field.receiver);
        break;
      }
      case ast::ExprKind::Index: {
        const ast::ExprIndex& index = expr.payload.get<ast::ExprIndex>();
        visit_expr(index.receiver);
        visit_expr(index.index);
        break;
      }
      case ast::ExprKind::Question: {
        const ast::ExprQuestion& question =
            expr.payload.get<ast::ExprQuestion>();
        visit_expr(question.inner);
        break;
      }
      case ast::ExprKind::If: {
        const ast::ExprIf& branch = expr.payload.get<ast::ExprIf>();
        if (branch.cond.is_valid()) {
          const ast::Cond& cond = ast.conds[branch.cond];
          if (cond.is_pattern) {
            visit_pattern(cond.pattern);
            visit_expr(cond.init);
          } else {
            visit_expr(cond.value);
          }
        }
        visit_block(branch.then_block);
        visit_block(branch.else_block);
        break;
      }
      case ast::ExprKind::Match: {
        const ast::ExprMatch& match = expr.payload.get<ast::ExprMatch>();
        visit_expr(match.scrutinee);
        for (const ast::ExprMatchArm& arm : match.arms) {
          visit_pattern(arm.pattern);
          visit_expr(arm.body);
        }
        break;
      }
      case ast::ExprKind::Loop: {
        const ast::ExprLoop& loop = expr.payload.get<ast::ExprLoop>();
        visit_block(loop.body);
        break;
      }
      case ast::ExprKind::While: {
        const ast::ExprWhile& loop = expr.payload.get<ast::ExprWhile>();
        if (loop.cond.is_valid()) {
          const ast::Cond& cond = ast.conds[loop.cond];
          if (cond.is_pattern) {
            visit_pattern(cond.pattern);
            visit_expr(cond.init);
          } else {
            visit_expr(cond.value);
          }
        }
        visit_block(loop.body);
        break;
      }
      case ast::ExprKind::Block: {
        const ast::ExprBlock& block = expr.payload.get<ast::ExprBlock>();
        visit_block(block.block);
        break;
      }
      case ast::ExprKind::Return: {
        const ast::ExprReturn& ret = expr.payload.get<ast::ExprReturn>();
        visit_expr(ret.value);
        break;
      }
      case ast::ExprKind::Range: {
        const ast::ExprRange& range = expr.payload.get<ast::ExprRange>();
        visit_expr(range.start);
        visit_expr(range.end);
        break;
      }
      case ast::ExprKind::Borrow: {
        const ast::ExprBorrow& borrow = expr.payload.get<ast::ExprBorrow>();
        visit_expr(borrow.inner);
        break;
      }
      case ast::ExprKind::Break:
      case ast::ExprKind::Continue: break;
    }
  }
};

std::vector<std::string> collect_names(std::span<const ast::ItemIdx> items,
                                       ast::AstArena& ast) {
  NameCollector collector(ast);
  collector.run(items);
  return collector.names;
}

bool check_names(std::string_view bytes,
                 Fixture& f,
                 const std::vector<std::string>& expected) {
  std::span<const ast::ItemIdx> items = parse(bytes, f);
  if (f.bag.has_errors()) {
    return false;
  }
  desugar_shadowing(items, f.ast, f.bag);
  if (f.bag.has_errors()) {
    return false;
  }
  return collect_names(items, f.ast) == expected;
}

// True when parsing succeeds but desugaring reports errors.
bool check_desugar_fails(std::string_view bytes, Fixture& f) {
  std::span<const ast::ItemIdx> items = parse(bytes, f);
  if (f.bag.has_errors()) {
    return false;
  }
  desugar_shadowing(items, f.ast, f.bag);
  return f.bag.has_errors();
}

}  // namespace

TEST_CASE("Desugar keeps first declarations and freshens shadows") {
  Fixture f;
  CHECK(check_names("fn f() { x := 1; { x := 2; y := x; }; z := x; }", f,
                    {"x", "x$0", "y", "x$0", "z", "x"}));
}

TEST_CASE("Desugar resolves uses to their shadowing scope") {
  Fixture f;
  CHECK(check_names("fn f(a: i32) { b := a; { b := 1; }; c := b; }", f,
                    {"a", "b", "a", "b$0", "c", "b"}));
}

TEST_CASE("Desugar rejects same-scope redeclarations") {
  Fixture f;
  CHECK(check_desugar_fails("fn f() { x := 1; x := 2; }", f));
}

TEST_CASE("Desugar rejects duplicate pattern bindings") {
  Fixture f;
  CHECK(check_desugar_fails("fn f(p: T) { (x, x) := p; }", f));
}

TEST_CASE("Desugar shares or-pattern bindings across alternatives") {
  Fixture f;
  CHECK(
      check_names("fn f(v: T) -> i32 { match v { A(x) | B(x) => x, _ => 0 } }",
                  f, {"v", "v", "x", "x", "x"}));
}

TEST_CASE("Desugar rejects mismatched or-pattern bindings") {
  Fixture f;
  CHECK(check_desugar_fails(
      "fn f(v: T) -> i32 { match v { A(x) | B(y) => 0 } }", f));
}

TEST_CASE("Desugar leaves keywords and module paths alone") {
  Fixture f;
  CHECK(
      check_names("fn get(self: Self) -> i32 { ret self.x }\n"
                  "fn useit() -> i32 { ret package::other + 1 }\n",
                  f, {"self", "self", "package::other"}));
}

}  // namespace parser
