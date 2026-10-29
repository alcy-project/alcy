// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linter/bindings.h"

#include <string_view>
#include <vector>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "linter/context.h"

namespace linter {

namespace {

// Tracks every name a function declares through a stack of scopes. A
// binding is reported when the scope that owns it closes, so the
// counters a rule reads are final.
//
// The traversal mirrors walk.cc but carries the scope stack, which the
// tree walker does not; the two are separate because a rule that only
// inspects a node does not want the bookkeeping and a rule that resolves
// names cannot do without it.
class Analyzer {
 public:
  Analyzer(LintContext& ctx, BindingIndex& out) : ctx_(ctx), out_(out) {}

  void run_item(ast::ItemIdx item) {
    if (!item.is_valid()) {
      return;
    }
    const ast::ItemNode& node = ctx_.ast.items[item];
    using K = ast::ItemKind;
    switch (node.kind) {
      case K::Fn: run_fn(node.payload.get<ast::ItemFn>()); break;
      case K::Impl: {
        const ast::ItemImpl& impl = node.payload.get<ast::ItemImpl>();
        for (const ast::ItemIdx method : impl.methods) {
          run_item(method);
        }
        break;
      }
      case K::Static: run_expr(node.payload.get<ast::ItemStatic>().init); break;
      case K::Const: run_expr(node.payload.get<ast::ItemConst>().init); break;
      default: break;
    }
  }

 private:
  void push_scope() { scopes_.emplace_back(); }

  void pop_scope() {
    if (scopes_.empty()) {
      return;
    }
    for (const u32 index : scopes_.back()) {
      out_.bindings.push_back(bindings_[index]);
    }
    scopes_.pop_back();
  }

  void declare(std::string_view name,
               diag::Span span,
               bool is_mut,
               bool is_param) {
    if (scopes_.empty()) {
      push_scope();
    }
    BindingUse binding;
    binding.name = name;
    binding.span = span;
    binding.is_mut = is_mut;
    binding.is_param = is_param;
    // The desugarer mints a suffixed spelling for a binding that shadows
    // one in an enclosing scope and leaves every other spelling alone,
    // so the suffix is exactly the shadow signal. See parser/desugar.h.
    binding.is_shadowed = name.find('$') != std::string_view::npos;
    const u32 index = static_cast<u32>(bindings_.size());
    bindings_.push_back(binding);
    scopes_.back().push_back(index);
  }

  // Records a name the body mentions. Search runs innermost scope first
  // and later declarations before earlier ones, so a freshened spelling
  // resolves to its own binding and a plain one to the nearest.
  void resolve(std::string_view name, bool read, bool mutation) {
    // Later declarations are nearer, so the search walks the scopes and
    // each scope's entries backwards; an index keeps that explicit.
    for (usize s = scopes_.size(); s > 0; --s) {
      const std::vector<u32>& scope = scopes_[s - 1];
      for (usize i = scope.size(); i > 0; --i) {
        BindingUse& binding = bindings_[scope[i - 1]];
        if (binding.name != name) {
          continue;
        }
        if (read) {
          ++binding.reads;
        }
        if (mutation) {
          ++binding.mutations;
        }
        return;
      }
    }
  }

  void declare_pattern(ast::PatternIdx pattern, bool is_param) {
    if (!pattern.is_valid()) {
      return;
    }
    const ast::PatternNode& node = ctx_.ast.patterns[pattern];
    using K = ast::PatternKind;
    switch (node.kind) {
      case K::Wildcard:
      case K::Literal: break;
      case K::Ident:
        declare(node.payload.ident.name.name, node.payload.ident.name.span,
                false, is_param);
        break;
      case K::MutIdent:
        declare(node.payload.mut_ident.name.name,
                node.payload.mut_ident.name.span, true, is_param);
        break;
      case K::Tuple:
        for (const ast::PatternIdx element : node.payload.tuple.elements) {
          declare_pattern(element, is_param);
        }
        break;
      case K::Struct:
        for (const ast::FieldPattern& field : node.payload.strukt.fields) {
          declare_pattern(field.pattern, is_param);
        }
        break;
      case K::Ref: declare_pattern(node.payload.ref.inner, is_param); break;
      case K::Or: {
        // Every alternative binds the same set, so one declaration
        // stands for the whole pattern.
        const ast::PatternOr& alternatives = node.payload.or_pat;
        if (!alternatives.alternatives.empty()) {
          declare_pattern(alternatives.alternatives[0], is_param);
        }
        break;
      }
    }
  }

  // The innermost place a path expression names, unwrapping the
  // projections a place can carry. Returns an invalid index when the
  // expression is not a place rooted at a local.
  ast::PathIdx base_place_path(ast::ExprIdx expr) const {
    while (expr.is_valid()) {
      const ast::ExprNode& node = ctx_.ast.exprs[expr];
      using K = ast::ExprKind;
      switch (node.kind) {
        case K::Path: return node.payload.get<ast::ExprPath>().idx;
        case K::Field:
          expr = node.payload.get<ast::ExprField>().receiver;
          break;
        case K::Index:
          expr = node.payload.get<ast::ExprIndex>().receiver;
          break;
        case K::Deref: expr = node.payload.get<ast::ExprDeref>().inner; break;
        default: return ast::PathIdx::invalid();
      }
    }
    return ast::PathIdx::invalid();
  }

  void resolve_place(ast::ExprIdx place, bool mutation) {
    const ast::PathIdx path = base_place_path(place);
    if (!path.is_valid()) {
      return;
    }
    const ast::Path& segments = ctx_.ast.paths[path];
    if (segments.segments.size() == 1) {
      resolve(segments.segments[0].name, /*read=*/true, mutation);
    }
  }

  void run_fn(const ast::ItemFn& fn) {
    push_scope();
    for (const ast::ItemFnParam& param : fn.params) {
      declare_pattern(param.pattern, /*is_param=*/true);
    }
    run_block(fn.body);
    pop_scope();
  }

  void run_block(ast::BlockIdx block) {
    if (!block.is_valid()) {
      return;
    }
    push_scope();
    const ast::Block& node = ctx_.ast.blocks[block];
    for (const ast::StmtIdx stmt : node.statements) {
      run_stmt(stmt);
    }
    run_expr(node.value);
    pop_scope();
  }

  void run_stmt(ast::StmtIdx stmt) {
    if (!stmt.is_valid()) {
      return;
    }
    const ast::StmtNode& node = ctx_.ast.stmts[stmt];
    using K = ast::StmtKind;
    switch (node.kind) {
      case K::Decl: {
        const ast::StmtDecl& decl = node.payload.get<ast::StmtDecl>();
        // The initializer runs before the name is bound, so an
        // initializer that names the binding reads the outer one.
        run_expr(decl.init);
        declare_pattern(decl.pattern, /*is_param=*/false);
        break;
      }
      case K::Reassign: {
        const ast::StmtReassign& reassign =
            node.payload.get<ast::StmtReassign>();
        run_expr(reassign.place);
        resolve_place(reassign.place, /*mutation=*/true);
        run_expr(reassign.value);
        break;
      }
      case K::Expr: run_expr(node.payload.get<ast::StmtExpr>().value); break;
    }
  }

  void run_cond_value(ast::CondIdx cond) {
    if (!cond.is_valid()) {
      return;
    }
    run_expr(ctx_.ast.conds[cond].value);
  }

  void run_expr(ast::ExprIdx expr) {
    if (!expr.is_valid()) {
      return;
    }
    const ast::ExprNode& node = ctx_.ast.exprs[expr];
    using K = ast::ExprKind;
    switch (node.kind) {
      case K::Literal:
      case K::Break:
      case K::Continue: break;
      case K::Path: {
        const ast::Path& path =
            ctx_.ast.paths[node.payload.get<ast::ExprPath>().idx];
        if (path.segments.size() == 1) {
          resolve(path.segments[0].name, /*read=*/true, /*mutation=*/false);
        }
        break;
      }
      case K::Struct: {
        const ast::ExprStruct& e = node.payload.get<ast::ExprStruct>();
        for (const ast::ExprFieldInit& field : e.init) {
          run_expr(field.value);
        }
        run_expr(e.base_expr);
        break;
      }
      case K::Tuple:
        for (const ast::ExprIdx element :
             node.payload.get<ast::ExprTuple>().elements) {
          run_expr(element);
        }
        break;
      case K::Array: {
        const ast::ExprArray& e = node.payload.get<ast::ExprArray>();
        for (const ast::ExprIdx element : e.elements) {
          run_expr(element);
        }
        run_expr(e.repeat);
        break;
      }
      case K::Unary: run_expr(node.payload.get<ast::ExprUnary>().inner); break;
      case K::Borrow: {
        const ast::ExprBorrow& e = node.payload.get<ast::ExprBorrow>();
        run_expr(e.inner);
        if (e.is_mut) {
          resolve_place(e.inner, /*mutation=*/true);
        }
        break;
      }
      case K::Deref: run_expr(node.payload.get<ast::ExprDeref>().inner); break;
      case K::Binary: {
        const ast::ExprBinary& e = node.payload.get<ast::ExprBinary>();
        run_expr(e.lhs);
        run_expr(e.rhs);
        break;
      }
      case K::Cast: run_expr(node.payload.get<ast::ExprCast>().inner); break;
      case K::Call: {
        const ast::ExprCall& e = node.payload.get<ast::ExprCall>();
        run_expr(e.callee);
        for (const ast::ExprIdx arg : e.args) {
          run_expr(arg);
        }
        break;
      }
      case K::MethodCall: {
        const ast::ExprMethodCall& e = node.payload.get<ast::ExprMethodCall>();
        run_expr(e.receiver);
        for (const ast::ExprIdx arg : e.args) {
          run_expr(arg);
        }
        // A method may take its receiver newly exclusively, and the
        // syntax does not say; counting the receiver as mutated keeps
        // unused-mut from flagging a binding only written by a method.
        resolve_place(e.receiver, /*mutation=*/true);
        break;
      }
      case K::Field:
        run_expr(node.payload.get<ast::ExprField>().receiver);
        break;
      case K::Index: {
        const ast::ExprIndex& e = node.payload.get<ast::ExprIndex>();
        run_expr(e.receiver);
        run_expr(e.index);
        break;
      }
      case K::Question:
        run_expr(node.payload.get<ast::ExprQuestion>().inner);
        break;
      case K::If: {
        const ast::ExprIf& e = node.payload.get<ast::ExprIf>();
        if (e.cond.is_valid() && ctx_.ast.conds[e.cond].is_pattern) {
          const ast::Cond& cond = ctx_.ast.conds[e.cond];
          run_expr(cond.init);
          push_scope();
          declare_pattern(cond.pattern, /*is_param=*/false);
          run_block(e.then_block);
          pop_scope();
        } else {
          run_cond_value(e.cond);
          run_block(e.then_block);
        }
        run_block(e.else_block);
        break;
      }
      case K::Match: {
        const ast::ExprMatch& e = node.payload.get<ast::ExprMatch>();
        run_expr(e.scrutinee);
        for (const ast::ExprMatchArm& arm : e.arms) {
          push_scope();
          declare_pattern(arm.pattern, /*is_param=*/false);
          run_expr(arm.body);
          run_expr(arm.guard);
          pop_scope();
        }
        break;
      }
      case K::Loop: run_block(node.payload.get<ast::ExprLoop>().body); break;
      case K::While: {
        const ast::ExprWhile& e = node.payload.get<ast::ExprWhile>();
        if (e.cond.is_valid() && ctx_.ast.conds[e.cond].is_pattern) {
          const ast::Cond& cond = ctx_.ast.conds[e.cond];
          run_expr(cond.init);
          push_scope();
          declare_pattern(cond.pattern, /*is_param=*/false);
          run_block(e.body);
          pop_scope();
        } else {
          run_cond_value(e.cond);
          run_block(e.body);
        }
        break;
      }
      case K::Block: run_block(node.payload.get<ast::ExprBlock>().block); break;
      case K::Return:
        run_expr(node.payload.get<ast::ExprReturn>().value);
        break;
      case K::Range: {
        const ast::ExprRange& e = node.payload.get<ast::ExprRange>();
        run_expr(e.start);
        run_expr(e.end);
        break;
      }
      case K::Closure: {
        const ast::ExprClosure& e = node.payload.get<ast::ExprClosure>();
        // A capture is how the closure names what it took, and naming it
        // is using it.
        for (const ast::Capture& capture : e.captures) {
          resolve(capture.name.name, /*read=*/true, /*mutation=*/false);
        }
        push_scope();
        for (const ast::ClosureParam& param : e.params) {
          if (param.is_wildcard) {
            continue;
          }
          declare(param.name.name, param.name.span, param.is_mut,
                  /*is_param=*/true);
        }
        run_expr(e.body);
        pop_scope();
        break;
      }
    }
  }

  LintContext& ctx_;
  BindingIndex& out_;
  std::vector<BindingUse> bindings_;
  std::vector<std::vector<u32>> scopes_;
};

}  // namespace

void collect_bindings(LintContext& ctx, BindingIndex& index) {
  index.bindings.clear();
  for (const ast::ItemIdx item : ctx.module_node().items) {
    Analyzer analyzer(ctx, index);
    analyzer.run_item(item);
  }
}

}  // namespace linter
