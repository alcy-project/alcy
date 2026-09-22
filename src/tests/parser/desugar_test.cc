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
  diag::DiagBag bag{arena};

  Fixture() { arena.reserve(1u << 20); }
};

std::span<ast::Item* const> parse(std::string_view bytes, Fixture& f) {
  lexer::Lexer lexer(bytes, source::kUnknownFile, f.bag);
  std::vector<lexer::Token> tokens;
  lexer.tokenize(tokens);
  Parser parser(std::span<const lexer::Token>(tokens.data(), tokens.size()),
                bytes, source::kUnknownFile, f.arena, f.bag);
  return parser.parse();
}

// Collects binding and use spellings in source order: IdentPattern and
// MutIdentPattern declarations, single-segment path uses.
struct NameCollector {
  std::vector<std::string> names;

  void run(std::span<ast::Item* const> items) {
    for (ast::Item* item : items) {
      visit_item(item);
    }
  }

  void visit_item(ast::Item* item) {
    switch (item->kind) {
      case ast::ItemKind::Fn: {
        const ast::FnItem* fn = static_cast<const ast::FnItem*>(item);
        for (const ast::FnParam& param : fn->params) {
          visit_pattern(param.pattern);
        }
        visit_block(fn->body);
        break;
      }
      case ast::ItemKind::Static: {
        const ast::StaticItem* decl = static_cast<const ast::StaticItem*>(item);
        visit_expr(decl->init);
        break;
      }
      case ast::ItemKind::Const: {
        const ast::ConstItem* decl = static_cast<const ast::ConstItem*>(item);
        visit_expr(decl->init);
        break;
      }
      case ast::ItemKind::Impl: {
        const ast::ImplItem* impl = static_cast<const ast::ImplItem*>(item);
        for (ast::FnItem* method : impl->methods) {
          visit_item(method);
        }
        break;
      }
      case ast::ItemKind::Struct:
      case ast::ItemKind::Enum:
      case ast::ItemKind::Use: break;
    }
  }

  void visit_block(const ast::Block* block) {
    if (block == nullptr) {
      return;
    }
    for (ast::Stmt* stmt : block->statements) {
      visit_stmt(stmt);
    }
    visit_expr(block->value);
  }

  void visit_stmt(const ast::Stmt* stmt) {
    switch (stmt->kind) {
      case ast::StmtKind::Decl: {
        const ast::DeclStmt* decl = static_cast<const ast::DeclStmt*>(stmt);
        visit_pattern(decl->pattern);
        visit_expr(decl->init);
        break;
      }
      case ast::StmtKind::Reassign: {
        const ast::ReassignStmt* reassign =
            static_cast<const ast::ReassignStmt*>(stmt);
        visit_expr(reassign->place);
        visit_expr(reassign->value);
        break;
      }
      case ast::StmtKind::Expr: {
        const ast::ExprStmt* expr_stmt =
            static_cast<const ast::ExprStmt*>(stmt);
        visit_expr(expr_stmt->value);
        break;
      }
    }
  }

  void visit_pattern(const ast::Pattern* pattern) {
    switch (pattern->kind) {
      case ast::PatternKind::Wildcard:
      case ast::PatternKind::Literal: break;
      case ast::PatternKind::Ident: {
        const ast::IdentPattern* ident =
            static_cast<const ast::IdentPattern*>(pattern);
        names.emplace_back(ident->name.name);
        break;
      }
      case ast::PatternKind::MutIdent: {
        const ast::MutIdentPattern* ident =
            static_cast<const ast::MutIdentPattern*>(pattern);
        names.emplace_back(ident->name.name);
        break;
      }
      case ast::PatternKind::Tuple: {
        const ast::TuplePattern* tuple =
            static_cast<const ast::TuplePattern*>(pattern);
        for (ast::Pattern* element : tuple->elements) {
          visit_pattern(element);
        }
        break;
      }
      case ast::PatternKind::Struct: {
        const ast::StructPattern* record =
            static_cast<const ast::StructPattern*>(pattern);
        for (const ast::FieldPattern& field : record->fields) {
          visit_pattern(field.pattern);
        }
        break;
      }
      case ast::PatternKind::Ref: {
        const ast::RefPattern* ref =
            static_cast<const ast::RefPattern*>(pattern);
        visit_pattern(ref->inner);
        break;
      }
      case ast::PatternKind::Or: {
        const ast::OrPattern* or_pattern =
            static_cast<const ast::OrPattern*>(pattern);
        for (ast::Pattern* alternative : or_pattern->alternatives) {
          visit_pattern(alternative);
        }
        break;
      }
    }
  }

  void visit_expr(const ast::Expr* expr) {
    if (expr == nullptr) {
      return;
    }
    switch (expr->kind) {
      case ast::ExprKind::Literal: break;
      case ast::ExprKind::Path: {
        const ast::PathExpr* path = static_cast<const ast::PathExpr*>(expr);
        if (path->path->segments.size() == 1) {
          names.emplace_back(path->path->segments[0].name);
        } else {
          std::string joined;
          for (const ast::Ident& segment : path->path->segments) {
            if (!joined.empty()) {
              joined.push_back(':');
              joined.push_back(':');
            }
            joined.append(segment.name);
          }
          names.push_back(joined);
        }
        break;
      }
      case ast::ExprKind::Struct: {
        const ast::StructExpr* init = static_cast<const ast::StructExpr*>(expr);
        for (const ast::FieldInit& field : init->init) {
          visit_expr(field.value);
        }
        visit_expr(init->base_expr);
        break;
      }
      case ast::ExprKind::Tuple: {
        const ast::TupleExpr* tuple = static_cast<const ast::TupleExpr*>(expr);
        for (ast::Expr* element : tuple->elements) {
          visit_expr(element);
        }
        break;
      }
      case ast::ExprKind::Unary: {
        const ast::UnaryExpr* unary = static_cast<const ast::UnaryExpr*>(expr);
        visit_expr(unary->inner);
        break;
      }
      case ast::ExprKind::Binary: {
        const ast::BinaryExpr* binary =
            static_cast<const ast::BinaryExpr*>(expr);
        visit_expr(binary->lhs);
        visit_expr(binary->rhs);
        break;
      }
      case ast::ExprKind::Cast: {
        const ast::CastExpr* cast = static_cast<const ast::CastExpr*>(expr);
        visit_expr(cast->inner);
        break;
      }
      case ast::ExprKind::Call: {
        const ast::CallExpr* call = static_cast<const ast::CallExpr*>(expr);
        visit_expr(call->callee);
        for (ast::Expr* arg : call->args) {
          visit_expr(arg);
        }
        break;
      }
      case ast::ExprKind::MethodCall: {
        const ast::MethodCallExpr* call =
            static_cast<const ast::MethodCallExpr*>(expr);
        visit_expr(call->receiver);
        for (ast::Expr* arg : call->args) {
          visit_expr(arg);
        }
        break;
      }
      case ast::ExprKind::Field: {
        const ast::FieldExpr* field = static_cast<const ast::FieldExpr*>(expr);
        visit_expr(field->receiver);
        break;
      }
      case ast::ExprKind::Index: {
        const ast::IndexExpr* index = static_cast<const ast::IndexExpr*>(expr);
        visit_expr(index->receiver);
        visit_expr(index->index);
        break;
      }
      case ast::ExprKind::Question: {
        const ast::QuestionExpr* question =
            static_cast<const ast::QuestionExpr*>(expr);
        visit_expr(question->inner);
        break;
      }
      case ast::ExprKind::If: {
        const ast::IfExpr* branch = static_cast<const ast::IfExpr*>(expr);
        if (branch->cond->is_pattern) {
          visit_pattern(branch->cond->pattern);
          visit_expr(branch->cond->init);
        } else {
          visit_expr(branch->cond->value);
        }
        visit_block(branch->then_block);
        visit_block(branch->else_block);
        break;
      }
      case ast::ExprKind::Match: {
        const ast::MatchExpr* match = static_cast<const ast::MatchExpr*>(expr);
        visit_expr(match->scrutinee);
        for (const ast::MatchArm& arm : match->arms) {
          visit_pattern(arm.pattern);
          visit_expr(arm.body);
        }
        break;
      }
      case ast::ExprKind::Loop: {
        const ast::LoopExpr* loop = static_cast<const ast::LoopExpr*>(expr);
        visit_block(loop->body);
        break;
      }
      case ast::ExprKind::While: {
        const ast::WhileExpr* loop = static_cast<const ast::WhileExpr*>(expr);
        if (loop->cond->is_pattern) {
          visit_pattern(loop->cond->pattern);
          visit_expr(loop->cond->init);
        } else {
          visit_expr(loop->cond->value);
        }
        visit_block(loop->body);
        break;
      }
      case ast::ExprKind::Block: {
        const ast::BlockExpr* block = static_cast<const ast::BlockExpr*>(expr);
        visit_block(block->block);
        break;
      }
      case ast::ExprKind::Return: {
        const ast::ReturnExpr* ret = static_cast<const ast::ReturnExpr*>(expr);
        visit_expr(ret->value);
        break;
      }
      case ast::ExprKind::Range: {
        const ast::RangeExpr* range = static_cast<const ast::RangeExpr*>(expr);
        visit_expr(range->start);
        visit_expr(range->end);
        break;
      }
      case ast::ExprKind::Borrow: {
        const ast::BorrowExpr* borrow =
            static_cast<const ast::BorrowExpr*>(expr);
        visit_expr(borrow->inner);
        break;
      }
      case ast::ExprKind::Break:
      case ast::ExprKind::Continue: break;
    }
  }
};

std::vector<std::string> collect_names(std::span<ast::Item* const> items) {
  NameCollector collector;
  collector.run(items);
  return collector.names;
}

bool check_names(std::string_view bytes,
                 Fixture& f,
                 const std::vector<std::string>& expected) {
  const std::span<ast::Item* const> items = parse(bytes, f);
  if (f.bag.has_errors()) {
    return false;
  }
  desugar_shadowing(items, f.arena, f.bag);
  if (f.bag.has_errors()) {
    return false;
  }
  return collect_names(items) == expected;
}

// True when parsing succeeds but desugaring reports errors.
bool check_desugar_fails(std::string_view bytes, Fixture& f) {
  const std::span<ast::Item* const> items = parse(bytes, f);
  if (f.bag.has_errors()) {
    return false;
  }
  desugar_shadowing(items, f.arena, f.bag);
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
