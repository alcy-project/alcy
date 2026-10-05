// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parser/desugar.h"

#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "base/nesting.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/messages.h"
#include "parser/diag_code.h"

namespace parser {

namespace {

bool is_keyword_name(std::string_view name) {
  return name == "self" || name == "super" || name == "package" ||
         name == "Self";
}

class Desugarer {
 public:
  struct Binding {
    std::string_view orig;
    std::string_view fresh;
    // Pre-seeded from a sibling or-pattern alternative: reuse without
    // declaring, and track whether the name actually reappears.
    bool preseeded = false;
    bool seen = false;
  };

  ast::AstArena& ast;
  diag::DiagBag& bag;
  u32 counter = 0;
  std::vector<std::vector<Binding>> scopes;
  std::vector<ast::Ident> path_scratch;
  // Bounds the tree walk; see base::MAX_NESTING.
  base::NestingGuard nesting_{base::MAX_NESTING};
  bool reported_too_deep_ = false;

  Desugarer(ast::AstArena& ast, diag::DiagBag& bag) : ast(ast), bag(bag) {}

  // Reports the nesting budget once and returns true when the caller
  // must stop descending.
  bool nesting_exhausted(diag::Span span) {
    if (!nesting_.exhausted()) {
      return false;
    }
    if (!reported_too_deep_) {
      reported_too_deep_ = true;
      const u32 index = bag.emit<i18n::Key::ParserNestingTooDeep>(
          diag::Severity::Error, diag::Stage::Parser, DiagCode::TooDeep, span,
          nesting_.limit());
      (void)index;
    }
    return true;
  }

  void push_scope() { scopes.emplace_back(); }

  void pop_scope() { scopes.pop_back(); }

  std::vector<Binding>& current() { return scopes.back(); }

  Binding* find_innermost(std::string_view name) {
    for (Binding& binding : scopes.back()) {
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
    char* const owned = static_cast<char*>(ast.spans.alloc(spelled.size(), 1));
    if (owned == nullptr) {
      // The arena is spent, so the file is refused and no name it mints
      // is read; keeping the original spelling says what the name was.
      return orig;
    }
    spelled.copy(owned, spelled.size());
    return std::string_view(owned, spelled.size());
  }

  // Declares one pattern-bound name, returning the spelling to store.
  // A name bound nowhere keeps its spelling so diagnostics stay clean;
  // only a shadowed name mints a suffixed one.
  std::string_view declare_named(const ast::Ident& ident) {
    if (is_keyword_name(ident.name)) {
      return ident.name;
    }
    if (Binding* existing = find_innermost(ident.name)) {
      if (existing->preseeded) {
        existing->seen = true;
        return existing->fresh;
      }
      const u32 index = bag.emit<i18n::Key::ParserNameAlreadyBound>(
          diag::Severity::Error, diag::Stage::Parser, DiagCode::AlreadyBound,
          ident.span, ident.name);
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

  void run(std::span<const ast::ItemIdx> items) {
    push_scope();
    for (ast::ItemIdx item : items) {
      visit_item(item);
    }
    pop_scope();
  }

  void visit_item(ast::ItemIdx item) {
    // An index the parser never built names no node, and a spent arena
    // means what it built is not a tree; either way there is nothing here
    // to walk.
    if (!item.is_valid() || ast.exhausted()) {
      return;
    }
    const ast::ItemNode& node = ast.items[item];
    switch (node.kind) {
      case ast::ItemKind::Fn: {
        push_scope();
        for (const ast::ItemFnParam& param :
             node.payload.get<ast::ItemFn>().params) {
          visit_pattern(param.pattern);
        }
        visit_block(node.payload.get<ast::ItemFn>().body);
        pop_scope();
        break;
      }
      case ast::ItemKind::Intrinsic: {
        // Bodiless signatures: rename parameter patterns for
        // uniformity, though nothing references them.
        push_scope();
        for (const ast::ItemFnParam& param :
             node.payload.get<ast::ItemIntrinsic>().params) {
          visit_pattern(param.pattern);
        }
        pop_scope();
        break;
      }
      case ast::ItemKind::Static: {
        push_scope();
        visit_expr(node.payload.get<ast::ItemStatic>().init);
        pop_scope();
        break;
      }
      case ast::ItemKind::Const: {
        push_scope();
        visit_expr(node.payload.get<ast::ItemConst>().init);
        pop_scope();
        break;
      }
      case ast::ItemKind::Impl: {
        for (ast::ItemIdx method : node.payload.get<ast::ItemImpl>().methods) {
          visit_item(method);
        }
        break;
      }
      case ast::ItemKind::Spec: {
        // Signatures carry patterns but no bodies; renaming inside
        // them keeps the pass uniform with `Fn`.
        for (const ast::SpecMethod& method :
             node.payload.get<ast::ItemSpec>().methods) {
          push_scope();
          for (const ast::ItemFnParam& param : method.params) {
            visit_pattern(param.pattern);
          }
          pop_scope();
        }
        break;
      }
      case ast::ItemKind::Struct:
      case ast::ItemKind::Enum:
      case ast::ItemKind::Use: break;
    }
  }

  void visit_block(ast::BlockIdx block) {
    if (!block.is_valid() || ast.exhausted()) {
      return;
    }
    if (nesting_exhausted(ast.blocks[block].span)) {
      return;
    }
    const base::NestingScope scope(nesting_);
    const ast::Block& node = ast.blocks[block];
    push_scope();
    for (ast::StmtIdx stmt : node.statements) {
      visit_stmt(stmt);
    }
    if (node.value.is_valid()) {
      visit_expr(node.value);
    }
    pop_scope();
  }

  void visit_stmt(ast::StmtIdx stmt) {
    if (!stmt.is_valid() || ast.exhausted()) {
      return;
    }
    const ast::StmtNode& node = ast.stmts[stmt];
    switch (node.kind) {
      case ast::StmtKind::Decl: {
        // Initializers evaluate before the bindings they introduce, so
        // `x := x` reads the outer `x`.
        visit_expr(node.payload.get<ast::StmtDecl>().init);
        visit_pattern(node.payload.get<ast::StmtDecl>().pattern);
        break;
      }
      case ast::StmtKind::Reassign: {
        const ast::StmtReassign& reassign =
            node.payload.get<ast::StmtReassign>();
        if (reassign.compound) {
          ast::ExprNode binary;
          binary.kind = ast::ExprKind::Binary;
          binary.span = ast.stmts[stmt].span;
          binary.payload.set(ast::ExprBinary{
              .op = reassign.op,
              .lhs = reassign.place,
              .rhs = reassign.value,
          });
          ast::StmtNode& target = ast.stmts[stmt];
          target.payload.set(ast::StmtReassign{
              .place = target.payload.get<ast::StmtReassign>().place,
              .compound = false,
              .op = target.payload.get<ast::StmtReassign>().op,
              .value = ast.exprs.push_back(binary),
          });
        }
        visit_expr(reassign.place);
        visit_expr(ast.stmts[stmt].payload.get<ast::StmtReassign>().value);
        break;
      }
      case ast::StmtKind::Expr: {
        visit_expr(node.payload.get<ast::StmtExpr>().value);
        break;
      }
    }
  }

  void visit_pattern(ast::PatternIdx pattern) {
    if (!pattern.is_valid() || ast.exhausted()) {
      return;
    }
    if (nesting_exhausted(ast.patterns[pattern].span)) {
      return;
    }
    const base::NestingScope scope(nesting_);
    const ast::PatternNode& node = ast.patterns[pattern];
    switch (node.kind) {
      case ast::PatternKind::Wildcard: break;
      case ast::PatternKind::Ident: {
        ast::PatternNode& target = ast.patterns[pattern];
        target.payload.ident.name.name =
            declare_named(target.payload.ident.name);
        break;
      }
      case ast::PatternKind::MutIdent: {
        ast::PatternNode& target = ast.patterns[pattern];
        target.payload.mut_ident.name.name =
            declare_named(target.payload.mut_ident.name);
        break;
      }
      case ast::PatternKind::Literal: break;
      case ast::PatternKind::Tuple: {
        for (ast::PatternIdx element : node.payload.tuple.elements) {
          visit_pattern(element);
        }
        break;
      }
      case ast::PatternKind::Struct: {
        for (const ast::FieldPattern& field : node.payload.strukt.fields) {
          visit_pattern(field.pattern);
        }
        break;
      }
      case ast::PatternKind::Ref: {
        visit_pattern(node.payload.ref.inner);
        break;
      }
      case ast::PatternKind::Or: visit_or(pattern); break;
    }
  }

  void visit_or(ast::PatternIdx or_pattern) {
    if (!or_pattern.is_valid() || ast.exhausted()) {
      return;
    }
    const ast::PatternNode& node = ast.patterns[or_pattern];
    // The first alternative establishes the scope; the rest reuse its
    // mapping and must bind the identical set. Afterwards the mapping
    // merges into the current scope so siblings and bodies resolve.
    push_scope();
    visit_pattern(node.payload.or_pat.alternatives[0]);
    std::vector<Binding> first = std::move(current());
    pop_scope();
    for (usize i = 1; i < node.payload.or_pat.alternatives.size(); ++i) {
      push_scope();
      for (Binding binding : first) {
        binding.seen = false;
        binding.preseeded = true;
        current().push_back(binding);
      }
      visit_pattern(node.payload.or_pat.alternatives[i]);
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
        const u32 index =
            bag.emit<i18n::Key::AnalyzerOrPatternBindsDifferentNames>(
                diag::Severity::Error, diag::Stage::Parser,
                DiagCode::OrPatternMismatch, node.span);
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
        const u32 index = bag.emit<i18n::Key::ParserNameAlreadyBound>(
            diag::Severity::Error, diag::Stage::Parser, DiagCode::AlreadyBound,
            node.span, binding.orig);
        (void)index;
        continue;
      }
      current().push_back(binding);
    }
  }

  void visit_cond(ast::CondIdx cond, bool* pushed) {
    if (!cond.is_valid() || ast.exhausted()) {
      return;
    }
    const ast::Cond& node = ast.conds[cond];
    // Initializers evaluate outside the bindings they introduce.
    if (node.is_pattern) {
      visit_expr(node.init);
      push_scope();
      visit_pattern(node.pattern);
      *pushed = true;
      return;
    }
    *pushed = false;
    visit_expr(node.value);
  }

  void visit_expr(ast::ExprIdx expr) {
    if (!expr.is_valid() || ast.exhausted()) {
      return;
    }
    if (nesting_exhausted(ast.exprs[expr].span)) {
      return;
    }
    const base::NestingScope scope(nesting_);
    const ast::ExprNode& node = ast.exprs[expr];
    switch (node.kind) {
      case ast::ExprKind::Literal: break;
      case ast::ExprKind::Path: {
        const ast::PathIdx path = node.payload.get<ast::ExprPath>().idx;
        if (ast.paths[path].segments.size() == 1) {
          const ast::Ident& first = ast.paths[path].segments[0];
          const std::string_view resolved = lookup(first.name);
          if (resolved != first.name) {
            path_scratch.clear();
            for (const ast::Ident& segment : ast.paths[path].segments) {
              path_scratch.push_back(segment);
            }
            path_scratch[0].name = resolved;
            ast::Path renamed;
            renamed.segments = ast::copy_to_arena(ast.spans, path_scratch);
            renamed.span = ast.paths[path].span;
            ast.exprs[expr].payload.set(ast::ExprPath{
                .idx = ast.paths.push_back(renamed),
                .type_args =
                    ast.exprs[expr].payload.get<ast::ExprPath>().type_args,
            });
          }
        }
        break;
      }
      case ast::ExprKind::Struct: {
        for (const ast::ExprFieldInit& field :
             node.payload.get<ast::ExprStruct>().init) {
          visit_expr(field.value);
        }
        if (node.payload.get<ast::ExprStruct>().base_expr.is_valid()) {
          visit_expr(node.payload.get<ast::ExprStruct>().base_expr);
        }
        break;
      }
      case ast::ExprKind::Tuple: {
        for (ast::ExprIdx element :
             node.payload.get<ast::ExprTuple>().elements) {
          visit_expr(element);
        }
        break;
      }
      case ast::ExprKind::Array: {
        const ast::ExprArray& array = node.payload.get<ast::ExprArray>();
        for (ast::ExprIdx element : array.elements) {
          visit_expr(element);
        }
        if (array.repeat.is_valid()) {
          visit_expr(array.repeat);
        }
        break;
      }
      case ast::ExprKind::Unary: {
        visit_expr(node.payload.get<ast::ExprUnary>().inner);
        break;
      }
      case ast::ExprKind::Borrow: {
        visit_expr(node.payload.get<ast::ExprBorrow>().inner);
        break;
      }
      case ast::ExprKind::Deref: {
        visit_expr(node.payload.get<ast::ExprDeref>().inner);
        break;
      }
      case ast::ExprKind::Binary: {
        visit_expr(node.payload.get<ast::ExprBinary>().lhs);
        visit_expr(node.payload.get<ast::ExprBinary>().rhs);
        break;
      }
      case ast::ExprKind::Cast: {
        visit_expr(node.payload.get<ast::ExprCast>().inner);
        break;
      }
      case ast::ExprKind::Call: {
        visit_expr(node.payload.get<ast::ExprCall>().callee);
        for (ast::ExprIdx arg : node.payload.get<ast::ExprCall>().args) {
          visit_expr(arg);
        }
        break;
      }
      case ast::ExprKind::MethodCall: {
        visit_expr(node.payload.get<ast::ExprMethodCall>().receiver);
        for (ast::ExprIdx arg : node.payload.get<ast::ExprMethodCall>().args) {
          visit_expr(arg);
        }
        break;
      }
      case ast::ExprKind::Field: {
        visit_expr(node.payload.get<ast::ExprField>().receiver);
        break;
      }
      case ast::ExprKind::Index: {
        visit_expr(node.payload.get<ast::ExprIndex>().receiver);
        visit_expr(node.payload.get<ast::ExprIndex>().index);
        break;
      }
      case ast::ExprKind::Question: {
        visit_expr(node.payload.get<ast::ExprQuestion>().inner);
        break;
      }
      case ast::ExprKind::If: {
        bool pushed = false;
        visit_cond(node.payload.get<ast::ExprIf>().cond, &pushed);
        visit_block(node.payload.get<ast::ExprIf>().then_block);
        if (pushed) {
          pop_scope();
        }
        if (node.payload.get<ast::ExprIf>().else_block.is_valid()) {
          visit_block(node.payload.get<ast::ExprIf>().else_block);
        }
        break;
      }
      case ast::ExprKind::Match: {
        visit_expr(node.payload.get<ast::ExprMatch>().scrutinee);
        for (const ast::ExprMatchArm& arm :
             node.payload.get<ast::ExprMatch>().arms) {
          push_scope();
          visit_pattern(arm.pattern);
          visit_expr(arm.body);
          pop_scope();
        }
        break;
      }
      case ast::ExprKind::Loop: {
        visit_block(node.payload.get<ast::ExprLoop>().body);
        break;
      }
      case ast::ExprKind::While: {
        bool pushed = false;
        visit_cond(node.payload.get<ast::ExprWhile>().cond, &pushed);
        visit_block(node.payload.get<ast::ExprWhile>().body);
        if (pushed) {
          pop_scope();
        }
        break;
      }
      case ast::ExprKind::Block: {
        visit_block(node.payload.get<ast::ExprBlock>().block);
        break;
      }
      case ast::ExprKind::Return: {
        visit_expr(node.payload.get<ast::ExprReturn>().value);
        break;
      }
      case ast::ExprKind::Range: {
        visit_expr(node.payload.get<ast::ExprRange>().start);
        visit_expr(node.payload.get<ast::ExprRange>().end);
        break;
      }
      case ast::ExprKind::Closure: {
        // Captures resolve like uses do: a shadowed binding answers to
        // its fresh spelling, so the list is rewritten the same way,
        // before the closure's own scope opens. A capture names a
        // local that already exists, so nothing is declared here.
        for (ast::Capture& capture :
             node.payload.get<ast::ExprClosure>().captures) {
          capture.name.name = lookup(capture.name.name);
        }
        // Parameters bind like `fn` parameters. A wildcard binds
        // nothing, so two of them never collide.
        push_scope();
        for (ast::ClosureParam& param :
             node.payload.get<ast::ExprClosure>().params) {
          if (param.is_wildcard) {
            continue;
          }
          ast::Ident name = param.name;
          name.name = declare_named(param.name);
          param.name = name;
        }
        visit_expr(node.payload.get<ast::ExprClosure>().body);
        pop_scope();
        break;
      }
      case ast::ExprKind::Break:
      case ast::ExprKind::Continue: break;
    }
  }
};

}  // namespace

void desugar_shadowing(std::span<const ast::ItemIdx> items,
                       ast::AstArena& ast,
                       diag::DiagBag& bag) {
  Desugarer desugar{ast, bag};
  desugar.run(items);
}

}  // namespace parser
