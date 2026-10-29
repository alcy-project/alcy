// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "linter/walk.h"

#include <span>

#include "analyzer/resolve.h"
#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "linter/context.h"

namespace linter {

namespace {

// The traversal itself. The declarations live here rather than in the
// header for the same reason the schedule does: a rule names the node
// kind it reads and the walk decides how to reach it.
void walk_exprs(LintContext& ctx,
                std::span<const ast::ExprIdx> exprs,
                const Visitor& visitor);
void walk_cond(LintContext& ctx, ast::CondIdx cond, const Visitor& visitor);
void walk_block(LintContext& ctx, ast::BlockIdx block, const Visitor& visitor);
void walk_expr(LintContext& ctx, ast::ExprIdx expr, const Visitor& visitor);
void walk_stmt(LintContext& ctx, ast::StmtIdx stmt, const Visitor& visitor);
void walk_pattern(LintContext& ctx,
                  ast::PatternIdx pattern,
                  const Visitor& visitor);
void walk_type(LintContext& ctx, ast::TypeIdx type, const Visitor& visitor);
void walk_generic(LintContext& ctx,
                  std::span<const ast::GenericParam> params,
                  const Visitor& visitor);
void walk_item(LintContext& ctx, ast::ItemIdx item, const Visitor& visitor);
void walk_items(LintContext& ctx,
                std::span<const ast::ItemIdx> items,
                const Visitor& visitor);

void call_expr(const Visitor& visitor, LintContext& ctx, ast::ExprIdx id) {
  if (visitor.expr != nullptr) {
    visitor.expr(ctx, id);
  }
}

void call_path(const Visitor& visitor, LintContext& ctx, ast::PathIdx id) {
  if (id.is_valid() && visitor.path != nullptr) {
    visitor.path(ctx, id);
  }
}

void walk_exprs(LintContext& ctx,
                std::span<const ast::ExprIdx> exprs,
                const Visitor& visitor) {
  for (const ast::ExprIdx expr : exprs) {
    walk_expr(ctx, expr, visitor);
  }
}

void walk_cond(LintContext& ctx, ast::CondIdx cond, const Visitor& visitor) {
  if (!cond.is_valid()) {
    return;
  }
  const ast::Cond& node = ctx.ast.conds[cond];
  if (node.is_pattern) {
    walk_pattern(ctx, node.pattern, visitor);
    walk_expr(ctx, node.init, visitor);
    return;
  }
  walk_expr(ctx, node.value, visitor);
}

void walk_block(LintContext& ctx, ast::BlockIdx block, const Visitor& visitor) {
  if (!block.is_valid()) {
    return;
  }
  if (visitor.block != nullptr) {
    visitor.block(ctx, block);
  }
  const ast::Block& node = ctx.ast.blocks[block];
  for (const ast::StmtIdx stmt : node.statements) {
    walk_stmt(ctx, stmt, visitor);
  }
  walk_expr(ctx, node.value, visitor);
}

void walk_expr(LintContext& ctx, ast::ExprIdx expr, const Visitor& visitor) {
  if (!expr.is_valid()) {
    return;
  }
  const ast::ExprNode& node = ctx.ast.exprs[expr];
  call_expr(visitor, ctx, expr);
  using K = ast::ExprKind;
  switch (node.kind) {
    case K::Literal:
    case K::Break:
    case K::Continue: break;
    case K::Path: {
      const ast::ExprPath& e = node.payload.get<ast::ExprPath>();
      call_path(visitor, ctx, e.idx);
      for (const ast::TypeIdx arg : e.type_args) {
        walk_type(ctx, arg, visitor);
      }
      break;
    }
    case K::Struct: {
      const ast::ExprStruct& e = node.payload.get<ast::ExprStruct>();
      call_path(visitor, ctx, e.path);
      for (const ast::TypeIdx arg : e.type_args) {
        walk_type(ctx, arg, visitor);
      }
      for (const ast::ExprFieldInit& field : e.init) {
        walk_expr(ctx, field.value, visitor);
      }
      walk_expr(ctx, e.base_expr, visitor);
      break;
    }
    case K::Tuple: {
      walk_exprs(ctx, node.payload.get<ast::ExprTuple>().elements, visitor);
      break;
    }
    case K::Array: {
      const ast::ExprArray& e = node.payload.get<ast::ExprArray>();
      walk_exprs(ctx, e.elements, visitor);
      walk_expr(ctx, e.repeat, visitor);
      call_path(visitor, ctx, e.count_path);
      break;
    }
    case K::Unary:
      walk_expr(ctx, node.payload.get<ast::ExprUnary>().inner, visitor);
      break;
    case K::Borrow:
      walk_expr(ctx, node.payload.get<ast::ExprBorrow>().inner, visitor);
      break;
    case K::Deref:
      walk_expr(ctx, node.payload.get<ast::ExprDeref>().inner, visitor);
      break;
    case K::Binary: {
      const ast::ExprBinary& e = node.payload.get<ast::ExprBinary>();
      walk_expr(ctx, e.lhs, visitor);
      walk_expr(ctx, e.rhs, visitor);
      break;
    }
    case K::Cast: {
      const ast::ExprCast& e = node.payload.get<ast::ExprCast>();
      walk_expr(ctx, e.inner, visitor);
      walk_type(ctx, e.type, visitor);
      break;
    }
    case K::Call: {
      const ast::ExprCall& e = node.payload.get<ast::ExprCall>();
      walk_expr(ctx, e.callee, visitor);
      walk_exprs(ctx, e.args, visitor);
      for (const ast::TypeIdx arg : e.type_args) {
        walk_type(ctx, arg, visitor);
      }
      break;
    }
    case K::MethodCall: {
      const ast::ExprMethodCall& e = node.payload.get<ast::ExprMethodCall>();
      walk_expr(ctx, e.receiver, visitor);
      walk_exprs(ctx, e.args, visitor);
      for (const ast::TypeIdx arg : e.type_args) {
        walk_type(ctx, arg, visitor);
      }
      break;
    }
    case K::Field:
      walk_expr(ctx, node.payload.get<ast::ExprField>().receiver, visitor);
      break;
    case K::Index: {
      const ast::ExprIndex& e = node.payload.get<ast::ExprIndex>();
      walk_expr(ctx, e.receiver, visitor);
      walk_expr(ctx, e.index, visitor);
      break;
    }
    case K::Question:
      walk_expr(ctx, node.payload.get<ast::ExprQuestion>().inner, visitor);
      break;
    case K::If: {
      const ast::ExprIf& e = node.payload.get<ast::ExprIf>();
      walk_cond(ctx, e.cond, visitor);
      walk_block(ctx, e.then_block, visitor);
      walk_block(ctx, e.else_block, visitor);
      break;
    }
    case K::Match: {
      const ast::ExprMatch& e = node.payload.get<ast::ExprMatch>();
      walk_expr(ctx, e.scrutinee, visitor);
      for (const ast::ExprMatchArm& arm : e.arms) {
        walk_pattern(ctx, arm.pattern, visitor);
        walk_expr(ctx, arm.body, visitor);
        walk_expr(ctx, arm.guard, visitor);
      }
      break;
    }
    case K::Loop:
      walk_block(ctx, node.payload.get<ast::ExprLoop>().body, visitor);
      break;
    case K::While: {
      const ast::ExprWhile& e = node.payload.get<ast::ExprWhile>();
      walk_cond(ctx, e.cond, visitor);
      walk_block(ctx, e.body, visitor);
      break;
    }
    case K::Block:
      walk_block(ctx, node.payload.get<ast::ExprBlock>().block, visitor);
      break;
    case K::Return:
      walk_expr(ctx, node.payload.get<ast::ExprReturn>().value, visitor);
      break;
    case K::Range: {
      const ast::ExprRange& e = node.payload.get<ast::ExprRange>();
      walk_expr(ctx, e.start, visitor);
      walk_expr(ctx, e.end, visitor);
      break;
    }
    case K::Closure: {
      const ast::ExprClosure& e = node.payload.get<ast::ExprClosure>();
      for (const ast::ClosureParam& param : e.params) {
        walk_type(ctx, param.type, visitor);
      }
      walk_expr(ctx, e.body, visitor);
      break;
    }
  }
}

void walk_stmt(LintContext& ctx, ast::StmtIdx stmt, const Visitor& visitor) {
  if (!stmt.is_valid()) {
    return;
  }
  if (visitor.stmt != nullptr) {
    visitor.stmt(ctx, stmt);
  }
  const ast::StmtNode& node = ctx.ast.stmts[stmt];
  using K = ast::StmtKind;
  switch (node.kind) {
    case K::Decl: {
      const ast::StmtDecl& d = node.payload.get<ast::StmtDecl>();
      walk_pattern(ctx, d.pattern, visitor);
      walk_type(ctx, d.type, visitor);
      walk_expr(ctx, d.init, visitor);
      break;
    }
    case K::Reassign: {
      const ast::StmtReassign& r = node.payload.get<ast::StmtReassign>();
      walk_expr(ctx, r.place, visitor);
      walk_expr(ctx, r.value, visitor);
      break;
    }
    case K::Expr:
      walk_expr(ctx, node.payload.get<ast::StmtExpr>().value, visitor);
      break;
  }
}

void walk_pattern(LintContext& ctx,
                  ast::PatternIdx pattern,
                  const Visitor& visitor) {
  if (!pattern.is_valid()) {
    return;
  }
  if (visitor.pattern != nullptr) {
    visitor.pattern(ctx, pattern);
  }
  const ast::PatternNode& node = ctx.ast.patterns[pattern];
  using K = ast::PatternKind;
  switch (node.kind) {
    case K::Wildcard:
    case K::Ident:
    case K::MutIdent:
    case K::Literal: break;
    case K::Tuple: {
      const ast::PatternTuple& p = node.payload.tuple;
      call_path(visitor, ctx, p.path);
      for (const ast::PatternIdx element : p.elements) {
        walk_pattern(ctx, element, visitor);
      }
      break;
    }
    case K::Struct: {
      const ast::PatternStruct& p = node.payload.strukt;
      call_path(visitor, ctx, p.path);
      for (const ast::FieldPattern& field : p.fields) {
        walk_pattern(ctx, field.pattern, visitor);
      }
      break;
    }
    case K::Ref: walk_pattern(ctx, node.payload.ref.inner, visitor); break;
    case K::Or: {
      const ast::PatternOr& p = node.payload.or_pat;
      for (const ast::PatternIdx alternative : p.alternatives) {
        walk_pattern(ctx, alternative, visitor);
      }
      break;
    }
  }
}

void walk_type(LintContext& ctx, ast::TypeIdx type, const Visitor& visitor) {
  if (!type.is_valid()) {
    return;
  }
  if (visitor.type != nullptr) {
    visitor.type(ctx, type);
  }
  const ast::TypeNode& node = ctx.ast.types[type];
  using K = ast::TypeKind;
  switch (node.kind) {
    case K::Primitive:
    case K::Unit:
    case K::Never:
    case K::Str:
    case K::Value: break;
    case K::Tuple: {
      for (const ast::TypeIdx element :
           node.payload.get<ast::TypeTuple>().elements) {
        walk_type(ctx, element, visitor);
      }
      break;
    }
    case K::Array: {
      const ast::TypeArray& t = node.payload.get<ast::TypeArray>();
      walk_type(ctx, t.element, visitor);
      call_path(visitor, ctx, t.length_path);
      break;
    }
    case K::Slice:
      walk_type(ctx, node.payload.get<ast::TypeSlice>().element, visitor);
      break;
    case K::Path: {
      const ast::TypePath& t = node.payload.get<ast::TypePath>();
      call_path(visitor, ctx, t.path);
      for (const ast::TypeIdx arg : t.args) {
        walk_type(ctx, arg, visitor);
      }
      break;
    }
    case K::Ref:
      walk_type(ctx, node.payload.get<ast::TypeRef>().inner, visitor);
      break;
    case K::RawPtr:
      walk_type(ctx, node.payload.get<ast::TypeRawPtr>().inner, visitor);
      break;
    // An owning function type shares the borrowing one's payload.
    case K::Func:
    case K::OwnFunc: {
      const ast::TypeFunc& t = node.payload.get<ast::TypeFunc>();
      for (const ast::TypeIdx param : t.params) {
        walk_type(ctx, param, visitor);
      }
      walk_type(ctx, t.ret, visitor);
      break;
    }
  }
}

void walk_generic(LintContext& ctx,
                  std::span<const ast::GenericParam> params,
                  const Visitor& visitor) {
  for (const ast::GenericParam& param : params) {
    walk_type(ctx, param.type, visitor);
    for (const ast::TypeIdx bound : param.bounds) {
      walk_type(ctx, bound, visitor);
    }
  }
}

void walk_item(LintContext& ctx, ast::ItemIdx item, const Visitor& visitor) {
  if (!item.is_valid()) {
    return;
  }
  if (visitor.item != nullptr) {
    visitor.item(ctx, item);
  }
  const ast::ItemNode& node = ctx.ast.items[item];
  using K = ast::ItemKind;
  switch (node.kind) {
    case K::Fn: {
      const ast::ItemFn& f = node.payload.get<ast::ItemFn>();
      walk_generic(ctx, f.generic, visitor);
      for (const ast::ItemFnParam& param : f.params) {
        walk_pattern(ctx, param.pattern, visitor);
        walk_type(ctx, param.type, visitor);
      }
      walk_type(ctx, f.return_type, visitor);
      walk_block(ctx, f.body, visitor);
      break;
    }
    case K::Impl: {
      const ast::ItemImpl& impl = node.payload.get<ast::ItemImpl>();
      walk_generic(ctx, impl.params, visitor);
      walk_type(ctx, impl.type, visitor);
      walk_type(ctx, impl.spec, visitor);
      for (const ast::ItemIdx method : impl.methods) {
        walk_item(ctx, method, visitor);
      }
      break;
    }
    case K::Static: {
      const ast::ItemStatic& s = node.payload.get<ast::ItemStatic>();
      walk_type(ctx, s.type, visitor);
      walk_expr(ctx, s.init, visitor);
      break;
    }
    case K::Const: {
      const ast::ItemConst& c = node.payload.get<ast::ItemConst>();
      walk_type(ctx, c.type, visitor);
      walk_expr(ctx, c.init, visitor);
      break;
    }
    case K::Struct: {
      const ast::ItemStruct& s = node.payload.get<ast::ItemStruct>();
      walk_generic(ctx, s.params, visitor);
      for (const ast::ItemStructField& field : s.fields) {
        walk_type(ctx, field.type, visitor);
      }
      break;
    }
    case K::Enum: {
      const ast::ItemEnum& e = node.payload.get<ast::ItemEnum>();
      walk_generic(ctx, e.params, visitor);
      for (const ast::ItemEnumVariant& variant : e.variants) {
        for (const ast::TypeIdx field : variant.fields) {
          walk_type(ctx, field, visitor);
        }
      }
      break;
    }
    case K::Spec: {
      const ast::ItemSpec& s = node.payload.get<ast::ItemSpec>();
      walk_generic(ctx, s.params, visitor);
      walk_type(ctx, s.super, visitor);
      for (const ast::SpecMethod& method : s.methods) {
        walk_generic(ctx, method.generic, visitor);
        for (const ast::ItemFnParam& param : method.params) {
          walk_pattern(ctx, param.pattern, visitor);
          walk_type(ctx, param.type, visitor);
        }
        walk_type(ctx, method.return_type, visitor);
      }
      break;
    }
    case K::Intrinsic: {
      const ast::ItemIntrinsic& f = node.payload.get<ast::ItemIntrinsic>();
      walk_generic(ctx, f.generic, visitor);
      for (const ast::ItemFnParam& param : f.params) {
        walk_pattern(ctx, param.pattern, visitor);
        walk_type(ctx, param.type, visitor);
      }
      walk_type(ctx, f.return_type, visitor);
      break;
    }
    case K::Extern: {
      const ast::ItemExtern& e = node.payload.get<ast::ItemExtern>();
      for (const ast::ItemExternFn& fn : e.fns) {
        walk_generic(ctx, fn.generic, visitor);
        for (const ast::ItemFnParam& param : fn.params) {
          walk_pattern(ctx, param.pattern, visitor);
          walk_type(ctx, param.type, visitor);
        }
        walk_type(ctx, fn.return_type, visitor);
      }
      break;
    }
    case K::Use: break;
  }
}

void walk_items(LintContext& ctx,
                std::span<const ast::ItemIdx> items,
                const Visitor& visitor) {
  for (const ast::ItemIdx item : items) {
    walk_item(ctx, item, visitor);
  }
}

}  // namespace

void walk_module(LintContext& ctx, const Visitor& visitor) {
  const analyzer::ModuleNode& node = *ctx.package.tree.modules[ctx.module];
  walk_items(ctx, node.items, visitor);
}

}  // namespace linter
