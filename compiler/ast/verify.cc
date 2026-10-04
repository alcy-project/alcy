// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/verify.h"

#include <string_view>
#include <utility>

#include "ast/ast.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"

namespace ast {

namespace {

// An invalid index is an allowed absent edge; anything else must name a node
// the table holds. What the table holds is its to answer, because a table that
// several parsers filled is a set of runs rather than one range and the reach
// it reports counts the room between them.
template <typename I, typename Table>
bool bound(I idx, const Table& table) {
  return table.bound(idx);
}

bool verify_cond_children(const Cond& cond, const AstArena& arena) {
  return bound(cond.pattern, arena.patterns) && bound(cond.init, arena.exprs) &&
         bound(cond.value, arena.exprs);
}

bool verify_block_children(const Block& block, const AstArena& arena) {
  for (const StmtIdx stmt : block.statements) {
    if (!bound(stmt, arena.stmts)) {
      return false;
    }
  }
  return bound(block.value, arena.exprs);
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
        if (!bound(element, arena.types)) {
          return false;
        }
      }
      return true;
    }
    case TypeKind::Array: {
      const TypeArray array = node.payload.get<TypeArray>();
      return bound(array.element, arena.types);
    }
    case TypeKind::Slice: {
      const TypeSlice slice = node.payload.get<TypeSlice>();
      return bound(slice.element, arena.types);
    }
    case TypeKind::Path: {
      const TypePath path = node.payload.get<TypePath>();
      if (!bound(path.path, arena.paths)) {
        return false;
      }
      for (const TypeIdx arg : path.args) {
        if (!bound(arg, arena.types)) {
          return false;
        }
      }
      return true;
    }
    case TypeKind::Ref: {
      const TypeRef ref = node.payload.get<TypeRef>();
      return bound(ref.inner, arena.types);
    }
    case TypeKind::Func: {
      const TypeFunc func = node.payload.get<TypeFunc>();
      for (const TypeIdx param : func.params) {
        if (!bound(param, arena.types)) {
          return false;
        }
      }
      return bound(func.ret, arena.types);
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
      return bound(literal.value, arena.literals);
    }
    case PatternKind::Tuple: {
      const PatternTuple tuple = node.payload.tuple;
      if (!bound(tuple.path, arena.paths)) {
        return false;
      }
      for (const PatternIdx element : tuple.elements) {
        if (!bound(element, arena.patterns)) {
          return false;
        }
      }
      return true;
    }
    case PatternKind::Struct: {
      const PatternStruct strukt = node.payload.strukt;
      if (!bound(strukt.path, arena.paths)) {
        return false;
      }
      for (const FieldPattern& field : strukt.fields) {
        if (!bound(field.pattern, arena.patterns)) {
          return false;
        }
      }
      return true;
    }
    case PatternKind::Ref: {
      const PatternRef ref = node.payload.ref;
      return bound(ref.inner, arena.patterns);
    }
    case PatternKind::Or: {
      const PatternOr or_pat = node.payload.or_pat;
      for (const PatternIdx alternative : or_pat.alternatives) {
        if (!bound(alternative, arena.patterns)) {
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
      return bound(literal.value, arena.literals);
    }
    case ExprKind::Path: {
      const ExprPath path = node.payload.get<ExprPath>();
      if (!bound(path.idx, arena.paths)) {
        return false;
      }
      for (const TypeIdx arg : path.type_args) {
        if (!bound(arg, arena.types)) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::Struct: {
      const ExprStruct strukt = node.payload.get<ExprStruct>();
      if (!bound(strukt.path, arena.paths) ||
          !bound(strukt.base_expr, arena.exprs)) {
        return false;
      }
      for (const ExprFieldInit& field : strukt.init) {
        if (!bound(field.value, arena.exprs)) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::Tuple: {
      const ExprTuple tuple = node.payload.get<ExprTuple>();
      for (const ExprIdx element : tuple.elements) {
        if (!bound(element, arena.exprs)) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::Array: {
      const ExprArray array = node.payload.get<ExprArray>();
      if (!bound(array.repeat, arena.exprs)) {
        return false;
      }
      for (const ExprIdx element : array.elements) {
        if (!bound(element, arena.exprs)) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::Unary: {
      const ExprUnary unary = node.payload.get<ExprUnary>();
      return bound(unary.inner, arena.exprs);
    }
    case ExprKind::Borrow: {
      const ExprBorrow borrow = node.payload.get<ExprBorrow>();
      return bound(borrow.inner, arena.exprs);
    }
    case ExprKind::Deref: {
      const ExprDeref deref = node.payload.get<ExprDeref>();
      return bound(deref.inner, arena.exprs);
    }
    case ExprKind::Binary: {
      const ExprBinary binary = node.payload.get<ExprBinary>();
      return bound(binary.lhs, arena.exprs) && bound(binary.rhs, arena.exprs);
    }
    case ExprKind::Cast: {
      const ExprCast cast = node.payload.get<ExprCast>();
      return bound(cast.inner, arena.exprs) && bound(cast.type, arena.types);
    }
    case ExprKind::Call: {
      const ExprCall call = node.payload.get<ExprCall>();
      if (!bound(call.callee, arena.exprs)) {
        return false;
      }
      for (const TypeIdx arg : call.type_args) {
        if (!bound(arg, arena.types)) {
          return false;
        }
      }
      for (const ExprIdx arg : call.args) {
        if (!bound(arg, arena.exprs)) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::MethodCall: {
      const ExprMethodCall call = node.payload.get<ExprMethodCall>();
      if (!bound(call.receiver, arena.exprs)) {
        return false;
      }
      for (const ExprIdx arg : call.args) {
        if (!bound(arg, arena.exprs)) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::Field: {
      const ExprField field = node.payload.get<ExprField>();
      return bound(field.receiver, arena.exprs);
    }
    case ExprKind::Index: {
      const ExprIndex index = node.payload.get<ExprIndex>();
      return bound(index.receiver, arena.exprs) &&
             bound(index.index, arena.exprs);
    }
    case ExprKind::Question: {
      const ExprQuestion question = node.payload.get<ExprQuestion>();
      return bound(question.inner, arena.exprs);
    }
    case ExprKind::If: {
      const ExprIf if_expr = node.payload.get<ExprIf>();
      return bound(if_expr.cond, arena.conds) &&
             bound(if_expr.then_block, arena.blocks) &&
             bound(if_expr.else_block, arena.blocks);
    }
    case ExprKind::Match: {
      const ExprMatch match = node.payload.get<ExprMatch>();
      if (!bound(match.scrutinee, arena.exprs)) {
        return false;
      }
      for (const ExprMatchArm& arm : match.arms) {
        if (!bound(arm.pattern, arena.patterns) ||
            !bound(arm.body, arena.exprs)) {
          return false;
        }
      }
      return true;
    }
    case ExprKind::Loop: {
      const ExprLoop loop = node.payload.get<ExprLoop>();
      return bound(loop.body, arena.blocks);
    }
    case ExprKind::While: {
      const ExprWhile while_expr = node.payload.get<ExprWhile>();
      return bound(while_expr.cond, arena.conds) &&
             bound(while_expr.body, arena.blocks);
    }
    case ExprKind::Block: {
      const ExprBlock block = node.payload.get<ExprBlock>();
      return bound(block.block, arena.blocks);
    }
    case ExprKind::Return: {
      const ExprReturn ret = node.payload.get<ExprReturn>();
      return bound(ret.value, arena.exprs);
    }
    case ExprKind::Break:
    case ExprKind::Continue: return true;
    case ExprKind::Range: {
      const ExprRange range = node.payload.get<ExprRange>();
      return bound(range.start, arena.exprs) && bound(range.end, arena.exprs);
    }
    case ExprKind::Closure: {
      const ExprClosure closure = node.payload.get<ExprClosure>();
      for (const ClosureParam& param : closure.params) {
        if (!bound(param.type, arena.types)) {
          return false;
        }
      }
      return bound(closure.body, arena.exprs);
    }
  }
  return false;
}

bool verify_stmt_children(const StmtNode& node, const AstArena& arena) {
  switch (node.kind) {
    case StmtKind::Decl: {
      const StmtDecl decl = node.payload.get<StmtDecl>();
      return bound(decl.pattern, arena.patterns) &&
             bound(decl.type, arena.types) && bound(decl.init, arena.exprs);
    }
    case StmtKind::Reassign: {
      const StmtReassign reassign = node.payload.get<StmtReassign>();
      return bound(reassign.place, arena.exprs) &&
             bound(reassign.value, arena.exprs);
    }
    case StmtKind::Expr: {
      const StmtExpr expr = node.payload.get<StmtExpr>();
      return bound(expr.value, arena.exprs);
    }
  }
  return false;
}

bool verify_param_children(const ItemFnParam& param, const AstArena& arena) {
  return bound(param.pattern, arena.patterns) && bound(param.type, arena.types);
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
      return bound(fn.return_type, arena.types) && bound(fn.body, arena.blocks);
    }
    case ItemKind::Intrinsic: {
      const ItemIntrinsic intrinsic = node.payload.get<ItemIntrinsic>();
      for (const ItemFnParam& param : intrinsic.params) {
        if (!verify_param_children(param, arena)) {
          return false;
        }
      }
      return bound(intrinsic.return_type, arena.types);
    }
    case ItemKind::Struct: {
      const ItemStruct strukt = node.payload.get<ItemStruct>();
      for (const ItemStructField& field : strukt.fields) {
        if (!bound(field.type, arena.types)) {
          return false;
        }
      }
      return true;
    }
    case ItemKind::Enum: {
      const ItemEnum enum_item = node.payload.get<ItemEnum>();
      for (const ItemEnumVariant& variant : enum_item.variants) {
        for (const TypeIdx field : variant.fields) {
          if (!bound(field, arena.types)) {
            return false;
          }
        }
      }
      return true;
    }
    case ItemKind::Impl: {
      const ItemImpl impl = node.payload.get<ItemImpl>();
      if (!bound(impl.type, arena.types)) {
        return false;
      }
      if (impl.spec.is_valid() && !bound(impl.spec, arena.types)) {
        return false;
      }
      for (const ItemIdx method : impl.methods) {
        if (!bound(method, arena.items)) {
          return false;
        }
      }
      return true;
    }
    case ItemKind::Spec: {
      const ItemSpec spec = node.payload.get<ItemSpec>();
      for (const SpecMethod& method : spec.methods) {
        for (const ItemFnParam& param : method.params) {
          if (!verify_param_children(param, arena)) {
            return false;
          }
        }
        if (method.return_type.is_valid() &&
            !bound(method.return_type, arena.types)) {
          return false;
        }
      }
      return true;
    }
    case ItemKind::Static: {
      const ItemStatic static_item = node.payload.get<ItemStatic>();
      return bound(static_item.type, arena.types) &&
             bound(static_item.init, arena.exprs);
    }
    case ItemKind::Const: {
      const ItemConst const_item = node.payload.get<ItemConst>();
      return bound(const_item.type, arena.types) &&
             bound(const_item.init, arena.exprs);
    }
    case ItemKind::Use: {
      const ItemUse use_item = node.payload.get<ItemUse>();
      return bound(use_item.path, arena.paths);
    }
  }
  return false;
}

}  // namespace

base::Result<void, VerificationError> verify_file(const AstArena& arena) {
  PROFILE_SCOPE_WITH_CATEGORY("verify-ast", "frontend");
  base::Result<void, VerificationError> broken = base::make_ok();
  // Each walk visits the nodes a table holds and none of the room between its
  // runs, which is why it goes through the table rather than over a count.
  arena.conds.for_each_node([&](const CondIdx i) {
    if (broken.is_err() || verify_cond_children(arena.conds[i], arena)) {
      return;
    }
    broken = base::make_err(VerificationError::DanglingCond);
  });
  arena.blocks.for_each_node([&](const BlockIdx i) {
    if (broken.is_err() || verify_block_children(arena.blocks[i], arena)) {
      return;
    }
    broken = base::make_err(VerificationError::DanglingBlock);
  });
  arena.types.for_each_node([&](const TypeIdx i) {
    if (broken.is_err() || verify_type_children(arena.types[i], arena)) {
      return;
    }
    broken = base::make_err(VerificationError::DanglingType);
  });
  arena.patterns.for_each_node([&](const PatternIdx i) {
    if (broken.is_err() || verify_pattern_children(arena.patterns[i], arena)) {
      return;
    }
    broken = base::make_err(VerificationError::DanglingPattern);
  });
  arena.exprs.for_each_node([&](const ExprIdx i) {
    if (broken.is_err() || verify_expr_children(arena.exprs[i], arena)) {
      return;
    }
    broken = base::make_err(VerificationError::DanglingExpr);
  });
  arena.stmts.for_each_node([&](const StmtIdx i) {
    if (broken.is_err() || verify_stmt_children(arena.stmts[i], arena)) {
      return;
    }
    broken = base::make_err(VerificationError::DanglingStmt);
  });
  arena.items.for_each_node([&](const ItemIdx i) {
    if (broken.is_err() || verify_item_children(arena.items[i], arena)) {
      return;
    }
    broken = base::make_err(VerificationError::DanglingItem);
  });
  if (broken.is_err()) {
    return base::make_err(std::move(broken).unwrap_err());
  }
  return base::make_ok();
}

std::string_view describe_verification_error(VerificationError error) {
  switch (error) {
    case VerificationError::DanglingType:
      return "type index names no type node";
    case VerificationError::DanglingPath: return "path index names no path";
    case VerificationError::DanglingPattern:
      return "pattern index names no pattern node";
    case VerificationError::DanglingExpr:
      return "expression index names no expression node";
    case VerificationError::DanglingStmt:
      return "statement index names no statement node";
    case VerificationError::DanglingBlock: return "block index names no block";
    case VerificationError::DanglingCond:
      return "condition index names no condition";
    case VerificationError::DanglingItem:
      return "item index names no item node";
    case VerificationError::DanglingLiteral:
      return "literal index names no literal";
  }
  return "invalid abstract syntax tree";
}

}  // namespace ast
