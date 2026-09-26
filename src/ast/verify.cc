// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/verify.h"

#include <string_view>

#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace ast {

namespace {

// An invalid index is an allowed absent edge; anything else must
// name a node in a table of `size` entries.
template <typename I>
bool bound(I idx, usize size) {
  return !idx.is_valid() || static_cast<usize>(idx.idx) < size;
}

bool verify_cond_children(const Cond& cond, const AstArena& arena) {
  return bound(cond.pattern, arena.patterns.size()) &&
         bound(cond.init, arena.exprs.size()) &&
         bound(cond.value, arena.exprs.size());
}

bool verify_block_children(const Block& block, const AstArena& arena) {
  for (const StmtIdx stmt : block.statements) {
    if (!bound(stmt, arena.stmts.size())) {
      return false;
    }
  }
  return bound(block.value, arena.exprs.size());
}

bool verify_type_children(const TypeNode& node, const AstArena& arena) {
  switch (node.kind) {
    case TypeKind::Primitive:
    case TypeKind::Unit:
    case TypeKind::Never:
    case TypeKind::Str: return true;
    case TypeKind::Tuple: {
      const TypeTuple tuple = node.payload.get<TypeTuple>();
      for (const TypeIdx element : tuple.elements) {
        if (!bound(element, arena.types.size())) {
          return false;
        }
      }
      return true;
    }
    case TypeKind::Array: {
      const TypeArray array = node.payload.get<TypeArray>();
      return bound(array.element, arena.types.size());
    }
    case TypeKind::Path: {
      const TypePath path = node.payload.get<TypePath>();
      if (!bound(path.path, arena.paths.size())) {
        return false;
      }
      for (const TypeIdx arg : path.args) {
        if (!bound(arg, arena.types.size())) {
          return false;
        }
      }
      return true;
    }
    case TypeKind::Ref: {
      const TypeRef ref = node.payload.get<TypeRef>();
      return bound(ref.inner, arena.types.size());
    }
  }
  return false;
}

bool verify_pattern_children(const PatternNode& node, const AstArena& arena) {
  switch (node.kind) {
    case PatternKind::Wildcard:
    case PatternKind::Ident:
    case PatternKind::MutIdent: return true;
    case PatternKind::Literal: {
      const PatternLiteral literal = node.payload.literal;
      return bound(literal.value, arena.literals.size());
    }
    case PatternKind::Tuple: {
      const PatternTuple tuple = node.payload.tuple;
      if (!bound(tuple.path, arena.paths.size())) {
        return false;
      }
      for (const PatternIdx element : tuple.elements) {
        if (!bound(element, arena.patterns.size())) {
          return false;
        }
      }
      return true;
    }
    case PatternKind::Struct: {
      const PatternStruct strukt = node.payload.strukt;
      if (!bound(strukt.path, arena.paths.size())) {
        return false;
      }
      for (const FieldPattern& field : strukt.fields) {
        if (!bound(field.pattern, arena.patterns.size())) {
          return false;
        }
      }
      return true;
    }
    case PatternKind::Ref: {
      const PatternRef ref = node.payload.ref;
      return bound(ref.inner, arena.patterns.size());
    }
    case PatternKind::Or: {
      const PatternOr or_pat = node.payload.or_pat;
      for (const PatternIdx alternative : or_pat.alternatives) {
        if (!bound(alternative, arena.patterns.size())) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

bool verify_expr_children(const ExprNode& node, const AstArena& arena) {
  switch (node.kind) {
    case ExprKind::Literal: {
      const ExprLiteral literal = node.payload.get<ExprLiteral>();
      return bound(literal.value, arena.literals.size());
    }
    case ExprKind::Path: {
      const ExprPath path = node.payload.get<ExprPath>();
      if (!bound(path.idx, arena.paths.size())) {
        return false;
      }
      for (const TypeIdx arg : path.type_args) {
        if (!bound(arg, arena.types.size())) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::Struct: {
      const ExprStruct strukt = node.payload.get<ExprStruct>();
      if (!bound(strukt.path, arena.paths.size()) ||
          !bound(strukt.base_expr, arena.exprs.size())) {
        return false;
      }
      for (const ExprFieldInit& field : strukt.init) {
        if (!bound(field.value, arena.exprs.size())) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::Tuple: {
      const ExprTuple tuple = node.payload.get<ExprTuple>();
      for (const ExprIdx element : tuple.elements) {
        if (!bound(element, arena.exprs.size())) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::Array: {
      const ExprArray array = node.payload.get<ExprArray>();
      if (!bound(array.repeat, arena.exprs.size())) {
        return false;
      }
      for (const ExprIdx element : array.elements) {
        if (!bound(element, arena.exprs.size())) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::Unary: {
      const ExprUnary unary = node.payload.get<ExprUnary>();
      return bound(unary.inner, arena.exprs.size());
    }
    case ExprKind::Borrow: {
      const ExprBorrow borrow = node.payload.get<ExprBorrow>();
      return bound(borrow.inner, arena.exprs.size());
    }
    case ExprKind::Deref: {
      const ExprDeref deref = node.payload.get<ExprDeref>();
      return bound(deref.inner, arena.exprs.size());
    }
    case ExprKind::Binary: {
      const ExprBinary binary = node.payload.get<ExprBinary>();
      return bound(binary.lhs, arena.exprs.size()) &&
             bound(binary.rhs, arena.exprs.size());
    }
    case ExprKind::Cast: {
      const ExprCast cast = node.payload.get<ExprCast>();
      return bound(cast.inner, arena.exprs.size()) &&
             bound(cast.type, arena.types.size());
    }
    case ExprKind::Call: {
      const ExprCall call = node.payload.get<ExprCall>();
      if (!bound(call.callee, arena.exprs.size())) {
        return false;
      }
      for (const TypeIdx arg : call.type_args) {
        if (!bound(arg, arena.types.size())) {
          return false;
        }
      }
      for (const ExprIdx arg : call.args) {
        if (!bound(arg, arena.exprs.size())) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::MethodCall: {
      const ExprMethodCall call = node.payload.get<ExprMethodCall>();
      if (!bound(call.receiver, arena.exprs.size())) {
        return false;
      }
      for (const ExprIdx arg : call.args) {
        if (!bound(arg, arena.exprs.size())) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::Field: {
      const ExprField field = node.payload.get<ExprField>();
      return bound(field.receiver, arena.exprs.size());
    }
    case ExprKind::Index: {
      const ExprIndex index = node.payload.get<ExprIndex>();
      return bound(index.receiver, arena.exprs.size()) &&
             bound(index.index, arena.exprs.size());
    }
    case ExprKind::Question: {
      const ExprQuestion question = node.payload.get<ExprQuestion>();
      return bound(question.inner, arena.exprs.size());
    }
    case ExprKind::If: {
      const ExprIf if_expr = node.payload.get<ExprIf>();
      return bound(if_expr.cond, arena.conds.size()) &&
             bound(if_expr.then_block, arena.blocks.size()) &&
             bound(if_expr.else_block, arena.blocks.size());
    }
    case ExprKind::Match: {
      const ExprMatch match = node.payload.get<ExprMatch>();
      if (!bound(match.scrutinee, arena.exprs.size())) {
        return false;
      }
      for (const ExprMatchArm& arm : match.arms) {
        if (!bound(arm.pattern, arena.patterns.size()) ||
            !bound(arm.body, arena.exprs.size())) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::Loop: {
      const ExprLoop loop = node.payload.get<ExprLoop>();
      return bound(loop.body, arena.blocks.size());
    }
    case ExprKind::While: {
      const ExprWhile while_expr = node.payload.get<ExprWhile>();
      return bound(while_expr.cond, arena.conds.size()) &&
             bound(while_expr.body, arena.blocks.size());
    }
    case ExprKind::Block: {
      const ExprBlock block = node.payload.get<ExprBlock>();
      return bound(block.block, arena.blocks.size());
    }
    case ExprKind::Return: {
      const ExprReturn ret = node.payload.get<ExprReturn>();
      return bound(ret.value, arena.exprs.size());
    }
    case ExprKind::Break:
    case ExprKind::Continue: return true;
    case ExprKind::Range: {
      const ExprRange range = node.payload.get<ExprRange>();
      return bound(range.start, arena.exprs.size()) &&
             bound(range.end, arena.exprs.size());
    }
  }
  return false;
}

bool verify_stmt_children(const StmtNode& node, const AstArena& arena) {
  switch (node.kind) {
    case StmtKind::Decl: {
      const StmtDecl decl = node.payload.get<StmtDecl>();
      return bound(decl.pattern, arena.patterns.size()) &&
             bound(decl.type, arena.types.size()) &&
             bound(decl.init, arena.exprs.size());
    }
    case StmtKind::Reassign: {
      const StmtReassign reassign = node.payload.get<StmtReassign>();
      return bound(reassign.place, arena.exprs.size()) &&
             bound(reassign.value, arena.exprs.size());
    }
    case StmtKind::Expr: {
      const StmtExpr expr = node.payload.get<StmtExpr>();
      return bound(expr.value, arena.exprs.size());
    }
  }
  return false;
}

bool verify_param_children(const ItemFnParam& param, const AstArena& arena) {
  return bound(param.pattern, arena.patterns.size()) &&
         bound(param.type, arena.types.size());
}

bool verify_item_children(const ItemNode& node, const AstArena& arena) {
  switch (node.kind) {
    case ItemKind::Fn: {
      const ItemFn fn = node.payload.get<ItemFn>();
      for (const ItemFnParam& param : fn.params) {
        if (!verify_param_children(param, arena)) {
          return false;
        }
      }
      return bound(fn.return_type, arena.types.size()) &&
             bound(fn.body, arena.blocks.size());
    }
    case ItemKind::Intrinsic: {
      const ItemIntrinsic intrinsic = node.payload.get<ItemIntrinsic>();
      for (const ItemFnParam& param : intrinsic.params) {
        if (!verify_param_children(param, arena)) {
          return false;
        }
      }
      return bound(intrinsic.return_type, arena.types.size());
    }
    case ItemKind::Struct: {
      const ItemStruct strukt = node.payload.get<ItemStruct>();
      for (const ItemStructField& field : strukt.fields) {
        if (!bound(field.type, arena.types.size())) {
          return false;
        }
      }
      return true;
    }
    case ItemKind::Enum: {
      const ItemEnum enum_item = node.payload.get<ItemEnum>();
      for (const ItemEnumVariant& variant : enum_item.variants) {
        for (const TypeIdx field : variant.fields) {
          if (!bound(field, arena.types.size())) {
            return false;
          }
        }
      }
      return true;
    }
    case ItemKind::Impl: {
      const ItemImpl impl = node.payload.get<ItemImpl>();
      if (!bound(impl.type, arena.types.size())) {
        return false;
      }
      for (const ItemIdx method : impl.methods) {
        if (!bound(method, arena.items.size())) {
          return false;
        }
      }
      return true;
    }
    case ItemKind::Static: {
      const ItemStatic static_item = node.payload.get<ItemStatic>();
      return bound(static_item.type, arena.types.size()) &&
             bound(static_item.init, arena.exprs.size());
    }
    case ItemKind::Const: {
      const ItemConst const_item = node.payload.get<ItemConst>();
      return bound(const_item.type, arena.types.size()) &&
             bound(const_item.init, arena.exprs.size());
    }
    case ItemKind::Use: {
      const ItemUse use_item = node.payload.get<ItemUse>();
      return bound(use_item.path, arena.paths.size());
    }
  }
  return false;
}

}  // namespace

base::Result<void, VerifyError> verify_file(const AstArena& arena) {
  for (usize i = 0; i < arena.conds.size(); ++i) {
    if (!verify_cond_children(arena.conds[i], arena)) {
      return base::make_err(VerifyError::DanglingCond);
    }
  }
  for (usize i = 0; i < arena.blocks.size(); ++i) {
    if (!verify_block_children(arena.blocks[i], arena)) {
      return base::make_err(VerifyError::DanglingBlock);
    }
  }
  for (usize i = 0; i < arena.types.size(); ++i) {
    if (!verify_type_children(arena.types[i], arena)) {
      return base::make_err(VerifyError::DanglingType);
    }
  }
  for (usize i = 0; i < arena.patterns.size(); ++i) {
    if (!verify_pattern_children(arena.patterns[i], arena)) {
      return base::make_err(VerifyError::DanglingPattern);
    }
  }
  for (usize i = 0; i < arena.exprs.size(); ++i) {
    if (!verify_expr_children(arena.exprs[i], arena)) {
      return base::make_err(VerifyError::DanglingExpr);
    }
  }
  for (usize i = 0; i < arena.stmts.size(); ++i) {
    if (!verify_stmt_children(arena.stmts[i], arena)) {
      return base::make_err(VerifyError::DanglingStmt);
    }
  }
  for (usize i = 0; i < arena.items.size(); ++i) {
    if (!verify_item_children(arena.items[i], arena)) {
      return base::make_err(VerifyError::DanglingItem);
    }
  }
  return base::make_ok();
}

std::string_view describe_verify_error(VerifyError error) {
  switch (error) {
    case VerifyError::DanglingType: return "type index names no type node";
    case VerifyError::DanglingPath: return "path index names no path";
    case VerifyError::DanglingPattern:
      return "pattern index names no pattern node";
    case VerifyError::DanglingExpr:
      return "expression index names no expression node";
    case VerifyError::DanglingStmt:
      return "statement index names no statement node";
    case VerifyError::DanglingBlock: return "block index names no block";
    case VerifyError::DanglingCond: return "condition index names no condition";
    case VerifyError::DanglingItem: return "item index names no item node";
    case VerifyError::DanglingLiteral: return "literal index names no literal";
  }
  return "invalid abstract syntax tree";
}

}  // namespace ast
