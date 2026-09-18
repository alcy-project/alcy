// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "parser/desugar.h"

#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"

namespace parser {

namespace {

// Diagnostic codes 4100-4199 are reserved for the parser.
constexpr u32 kParserOrPatternMismatch = 4102;
constexpr u32 kParserAlreadyBound = 4103;

bool is_keyword_name(std::string_view name) {
  return name == "self" || name == "super" || name == "package" ||
         name == "Self";
}

struct Desugar {
  struct Binding {
    std::string_view orig;
    std::string_view fresh;
    // Pre-seeded from a sibling or-pattern alternative: reuse without
    // declaring, and track whether the name actually reappears.
    bool preseeded = false;
    bool seen = false;
  };

  mem::Arena& arena;
  diag::DiagBag& bag;
  u32 counter = 0;
  std::vector<std::vector<Binding>> scopes;
  std::vector<ast::Ident> path_scratch;

  Desugar(mem::Arena& arena, diag::DiagBag& bag) : arena(arena), bag(bag) {}

  void push_scope() { scopes.emplace_back(); }

  void pop_scope() { scopes.pop_back(); }

  std::vector<Binding>& current() { return scopes.back(); }

  const Binding* find_innermost(std::string_view name) const {
    for (const Binding& binding : scopes.back()) {
      if (binding.orig == name) {
        return &binding;
      }
    }
    return nullptr;
  }

  std::string_view lookup(std::string_view name) const {
    if (is_keyword_name(name)) {
      return name;
    }
    for (const std::vector<Binding>& scope : scopes | std::views::reverse) {
      for (const Binding& binding : scope) {
        if (binding.orig == name) {
          return binding.fresh;
        }
      }
    }
    return name;
  }

  std::string_view fresh_name(std::string_view orig) {
    std::string spelled = std::string(orig) + "$" + std::to_string(counter);
    ++counter;
    char* const owned = static_cast<char*>(arena.alloc(spelled.size(), 1));
    spelled.copy(owned, spelled.size());
    return std::string_view(owned, spelled.size());
  }

  // Declares one pattern-bound name, returning the spelling to store.
  // Keywords map to themselves; redeclaring a name already present in
  // the innermost scope is an error. A name bound nowhere stays as-is
  // while a shadowed name mints a suffixed spelling, keeping
  // diagnostics clean where nothing is shadowed.
  std::string_view declare_named(const ast::Ident& ident) {
    if (is_keyword_name(ident.name)) {
      return ident.name;
    }
    if (const Binding* existing = find_innermost(ident.name)) {
      if (existing->preseeded) {
        const_cast<Binding*>(existing)->seen = true;
        return existing->fresh;
      }
      const u32 index =
          bag.emit(diag::Severity::Error, kParserAlreadyBound, ident.span,
                   "`{}` is already bound in this scope", ident.name);
      (void)index;
      return ident.name;
    }
    bool shadowed = false;
    for (usize i = 0; i + 1 < scopes.size(); ++i) {
      for (const Binding& binding : scopes[i]) {
        if (binding.orig == ident.name) {
          shadowed = true;
          break;
        }
      }
      if (shadowed) {
        break;
      }
    }
    if (!shadowed) {
      current().push_back(Binding{.orig = ident.name, .fresh = ident.name});
      return ident.name;
    }
    const std::string_view fresh = fresh_name(ident.name);
    current().push_back(Binding{.orig = ident.name, .fresh = fresh});
    return fresh;
  }

  void run(std::span<ast::Item* const> items) {
    push_scope();
    for (ast::Item* item : items) {
      visit_item(item);
    }
    pop_scope();
  }

  void visit_item(ast::Item* item) {
    switch (item->kind) {
      case ast::ItemKind::Fn: {
        ast::FnItem* fn = static_cast<ast::FnItem*>(item);
        push_scope();
        for (const ast::FnParam& param : fn->params) {
          visit_pattern(const_cast<ast::Pattern*>(param.pattern));
        }
        visit_block(const_cast<ast::Block*>(fn->body));
        pop_scope();
        break;
      }
      case ast::ItemKind::Static: {
        ast::StaticItem* decl = static_cast<ast::StaticItem*>(item);
        push_scope();
        visit_expr(const_cast<ast::Expr*>(decl->init));
        pop_scope();
        break;
      }
      case ast::ItemKind::Const: {
        ast::ConstItem* decl = static_cast<ast::ConstItem*>(item);
        push_scope();
        visit_expr(const_cast<ast::Expr*>(decl->init));
        pop_scope();
        break;
      }
      case ast::ItemKind::Impl: {
        ast::ImplItem* impl = static_cast<ast::ImplItem*>(item);
        for (ast::FnItem* method : impl->methods) {
          visit_item(method);
        }
        break;
      }
      case ast::ItemKind::Mod: {
        ast::ModItem* mod = static_cast<ast::ModItem*>(item);
        for (ast::Item* child : mod->items) {
          visit_item(child);
        }
        break;
      }
      case ast::ItemKind::Struct:
      case ast::ItemKind::Enum:
      case ast::ItemKind::Use: break;
    }
  }

  void visit_block(ast::Block* block) {
    if (block == nullptr) {
      return;
    }
    push_scope();
    for (ast::Stmt* stmt : block->statements) {
      visit_stmt(stmt);
    }
    if (block->value != nullptr) {
      visit_expr(const_cast<ast::Expr*>(block->value));
    }
    pop_scope();
  }

  void visit_stmt(ast::Stmt* stmt) {
    switch (stmt->kind) {
      case ast::StmtKind::Decl: {
        ast::DeclStmt* decl = static_cast<ast::DeclStmt*>(stmt);
        // Initializers evaluate before the bindings they introduce, so
        // `x := x` reads the outer `x`.
        visit_expr(const_cast<ast::Expr*>(decl->init));
        visit_pattern(const_cast<ast::Pattern*>(decl->pattern));
        break;
      }
      case ast::StmtKind::Reassign: {
        ast::ReassignStmt* reassign = static_cast<ast::ReassignStmt*>(stmt);
        visit_expr(const_cast<ast::Expr*>(reassign->place));
        visit_expr(const_cast<ast::Expr*>(reassign->value));
        break;
      }
      case ast::StmtKind::Expr: {
        ast::ExprStmt* expr_stmt = static_cast<ast::ExprStmt*>(stmt);
        visit_expr(const_cast<ast::Expr*>(expr_stmt->value));
        break;
      }
    }
  }

  void visit_pattern(ast::Pattern* pattern) {
    switch (pattern->kind) {
      case ast::PatternKind::Wildcard: break;
      case ast::PatternKind::Ident: {
        ast::IdentPattern* ident = static_cast<ast::IdentPattern*>(pattern);
        ident->name.name = declare_named(ident->name);
        break;
      }
      case ast::PatternKind::MutIdent: {
        ast::MutIdentPattern* ident =
            static_cast<ast::MutIdentPattern*>(pattern);
        ident->name.name = declare_named(ident->name);
        break;
      }
      case ast::PatternKind::Literal: break;
      case ast::PatternKind::Tuple: {
        ast::TuplePattern* tuple = static_cast<ast::TuplePattern*>(pattern);
        for (ast::Pattern* element : tuple->elements) {
          visit_pattern(element);
        }
        break;
      }
      case ast::PatternKind::Struct: {
        ast::StructPattern* record = static_cast<ast::StructPattern*>(pattern);
        for (const ast::FieldPattern& field : record->fields) {
          visit_pattern(const_cast<ast::Pattern*>(field.pattern));
        }
        break;
      }
      case ast::PatternKind::Ref: {
        ast::RefPattern* ref = static_cast<ast::RefPattern*>(pattern);
        visit_pattern(const_cast<ast::Pattern*>(ref->inner));
        break;
      }
      case ast::PatternKind::Or:
        visit_or(static_cast<ast::OrPattern*>(pattern));
        break;
    }
  }

  void visit_or(ast::OrPattern* or_pattern) {
    // The first alternative establishes the scope; the rest reuse its
    // mapping and must bind the identical set. Afterwards the mapping
    // merges into the current scope so siblings and bodies resolve.
    push_scope();
    visit_pattern(const_cast<ast::Pattern*>(or_pattern->alternatives[0]));
    std::vector<Binding> first = std::move(current());
    pop_scope();
    for (usize i = 1; i < or_pattern->alternatives.size(); ++i) {
      push_scope();
      for (Binding binding : first) {
        binding.seen = false;
        binding.preseeded = true;
        current().push_back(binding);
      }
      visit_pattern(const_cast<ast::Pattern*>(or_pattern->alternatives[i]));
      bool mismatch = current().size() != first.size();
      if (!mismatch) {
        for (const Binding& binding : current()) {
          if (binding.preseeded && !binding.seen) {
            mismatch = true;
            break;
          }
        }
      }
      if (mismatch) {
        const u32 index = bag.emit(
            diag::Severity::Error, kParserOrPatternMismatch, or_pattern->span,
            "or-pattern alternatives must bind the same names");
        (void)index;
      }
      pop_scope();
    }
    for (const Binding& binding : first) {
      bool clash = false;
      for (const Binding& existing : current()) {
        if (existing.orig == binding.orig && !existing.preseeded) {
          clash = true;
          break;
        }
      }
      if (clash) {
        const u32 index = bag.emit(
            diag::Severity::Error, kParserAlreadyBound, or_pattern->span,
            "`{}` is already bound in this scope", binding.orig);
        (void)index;
        continue;
      }
      current().push_back(binding);
    }
  }

  void visit_cond(ast::Cond* cond, bool* pushed) {
    // Initializers evaluate outside the bindings they introduce.
    if (cond->is_pattern) {
      visit_expr(const_cast<ast::Expr*>(cond->init));
      push_scope();
      visit_pattern(const_cast<ast::Pattern*>(cond->pattern));
      *pushed = true;
      return;
    }
    *pushed = false;
    visit_expr(const_cast<ast::Expr*>(cond->value));
  }

  void visit_expr(ast::Expr* expr) {
    if (expr == nullptr) {
      return;
    }
    switch (expr->kind) {
      case ast::ExprKind::Literal: break;
      case ast::ExprKind::Path: {
        ast::PathExpr* path = static_cast<ast::PathExpr*>(expr);
        if (path->path->segments.size() == 1) {
          const ast::Ident& first = path->path->segments[0];
          const std::string_view resolved = lookup(first.name);
          if (resolved != first.name) {
            path_scratch.clear();
            for (const ast::Ident& segment : path->path->segments) {
              path_scratch.push_back(segment);
            }
            path_scratch[0].name = resolved;
            ast::Path* renamed = arena.create<ast::Path>();
            renamed->segments = ast::copy_to_arena(arena, path_scratch);
            renamed->span = path->path->span;
            path->path = renamed;
          }
        }
        break;
      }
      case ast::ExprKind::Struct: {
        ast::StructExpr* init = static_cast<ast::StructExpr*>(expr);
        for (const ast::FieldInit& field : init->init) {
          visit_expr(const_cast<ast::Expr*>(field.value));
        }
        if (init->base_expr != nullptr) {
          visit_expr(const_cast<ast::Expr*>(init->base_expr));
        }
        break;
      }
      case ast::ExprKind::Tuple: {
        ast::TupleExpr* tuple = static_cast<ast::TupleExpr*>(expr);
        for (ast::Expr* element : tuple->elements) {
          visit_expr(element);
        }
        break;
      }
      case ast::ExprKind::Unary: {
        ast::UnaryExpr* unary = static_cast<ast::UnaryExpr*>(expr);
        visit_expr(const_cast<ast::Expr*>(unary->inner));
        break;
      }
      case ast::ExprKind::Binary: {
        ast::BinaryExpr* binary = static_cast<ast::BinaryExpr*>(expr);
        visit_expr(const_cast<ast::Expr*>(binary->lhs));
        visit_expr(const_cast<ast::Expr*>(binary->rhs));
        break;
      }
      case ast::ExprKind::Cast: {
        ast::CastExpr* cast = static_cast<ast::CastExpr*>(expr);
        visit_expr(const_cast<ast::Expr*>(cast->inner));
        break;
      }
      case ast::ExprKind::Call: {
        ast::CallExpr* call = static_cast<ast::CallExpr*>(expr);
        visit_expr(const_cast<ast::Expr*>(call->callee));
        for (ast::Expr* arg : call->args) {
          visit_expr(arg);
        }
        break;
      }
      case ast::ExprKind::MethodCall: {
        ast::MethodCallExpr* call = static_cast<ast::MethodCallExpr*>(expr);
        visit_expr(const_cast<ast::Expr*>(call->receiver));
        for (ast::Expr* arg : call->args) {
          visit_expr(arg);
        }
        break;
      }
      case ast::ExprKind::Field: {
        ast::FieldExpr* field = static_cast<ast::FieldExpr*>(expr);
        visit_expr(const_cast<ast::Expr*>(field->receiver));
        break;
      }
      case ast::ExprKind::Index: {
        ast::IndexExpr* index = static_cast<ast::IndexExpr*>(expr);
        visit_expr(const_cast<ast::Expr*>(index->receiver));
        visit_expr(const_cast<ast::Expr*>(index->index));
        break;
      }
      case ast::ExprKind::Question: {
        ast::QuestionExpr* question = static_cast<ast::QuestionExpr*>(expr);
        visit_expr(const_cast<ast::Expr*>(question->inner));
        break;
      }
      case ast::ExprKind::If: {
        ast::IfExpr* branch = static_cast<ast::IfExpr*>(expr);
        bool pushed = false;
        visit_cond(const_cast<ast::Cond*>(branch->cond), &pushed);
        visit_block(const_cast<ast::Block*>(branch->then_block));
        if (pushed) {
          pop_scope();
        }
        visit_block(const_cast<ast::Block*>(branch->else_block));
        break;
      }
      case ast::ExprKind::Match: {
        ast::MatchExpr* match = static_cast<ast::MatchExpr*>(expr);
        visit_expr(const_cast<ast::Expr*>(match->scrutinee));
        for (const ast::MatchArm& arm : match->arms) {
          push_scope();
          visit_pattern(const_cast<ast::Pattern*>(arm.pattern));
          visit_expr(const_cast<ast::Expr*>(arm.body));
          pop_scope();
        }
        break;
      }
      case ast::ExprKind::Loop: {
        ast::LoopExpr* loop = static_cast<ast::LoopExpr*>(expr);
        visit_block(const_cast<ast::Block*>(loop->body));
        break;
      }
      case ast::ExprKind::While: {
        ast::WhileExpr* loop = static_cast<ast::WhileExpr*>(expr);
        bool pushed = false;
        visit_cond(const_cast<ast::Cond*>(loop->cond), &pushed);
        visit_block(const_cast<ast::Block*>(loop->body));
        if (pushed) {
          pop_scope();
        }
        break;
      }
      case ast::ExprKind::Block: {
        ast::BlockExpr* block = static_cast<ast::BlockExpr*>(expr);
        visit_block(const_cast<ast::Block*>(block->block));
        break;
      }
      case ast::ExprKind::Return: {
        ast::ReturnExpr* ret = static_cast<ast::ReturnExpr*>(expr);
        visit_expr(const_cast<ast::Expr*>(ret->value));
        break;
      }
      case ast::ExprKind::Range: {
        ast::RangeExpr* range = static_cast<ast::RangeExpr*>(expr);
        visit_expr(const_cast<ast::Expr*>(range->start));
        visit_expr(const_cast<ast::Expr*>(range->end));
        break;
      }
      case ast::ExprKind::Break:
      case ast::ExprKind::Continue: break;
    }
  }
};

}  // namespace

void desugar_shadowing(std::span<ast::Item* const> items,
                       mem::Arena& arena,
                       diag::DiagBag& bag) {
  Desugar desugar{arena, bag};
  desugar.run(items);
}

}  // namespace parser
