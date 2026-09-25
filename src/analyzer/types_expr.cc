// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <span>
#include <string_view>
#include <vector>

#include "analyzer/checker.h"
#include "analyzer/fmt.h"
#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace analyzer {

// Patterns

void Checker::collect_pattern_idents(ast::PatternIdx pattern,
                                     std::vector<std::string_view>& out) {
  switch (ast.patterns[pattern].kind) {
    case ast::PatternKind::Wildcard:
    case ast::PatternKind::Literal: return;
    case ast::PatternKind::Ident: {
      out.push_back(ast.patterns[pattern].payload.ident.name.name);
      return;
    }
    case ast::PatternKind::MutIdent: {
      out.push_back(ast.patterns[pattern].payload.mut_ident.name.name);
      return;
    }
    case ast::PatternKind::Tuple: {
      for (ast::PatternIdx element :
           ast.patterns[pattern].payload.tuple.elements) {
        collect_pattern_idents(element, out);
      }
      return;
    }
    case ast::PatternKind::Struct: {
      for (const ast::FieldPattern& field :
           ast.patterns[pattern].payload.strukt.fields) {
        collect_pattern_idents(field.pattern, out);
      }
      return;
    }
    case ast::PatternKind::Ref: {
      collect_pattern_idents(ast.patterns[pattern].payload.ref.inner, out);
      return;
    }
    case ast::PatternKind::Or: {
      for (ast::PatternIdx alt :
           ast.patterns[pattern].payload.or_pat.alternatives) {
        collect_pattern_idents(alt, out);
      }
      return;
    }
  }
}

void Checker::bind_error_idents(u32 module, ast::PatternIdx pattern) {
  std::vector<std::string_view> names;
  collect_pattern_idents(pattern, names);
  (void)module;
  for (std::string_view name : names) {
    scopes.back().push_back({name, error_type(), false});
  }
}
bool Checker::bind_pattern(u32 module,
                           ast::PatternIdx pattern,
                           ir::TypeIdx type) {
  const bool comp = bind_comp_known || comp_depth > 0;
  const ast::PatternNode& node = ast.patterns[pattern];
  switch (node.kind) {
    case ast::PatternKind::Wildcard: return false;
    case ast::PatternKind::Ident: {
      scopes.back().push_back(
          {node.payload.ident.name.name, type, false, comp});
      return false;
    }
    case ast::PatternKind::MutIdent: {
      scopes.back().push_back(
          {node.payload.mut_ident.name.name, type, true, comp});
      return false;
    }
    case ast::PatternKind::Literal: {
      check_literal(node.payload.literal.value, &type);
      return true;
    }
    case ast::PatternKind::Tuple: {
      if (node.payload.tuple.path.is_valid()) {
        PathValue resolved;
        if (!resolve_variant_path(module, node.payload.tuple.path, resolved)) {
          bind_error_idents(module, pattern);
          return true;
        }
        if (resolved.kind != PathValue::Kind::TupleVariant &&
            resolved.kind != PathValue::Kind::BlessedCtor) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, node.span,
                       "not a tuple variant");
          (void)index;
          bind_error_idents(module, pattern);
          return true;
        }
        if (resolved.kind == PathValue::Kind::TupleVariant) {
          if (!is_error(resolved.type)) {
            unify(type, resolved.type, node.span, "tuple variant pattern");
          }
        }
        std::vector<ir::TypeIdx> payloads;
        if (!is_error(resolved.type) ||
            resolved.kind == PathValue::Kind::BlessedCtor) {
          payloads = variant_payloads(resolved, type, node.span);
        } else {
          // Generic enum: the scrutinee selects the instantiation.
          const GenericInstance* instance =
              generic_instance_for(nominal_index(resolved.enom), type);
          if (instance == nullptr) {
            const u32 index = bag.emit(
                diag::Severity::Error, kAnalyzerTypeMismatch, node.span,
                "variant is not a member of the matched type");
            (void)index;
            bind_error_idents(module, pattern);
            return true;
          }
          const usize kept = push_generic_scope(*instance);
          payloads = variant_payloads(resolved, type, node.span);
          pop_generic_scope(kept);
        }
        const std::span<const ast::PatternIdx> elements =
            node.payload.tuple.elements;
        if (payloads.size() != elements.size()) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerArityError, node.span,
                       "variant expects {} fields, pattern has {}",
                       payloads.size(), elements.size());
          (void)index;
          bind_error_idents(module, pattern);
          return true;
        }
        bool refutable = true;
        for (usize i = 0; i < payloads.size(); ++i) {
          refutable =
              bind_pattern(module, elements[i], payloads[i]) && refutable;
        }
        return refutable;
      }
      if (tag_of(type) != ir::TypeTag::Tuple) {
        unify(type, builder.tuple_type({ir::TypeIdx(0), 0}), node.span,
              "tuple pattern");
        bind_error_idents(module, pattern);
        return false;
      }
      const ir::TupleType& tuple_type =
          builder.tuple_types()[builder.types()[type].as_tuple()];
      const std::span<const ast::PatternIdx> elements =
          node.payload.tuple.elements;
      if (tuple_type.elements.size() != elements.size()) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerArityError, node.span,
                     "tuple pattern has {} elements, type has {}",
                     elements.size(), tuple_type.elements.size());
        (void)index;
        bind_error_idents(module, pattern);
        return false;
      }
      bool refutable = false;
      for (u32 i = 0; i < static_cast<u32>(elements.size()); ++i) {
        if (bind_pattern(module, elements[i], tuple_type.elements[i])) {
          refutable = true;
        }
      }
      return refutable;
    }
    case ast::PatternKind::Struct: {
      NominalEntry* nominal =
          resolve_struct_path(module, node.payload.strukt.path);
      if (nominal == nullptr) {
        bind_error_idents(module, pattern);
        return false;
      }
      unify(type, intern_nominal(*nominal), node.span, "struct pattern");
      const ast::ItemNode& decl = ast.items[nominal->item];
      const ir::StructType& struct_type =
          builder.struct_types()[builder.types()[nominal->type].as_struct()];
      bool refutable = false;
      for (const ast::FieldPattern& field : node.payload.strukt.fields) {
        u32 index = 0;
        bool found = false;
        for (const ast::ItemStructField& decl_field :
             decl.payload.get<ast::ItemStruct>().fields) {
          if (decl_field.name.name == field.name.name) {
            found = true;
            break;
          }
          ++index;
        }
        if (!found) {
          const u32 diag =
              bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                       field.name.span, "unknown field '{}'", field.name.name);
          (void)diag;
          bind_error_idents(module, field.pattern);
          continue;
        }
        if (bind_pattern(module, field.pattern, struct_type.fields[index])) {
          refutable = true;
        }
      }
      return refutable;
    }
    case ast::PatternKind::Ref: {
      const ir::TypeTag tag = tag_of(type);
      if ((node.payload.ref.is_mut && tag != ir::TypeTag::MutRef) ||
          (!node.payload.ref.is_mut && tag != ir::TypeTag::Ref)) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, node.span,
                     "reference pattern on a non-reference type");
        (void)index;
        bind_error_idents(module, pattern);
        return false;
      }
      const ir::TypeIdx pointee =
          builder.ref_types()[builder.types()[type].as_ref()].pointee;
      return bind_pattern(module, node.payload.ref.inner, pointee);
    }
    case ast::PatternKind::Or: {
      const std::span<const ast::PatternIdx> alternatives =
          node.payload.or_pat.alternatives;
      if (alternatives.empty()) {
        return false;
      }
      const usize alt0_start = scopes.back().size();
      bool refutable = bind_pattern(module, alternatives[0], type);
      const usize base = scopes.back().size();
      for (usize i = 1; i < alternatives.size(); ++i) {
        const usize mark = scopes.back().size();
        if (bind_pattern(module, alternatives[i], type)) {
          refutable = true;
        }
        // Every alternative must bind the same names; extras are
        // dropped with a diagnostic.
        for (usize j = mark; j < scopes.back().size(); ++j) {
          bool found = false;
          for (usize k = alt0_start; k < base; ++k) {
            if (scopes.back()[k].name == scopes.back()[j].name) {
              found = true;
              break;
            }
          }
          if (!found) {
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                         ast.patterns[alternatives[i]].span,
                         "or-pattern alternatives must bind the same names");
            (void)index;
          }
        }
        scopes.back().erase(
            scopes.back().begin() +
                static_cast<std::vector<Local>::difference_type>(mark),
            scopes.back().end());
      }
      return refutable;
    }
  }
}

// Expressions

bool Checker::is_bare_int_literal(ast::ExprIdx expr) const {
  const ast::ExprNode& node = ast.exprs[expr];
  if (node.kind != ast::ExprKind::Literal) {
    return false;
  }
  const ast::Literal& value =
      ast.literals[node.payload.get<ast::ExprLiteral>().value];
  if (value.kind != ast::LiteralKind::Integer) {
    return false;
  }
  const std::string_view spelling = value.spelling;
  for (char c : spelling) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      return false;
    }
  }
  return true;
}

// Structural comp-known-ness over checked expressions: literals,
// comp-known locals and literal consts, and pure combinations
// thereof. Calls count when their arguments are comp-known and the
// callee is not an intrinsic (print/panic never are).
bool Checker::expr_comp_known(u32 module, ast::ExprIdx expr) const {
  const ast::ExprNode& node = ast.exprs[expr];
  switch (node.kind) {
    case ast::ExprKind::Literal: {
      const ast::Literal& value =
          ast.literals[node.payload.get<ast::ExprLiteral>().value];
      return value.kind == ast::LiteralKind::Integer ||
             value.kind == ast::LiteralKind::Bool ||
             value.kind == ast::LiteralKind::String;
    }
    case ast::ExprKind::Path: {
      const ast::PathIdx path = node.payload.get<ast::ExprPath>().idx;
      const std::span<const ast::Ident> segments = ast.paths[path].segments;
      if (segments.size() != 1) {
        return false;
      }
      if (const Local* local = lookup_local(segments[0].name)) {
        return local->comp_known;
      }
      return is_literal_const(module, path);
    }
    case ast::ExprKind::Unary:
      return expr_comp_known(module, node.payload.get<ast::ExprUnary>().inner);
    case ast::ExprKind::Borrow:
      return expr_comp_known(module, node.payload.get<ast::ExprBorrow>().inner);
    case ast::ExprKind::Binary:
      return expr_comp_known(module, node.payload.get<ast::ExprBinary>().lhs) &&
             expr_comp_known(module, node.payload.get<ast::ExprBinary>().rhs);
    case ast::ExprKind::Cast:
      return expr_comp_known(module, node.payload.get<ast::ExprCast>().inner);
    case ast::ExprKind::Tuple: {
      for (ast::ExprIdx element : node.payload.get<ast::ExprTuple>().elements) {
        if (!expr_comp_known(module, element)) {
          return false;
        }
      }
      return true;
    }
    case ast::ExprKind::Array: {
      const ast::ExprArray& array = node.payload.get<ast::ExprArray>();
      if (array.repeat.is_valid()) {
        return expr_comp_known(module, array.repeat);
      }
      for (ast::ExprIdx element : array.elements) {
        if (!expr_comp_known(module, element)) {
          return false;
        }
      }
      return true;
    }
    case ast::ExprKind::Struct: {
      for (const ast::ExprFieldInit& field :
           node.payload.get<ast::ExprStruct>().init) {
        if (!expr_comp_known(module, field.value)) {
          return false;
        }
      }
      return !node.payload.get<ast::ExprStruct>().base_expr.is_valid() ||
             expr_comp_known(module,
                             node.payload.get<ast::ExprStruct>().base_expr);
    }
    case ast::ExprKind::Field:
      return expr_comp_known(module,
                             node.payload.get<ast::ExprField>().receiver);
    case ast::ExprKind::Index:
      return expr_comp_known(module,
                             node.payload.get<ast::ExprIndex>().receiver) &&
             expr_comp_known(module, node.payload.get<ast::ExprIndex>().index);
    case ast::ExprKind::Call: {
      for (ast::ExprIdx arg : node.payload.get<ast::ExprCall>().args) {
        if (!expr_comp_known(module, arg)) {
          return false;
        }
      }
      const ast::ExprIdx callee = node.payload.get<ast::ExprCall>().callee;
      if (ast.exprs[callee].kind != ast::ExprKind::Path) {
        return false;
      }
      const ast::PathIdx path =
          ast.exprs[callee].payload.get<ast::ExprPath>().idx;
      const std::span<const ast::Ident> segments = ast.paths[path].segments;
      if (segments.size() == 1) {
        const std::string_view name = segments[0].name;
        if (name == "print" || name == "println" || name == "panic") {
          return false;
        }
      }
      return true;
    }
    case ast::ExprKind::MethodCall: {
      if (!expr_comp_known(module,
                           node.payload.get<ast::ExprMethodCall>().receiver)) {
        return false;
      }
      for (ast::ExprIdx arg : node.payload.get<ast::ExprMethodCall>().args) {
        if (!expr_comp_known(module, arg)) {
          return false;
        }
      }
      return true;
    }
    case ast::ExprKind::Question:
      return expr_comp_known(module,
                             node.payload.get<ast::ExprQuestion>().inner);
    case ast::ExprKind::If: {
      const ast::Cond& cond = ast.conds[node.payload.get<ast::ExprIf>().cond];
      if (cond.is_pattern || !expr_comp_known(module, cond.value)) {
        return false;
      }
      if (!expr_comp_known_block(module,
                                 node.payload.get<ast::ExprIf>().then_block) ||
          (node.payload.get<ast::ExprIf>().else_block.is_valid() &&
           !expr_comp_known_block(
               module, node.payload.get<ast::ExprIf>().else_block))) {
        return false;
      }
      return true;
    }
    case ast::ExprKind::Match: {
      if (!expr_comp_known(module,
                           node.payload.get<ast::ExprMatch>().scrutinee)) {
        return false;
      }
      for (const ast::ExprMatchArm& arm :
           node.payload.get<ast::ExprMatch>().arms) {
        if (!expr_comp_known(module, arm.body)) {
          return false;
        }
      }
      return true;
    }
    case ast::ExprKind::Block:
      return expr_comp_known_block(module,
                                   node.payload.get<ast::ExprBlock>().block);
    case ast::ExprKind::Loop:
      return expr_comp_known_block(module,
                                   node.payload.get<ast::ExprLoop>().body);
    case ast::ExprKind::While: {
      const ast::Cond& cond =
          ast.conds[node.payload.get<ast::ExprWhile>().cond];
      if (cond.is_pattern || !expr_comp_known(module, cond.value)) {
        return false;
      }
      return expr_comp_known_block(module,
                                   node.payload.get<ast::ExprWhile>().body);
    }
    case ast::ExprKind::Break:
    case ast::ExprKind::Continue: return true;
    case ast::ExprKind::Return:
    case ast::ExprKind::Range: return false;
  }
}

bool Checker::expr_comp_known_block(u32 module, ast::BlockIdx block) const {
  const ast::Block& node = ast.blocks[block];
  for (ast::StmtIdx stmt : node.statements) {
    const ast::StmtNode& stmt_node = ast.stmts[stmt];
    if (stmt_node.kind == ast::StmtKind::Decl) {
      if (!expr_comp_known(module,
                           stmt_node.payload.get<ast::StmtDecl>().init)) {
        return false;
      }
      continue;
    }
    if (stmt_node.kind == ast::StmtKind::Reassign) {
      const ast::StmtReassign& reassign =
          stmt_node.payload.get<ast::StmtReassign>();
      if (ast.exprs[reassign.place].kind != ast::ExprKind::Path ||
          !expr_comp_known(module, reassign.value)) {
        return false;
      }
      continue;
    }
    if (stmt_node.kind == ast::StmtKind::Expr) {
      const ast::ExprKind kind =
          ast.exprs[stmt_node.payload.get<ast::StmtExpr>().value].kind;
      if (kind == ast::ExprKind::Break || kind == ast::ExprKind::Continue) {
        continue;
      }
    }
    return false;
  }
  return !node.value.is_valid() || expr_comp_known(module, node.value);
}

// Checks operands of a binary operator: same-type numerics, with
// bare integer literals coerced to the other side (so `x + 42`
// works for any integer x without an annotation).
ir::TypeIdx Checker::check_binary_operands(u32 module,
                                           ast::ExprIdx lhs,
                                           ast::ExprIdx rhs,
                                           diag::Span span,
                                           std::string_view what) {
  ir::TypeIdx left = check_expr(module, lhs, nullptr);
  ir::TypeIdx right = check_expr(module, rhs, nullptr);
  if (!types_equal(left, right) && !is_error(left) && !is_error(right)) {
    if (is_integer_tag(tag_of(right)) && is_bare_int_literal(lhs)) {
      left = check_expr(module, lhs, &right);
    } else if (is_integer_tag(tag_of(left)) && is_bare_int_literal(rhs)) {
      right = check_expr(module, rhs, &left);
    }
  }
  if (is_error(left)) {
    return right;
  }
  if (is_error(right)) {
    return left;
  }
  if (types_equal(left, right)) {
    return left;
  }
  const u32 index =
      bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, span,
               "type mismatch in {}: '{}' vs '{}'", what,
               pretty_tag(tag_of(left)), pretty_tag(tag_of(right)));
  (void)index;
  return error_type();
}

void Checker::check_call_args(u32 module,
                              std::span<const ast::ExprIdx> args,
                              const std::vector<ir::TypeIdx>& params,
                              const std::vector<bool>& comp_params,
                              diag::Span span,
                              std::string_view what,
                              bool skip_first) {
  const usize fixed = skip_first ? 1 : 0;
  if (args.size() + fixed != params.size()) {
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                               "'{}' expects {} arguments, found {}", what,
                               params.size() - fixed, args.size());
    (void)index;
    return;
  }
  for (usize i = 0; i < args.size(); ++i) {
    const ir::TypeIdx param = params[fixed + i];
    const ir::TypeIdx actual = check_expr(module, args[i], &param);
    unify(param, actual, ast.exprs[args[i]].span, "argument");
    const bool comp_required =
        comp_depth > 0 ||
        (fixed + i < comp_params.size() && comp_params[fixed + i]);
    if (comp_required && !comp_checked_in_scope(args[i]) &&
        !expr_comp_known(module, args[i])) {
      if (comp_depth > 0) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                     ast.exprs[args[i]].span,
                     "comp evaluation argument must be comp-known");
        (void)index;
      } else {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                     ast.exprs[args[i]].span,
                     "argument for `comp` parameter must be comp-known");
        (void)index;
      }
    }
  }
}

// A core prelude formatting helper: resolved through the prelude,
// never through a user module (locals shadow the prelude first).
bool Checker::is_core_fmt(const CheckedModule::FnSig* fn) const {
  if (fn->name != "write" && fn->name != "format") {
    return false;
  }
  for (const ModuleNode* module : tree.modules) {
    if (!module->is_prelude) {
      continue;
    }
    for (ast::ItemIdx item : module->items) {
      if (item == fn->item) {
        return true;
      }
    }
  }
  return false;
}

// Comp flags of a resolved function item, in parameter order.
std::vector<bool> Checker::comp_param_flags(ast::ItemIdx item) const {
  std::vector<bool> flags;
  if (!item.is_valid()) {
    return flags;
  }
  const ast::ItemNode& node = ast.items[item];
  if (node.kind != ast::ItemKind::Fn) {
    return flags;
  }
  for (const ast::ItemFnParam& param : node.payload.get<ast::ItemFn>().params) {
    flags.push_back(param.is_comp);
  }
  return flags;
}

// Comp blocks verify their contents in-scope while checking;
// re-checking them afterwards would see popped scopes. Only
// non-block initializers need the post-check here.
bool Checker::comp_checked_in_scope(ast::ExprIdx init) const {
  return ast.exprs[init].kind == ast::ExprKind::Block &&
         ast.exprs[init].payload.get<ast::ExprBlock>().is_comp;
}

// Literal consts (inline constants) are readable in comp
// evaluation; anything else with storage is not.
bool Checker::is_literal_const(u32 module, ast::PathIdx path) const {
  const std::span<const ast::Ident> segments = ast.paths[path].segments;
  if (segments.size() != 1) {
    return false;
  }
  const CheckedModule::StaticInfo* info =
      lookup_static(module, segments[0].name);
  return info != nullptr && info->is_const && info->init.is_valid() &&
         ast.exprs[info->init].kind == ast::ExprKind::Literal;
}

ir::TypeIdx Checker::check_path_expr(u32 module,
                                     ast::PathIdx path,
                                     const ir::TypeIdx* expected,
                                     diag::Span span) {
  PathValue resolved;
  if (!resolve_value_path(module, path, resolved)) {
    return error_type();
  }
  switch (resolved.kind) {
    case PathValue::Kind::Local:
    case PathValue::Kind::Static:
    case PathValue::Kind::UnitVariant: {
      if (resolved.kind == PathValue::Kind::Static && comp_depth > 0 &&
          !is_literal_const(module, path)) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidComp, span,
                     "statics with storage cannot be read in comp evaluation");
        (void)index;
        return error_type();
      }
      if (resolved.kind == PathValue::Kind::UnitVariant) {
        ir::TypeIdx owner = resolved.type;
        if (is_error(owner)) {
          // Generic enum: the expectation selects the instantiation.
          const GenericInstance* instance =
              expected != nullptr ? generic_find(*expected) : nullptr;
          if (instance == nullptr ||
              instance->nominal != nominal_index(resolved.enom)) {
            const u32 index =
                bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                         "cannot infer the generic type; add an annotation");
            (void)index;
            return error_type();
          }
          owner = *expected;
        }
        modules[module].variants.push_back(
            {path, false, true, owner, resolved.variant});
        if (expected != nullptr) {
          return unify(*expected, owner, span, "path");
        }
        return owner;
      }
      if (expected != nullptr) {
        return unify(*expected, resolved.type, span, "path");
      }
      return resolved.type;
    }
    case PathValue::Kind::Function:
    case PathValue::Kind::AssocFunction:
    case PathValue::Kind::TupleVariant: {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                   "callee needs arguments");
      (void)index;
      return error_type();
    }
    case PathValue::Kind::BlessedCtor: {
      // Only `None` stands alone as a value; payload constructors
      // need call syntax. The expectation selects the instantiation.
      if (resolved.ctor_name != "None") {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                     "callee needs arguments");
        (void)index;
        return error_type();
      }
      if (expected != nullptr) {
        if (const BlessedEntry* entry = blessed_find(*expected)) {
          if (!entry->is_result) {
            modules[module].variants.push_back(
                {path, true, false, *expected, 0});
            return *expected;
          }
        }
      }
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                   "cannot infer the blessed type; add an annotation");
      (void)index;
      return error_type();
    }
    case PathValue::Kind::Type: {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                   "expected a value, found a type");
      (void)index;
      return error_type();
    }
  }
}

// Verifies a literal format string against argument element types;
// other comp-known strings verify in lowering, which holds the bytes.
// Returns false after diagnosing.
bool Checker::verify_fmt_literal(ast::ExprIdx fmt_expr,
                                 ast::ExprIdx args_expr,
                                 const std::vector<ir::TypeIdx>& elements,
                                 diag::Span span) {
  if (ast.exprs[fmt_expr].kind != ast::ExprKind::Literal) {
    return true;
  }
  const ast::Literal& literal =
      ast.literals[ast.exprs[fmt_expr].payload.get<ast::ExprLiteral>().value];
  if (literal.kind != ast::LiteralKind::String) {
    return true;
  }
  const FmtParse parsed =
      parse_format_string(unescape_format_string(literal.spelling));
  if (parsed.error != FmtError::None) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                 ast.exprs[fmt_expr].span, "invalid format string");
    (void)index;
    return false;
  }
  if (parsed.placeholders != elements.size()) {
    const u32 index = bag.emit(
        diag::Severity::Error, kAnalyzerArityError, ast.exprs[fmt_expr].span,
        "format string has {} placeholders for {} arguments",
        parsed.placeholders, elements.size());
    (void)index;
    return false;
  }
  for (ir::TypeIdx element : elements) {
    if (!is_formattable_tag(tag_of(element))) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                   ast.exprs[args_expr].span, "argument is not formattable");
      (void)index;
      return false;
    }
  }
  (void)span;
  return true;
}

// Checks a core `fmt::write` call like the print intrinsics:
// shapes here, literal content in lowering (which holds the bytes).
ir::TypeIdx Checker::check_fmt_write(u32 module,
                                     ast::ExprIdx expr,
                                     const ir::TypeIdx* expected,
                                     const CheckedModule::FnSig* fn) {
  const ast::ExprNode& node = ast.exprs[expr];
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  const diag::Span span = node.span;
  if (comp_depth > 0) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerInvalidComp, span,
                 "'write' is not allowed in comp evaluation");
    (void)index;
    return error_type();
  }
  if (args.size() != 3) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                 "'write' expects 3 arguments, found {}", args.size());
    (void)index;
    return error_type();
  }
  const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
  const ir::TypeIdx fmt_type = check_expr(module, args[0], &str);
  unify(str, fmt_type, ast.exprs[args[0]].span, "format string");
  if (!expr_comp_known(module, args[0])) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                 ast.exprs[args[0]].span, "format string must be comp-known");
    (void)index;
    return error_type();
  }
  const ir::TypeIdx buf_type = check_expr(module, args[1], nullptr);
  bool buf_ok = false;
  if (!is_error(buf_type) && tag_of(buf_type) == ir::TypeTag::MutRef) {
    const ir::TypeIdx pointee =
        builder.ref_types()[builder.types()[buf_type].as_ref()].pointee;
    if (tag_of(pointee) == ir::TypeTag::Array) {
      const ir::ArrayType& shape =
          builder.array_types()[builder.types()[pointee].as_array()];
      buf_ok = tag_of(shape.element) == ir::TypeTag::U8;
    }
  }
  if (!buf_ok) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch,
                 ast.exprs[args[1]].span, "buffer must be `&mut [u8]`");
    (void)index;
    return error_type();
  }
  const ir::TypeIdx args_type = check_expr(module, args[2], nullptr);
  // `()` reads as the empty tuple (see types.md).
  const bool empty_args =
      !is_error(args_type) && tag_of(args_type) == ir::TypeTag::Void;
  if (!is_error(args_type) && !empty_args &&
      tag_of(args_type) != ir::TypeTag::Tuple) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch,
                 ast.exprs[args[2]].span, "arguments must be a tuple");
    (void)index;
    return error_type();
  }
  if (!is_error(args_type)) {
    std::vector<ir::TypeIdx> elements;
    if (!empty_args) {
      const ir::TupleType& shape =
          builder.tuple_types()[builder.types()[args_type].as_tuple()];
      for (ir::TypeIdx element : shape.elements) {
        elements.push_back(element);
      }
    }
    if (!verify_fmt_literal(args[0], args[2], elements, span)) {
      return error_type();
    }
  }
  record_call(module, node.payload.get<ast::ExprCall>().callee, fn);
  NominalEntry* outcome = find_nominal_in_scope(module, "WriteOutcome");
  if (outcome == nullptr) {
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerUnknownType,
                               span, "'WriteOutcome' is not in scope");
    (void)index;
    return error_type();
  }
  const ir::TypeIdx result = intern_nominal(*outcome);
  if (expected != nullptr) {
    return unify(*expected, result, span, "call");
  }
  return result;
}

// Checks a core `format` call: like `write` without the buffer.
ir::TypeIdx Checker::check_fmt_format(u32 module,
                                      ast::ExprIdx expr,
                                      const ir::TypeIdx* expected,
                                      const CheckedModule::FnSig* fn) {
  const ast::ExprNode& node = ast.exprs[expr];
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  const diag::Span span = node.span;
  if (comp_depth > 0) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerInvalidComp, span,
                 "'format' is not allowed in comp evaluation");
    (void)index;
    return error_type();
  }
  if (args.size() != 2) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                 "'format' expects 2 arguments, found {}", args.size());
    (void)index;
    return error_type();
  }
  const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
  const ir::TypeIdx fmt_type = check_expr(module, args[0], &str);
  unify(str, fmt_type, ast.exprs[args[0]].span, "format string");
  if (!expr_comp_known(module, args[0])) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                 ast.exprs[args[0]].span, "format string must be comp-known");
    (void)index;
    return error_type();
  }
  const ir::TypeIdx args_type = check_expr(module, args[1], nullptr);
  const bool empty_args =
      !is_error(args_type) && tag_of(args_type) == ir::TypeTag::Void;
  if (!is_error(args_type) && !empty_args &&
      tag_of(args_type) != ir::TypeTag::Tuple) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch,
                 ast.exprs[args[1]].span, "arguments must be a tuple");
    (void)index;
    return error_type();
  }
  if (!is_error(args_type)) {
    std::vector<ir::TypeIdx> elements;
    if (!empty_args) {
      const ir::TupleType& shape =
          builder.tuple_types()[builder.types()[args_type].as_tuple()];
      for (ir::TypeIdx element : shape.elements) {
        elements.push_back(element);
      }
    }
    if (!verify_fmt_literal(args[0], args[1], elements, span)) {
      return error_type();
    }
  }
  record_call(module, node.payload.get<ast::ExprCall>().callee, fn);
  NominalEntry* string_type = find_nominal_in_scope(module, "String");
  if (string_type == nullptr) {
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerUnknownType,
                               span, "'String' is not in scope");
    (void)index;
    return error_type();
  }
  const ir::TypeIdx result = intern_nominal(*string_type);
  if (expected != nullptr) {
    return unify(*expected, result, span, "call");
  }
  return result;
}

// Calls through a resolved callee path: free and associated
// functions, tuple variant constructors (user and blessed), and
// the `print` intrinsic.
ir::TypeIdx Checker::check_call(u32 module,
                                ast::ExprIdx expr,
                                const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprIdx callee = node.payload.get<ast::ExprCall>().callee;
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  const diag::Span span = node.span;
  if (ast.exprs[callee].kind != ast::ExprKind::Path) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                 ast.exprs[callee].span, "cannot call a non-path expression");
    (void)index;
    for (ast::ExprIdx arg : args) {
      check_expr(module, arg, nullptr);
    }
    return error_type();
  }
  const ast::PathIdx path = ast.exprs[callee].payload.get<ast::ExprPath>().idx;
  const std::span<const ast::Ident> segments = ast.paths[path].segments;
  if (segments.size() == 1 && lookup_local(segments[0].name) == nullptr &&
      lookup_static(module, segments[0].name) == nullptr &&
      lookup_function(module, segments[0].name) == nullptr) {
    const std::string_view name = segments[0].name;
    if (name == "print" || name == "println") {
      if (comp_depth > 0) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidComp, span,
                     "'{}' is not allowed in comp evaluation", name);
        (void)index;
        return error_type();
      }
      if (args.size() != 1) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                     "'{}' expects 1 argument, found {}", name, args.size());
        (void)index;
        return error_type();
      }
      const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
      const ir::TypeIdx actual = check_expr(module, args[0], &str);
      unify(str, actual, ast.exprs[args[0]].span, "print argument");
      const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
      if (expected != nullptr) {
        return unify(*expected, unit, span, "call");
      }
      return unit;
    }
    if (name == "panic") {
      if (comp_depth > 0) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidComp, span,
                     "'panic' is not allowed in comp evaluation");
        (void)index;
        return error_type();
      }
      if (args.size() != 1) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                     "'panic' expects 1 argument, found {}", args.size());
        (void)index;
        return error_type();
      }
      const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
      const ir::TypeIdx actual = check_expr(module, args[0], &str);
      unify(str, actual, ast.exprs[args[0]].span, "panic argument");
      return builder.never_type();
    }
  }
  PathValue resolved;
  if (!resolve_value_path(module, path, resolved)) {
    for (ast::ExprIdx arg : args) {
      check_expr(module, arg, nullptr);
    }
    return error_type();
  }
  if (resolved.kind == PathValue::Kind::Function) {
    const CheckedModule::FnSig* fn = resolved.function;
    if (is_core_fmt(fn)) {
      if (fn->name == "format") {
        return check_fmt_format(module, expr, expected, fn);
      }
      return check_fmt_write(module, expr, expected, fn);
    }
    record_call(module, callee, fn);
    check_call_args(module, args, fn->params, comp_param_flags(fn->item), span,
                    fn->name, false);
    if (expected != nullptr) {
      return unify(*expected, fn->ret, span, "call");
    }
    return fn->ret;
  }
  if (resolved.kind == PathValue::Kind::AssocFunction) {
    const CheckedModule::MethodInfo* method = resolved.method;
    record_call(module, callee, method);
    // Associated functions take no receiver; params map 1:1.
    check_call_args(module, args, method->params,
                    comp_param_flags(method->item), span, method->name, false);
    if (expected != nullptr) {
      return unify(*expected, method->ret, span, "call");
    }
    return method->ret;
  }
  if (resolved.kind == PathValue::Kind::TupleVariant ||
      resolved.kind == PathValue::Kind::BlessedCtor) {
    ir::TypeIdx enum_type = resolved.type;
    std::vector<ir::TypeIdx> payloads;
    if (resolved.kind == PathValue::Kind::BlessedCtor) {
      // The annotation (or other expectation) selects the
      // instantiation; any already-interned same-kind entry is only
      // a fallback for inference-free positions.
      const BlessedEntry* entry = nullptr;
      if (expected != nullptr) {
        entry = blessed_find(*expected);
      }
      if (entry == nullptr) {
        entry = resolved.blessed;
      }
      if (entry == nullptr) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                     "cannot infer the blessed type; add an annotation");
        (void)index;
        for (ast::ExprIdx arg : args) {
          check_expr(module, arg, nullptr);
        }
        return error_type();
      }
      const bool wants_result =
          resolved.ctor_name == "Ok" || resolved.ctor_name == "Err";
      if (wants_result != entry->is_result) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch, span,
                     "'{}' is not a variant of '{}'", resolved.ctor_name,
                     entry->is_result ? "Option" : "Result");
        (void)index;
        return error_type();
      }
      enum_type = entry->type;
      payloads = variant_payloads(resolved, enum_type, span);
    } else if (!is_error(enum_type)) {
      payloads = variant_payloads(resolved, enum_type, span);
    } else {
      // Generic enum: the expectation selects the instantiation.
      const GenericInstance* instance =
          expected != nullptr ? generic_find(*expected) : nullptr;
      if (instance == nullptr ||
          instance->nominal != nominal_index(resolved.enom)) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, span,
                     "cannot infer the generic type; add an annotation");
        (void)index;
        for (ast::ExprIdx arg : args) {
          check_expr(module, arg, nullptr);
        }
        return error_type();
      }
      const usize kept = push_generic_scope(*instance);
      payloads = variant_payloads(resolved, *expected, span);
      pop_generic_scope(kept);
      enum_type = *expected;
    }
    if (resolved.kind == PathValue::Kind::TupleVariant) {
      modules[module].variants.push_back(
          {path, false, true, enum_type, resolved.variant});
    } else {
      modules[module].variants.push_back(
          {path, true, resolved.blessed_first, enum_type, 0});
    }
    if (args.size() != payloads.size()) {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerArityError,
                                 span, "variant expects {} arguments, found {}",
                                 payloads.size(), args.size());
      (void)index;
      for (ast::ExprIdx arg : args) {
        check_expr(module, arg, nullptr);
      }
      return error_type();
    }
    for (usize i = 0; i < args.size(); ++i) {
      const ir::TypeIdx actual = check_expr(module, args[i], &payloads[i]);
      unify(payloads[i], actual, ast.exprs[args[i]].span, "variant argument");
      if (comp_depth > 0 && !comp_checked_in_scope(args[i]) &&
          !expr_comp_known(module, args[i])) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                     ast.exprs[args[i]].span,
                     "comp evaluation argument must be comp-known");
        (void)index;
      }
    }
    if (expected != nullptr) {
      return unify(*expected, enum_type, span, "call");
    }
    return enum_type;
  }
  const u32 index = bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                             span, "not callable");
  (void)index;
  for (ast::ExprIdx arg : args) {
    check_expr(module, arg, nullptr);
  }
  return error_type();
}

ir::TypeIdx Checker::check_method_call(u32 module,
                                       ast::ExprIdx expr,
                                       const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ir::TypeIdx receiver = check_expr(
      module, node.payload.get<ast::ExprMethodCall>().receiver, nullptr);
  if (is_error(receiver)) {
    for (ast::ExprIdx arg : node.payload.get<ast::ExprMethodCall>().args) {
      check_expr(module, arg, nullptr);
    }
    return error_type();
  }
  const std::string_view name =
      node.payload.get<ast::ExprMethodCall>().name.name;
  const diag::Span span = node.span;
  if (const BlessedEntry* entry = blessed_find(receiver)) {
    const ir::TypeIdx ok = entry->args[0];
    if (name == "unwrap") {
      if (!node.payload.get<ast::ExprMethodCall>().args.empty()) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                     "'unwrap' expects 0 arguments, found {}",
                     node.payload.get<ast::ExprMethodCall>().args.size());
        (void)index;
        return error_type();
      }
      if (expected != nullptr) {
        return unify(*expected, ok, span, "method call");
      }
      return ok;
    }
    if (name == "expect") {
      if (node.payload.get<ast::ExprMethodCall>().args.size() != 1) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                     "'expect' expects 1 argument, found {}",
                     node.payload.get<ast::ExprMethodCall>().args.size());
        (void)index;
        return error_type();
      }
      const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
      const ir::TypeIdx actual = check_expr(
          module, node.payload.get<ast::ExprMethodCall>().args[0], &str);
      unify(str, actual,
            ast.exprs[node.payload.get<ast::ExprMethodCall>().args[0]].span,
            "expect argument");
      if (expected != nullptr) {
        return unify(*expected, ok, span, "method call");
      }
      return ok;
    }
    if (name == "is_ok" || name == "is_err") {
      if (!node.payload.get<ast::ExprMethodCall>().args.empty()) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerArityError, span,
                     "'{}' expects 0 arguments, found {}", name,
                     node.payload.get<ast::ExprMethodCall>().args.size());
        (void)index;
        return error_type();
      }
      const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
      if (expected != nullptr) {
        return unify(*expected, boolean, span, "method call");
      }
      return boolean;
    }
  }
  ir::TypeIdx nominal = receiver;
  const ir::TypeTag tag = tag_of(receiver);
  if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
    nominal = builder.ref_types()[builder.types()[receiver].as_ref()].pointee;
  }
  const CheckedModule::MethodInfo* method = lookup_method(nominal, name);
  if (method == nullptr) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                 node.payload.get<ast::ExprMethodCall>().name.span,
                 "no method '{}'", name);
    (void)index;
    for (ast::ExprIdx arg : node.payload.get<ast::ExprMethodCall>().args) {
      check_expr(module, arg, nullptr);
    }
    return error_type();
  }
  if (method->receiver == CheckedModule::ReceiverKind::None) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                 node.payload.get<ast::ExprMethodCall>().name.span,
                 "associated function '{}' called as a method", name);
    (void)index;
    return error_type();
  }
  record_call(module, expr, method);
  // No autoref/deref in MVP beyond this: an owned receiver coerces
  // to the declared borrow; full borrow checking is a later stage.
  const ir::TypeIdx declared = method->params[0];
  if (receiver.idx != declared.idx) {
    bool coerced = false;
    if (tag != ir::TypeTag::Ref && tag != ir::TypeTag::MutRef) {
      const ir::TypeTag declared_tag = tag_of(declared);
      if ((declared_tag == ir::TypeTag::Ref ||
           declared_tag == ir::TypeTag::MutRef) &&
          builder.ref_types()[builder.types()[declared].as_ref()].pointee.idx ==
              receiver.idx) {
        coerced = true;
      }
    }
    if (!coerced) {
      const u32 index = bag.emit(
          diag::Severity::Error, kAnalyzerTypeMismatch,
          ast.exprs[node.payload.get<ast::ExprMethodCall>().receiver].span,
          "type mismatch in receiver");
      (void)index;
      return error_type();
    }
  }
  std::vector<ir::TypeIdx> rest(method->params.begin() + 1,
                                method->params.end());
  std::vector<bool> comp_flags = comp_param_flags(method->item);
  // The receiver has no call-site argument; drop its flag with it.
  std::vector<bool> rest_flags;
  if (comp_flags.size() == method->params.size() && !comp_flags.empty()) {
    rest_flags.assign(comp_flags.begin() + 1, comp_flags.end());
  }
  check_call_args(module, node.payload.get<ast::ExprMethodCall>().args, rest,
                  rest_flags, span, name, false);
  if (expected != nullptr) {
    return unify(*expected, method->ret, span, "method call");
  }
  return method->ret;
}

ir::TypeIdx Checker::check_field(u32 module,
                                 ast::ExprIdx expr,
                                 const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::Ident field_name = node.payload.get<ast::ExprField>().name;
  ir::TypeIdx receiver =
      check_expr(module, node.payload.get<ast::ExprField>().receiver, nullptr);
  if (is_error(receiver)) {
    return error_type();
  }
  // Field access sees through references (method receivers are
  // commonly `&Self`); borrow checking is a later stage.
  while (tag_of(receiver) == ir::TypeTag::Ref ||
         tag_of(receiver) == ir::TypeTag::MutRef) {
    receiver = builder.ref_types()[builder.types()[receiver].as_ref()].pointee;
  }
  const ir::TypeTag tag = tag_of(receiver);
  if (tag == ir::TypeTag::Struct) {
    const ir::StructType& struct_type =
        builder.struct_types()[builder.types()[receiver].as_struct()];
    NominalEntry* owner = nullptr;
    u32 field_index = 0;
    for (NominalEntry& entry : nominals) {
      if (!entry.complete || entry.type.idx != receiver.idx) {
        continue;
      }
      const ast::ItemNode& owner_node = ast.items[entry.item];
      if (owner_node.kind != ast::ItemKind::Struct) {
        continue;
      }
      u32 i = 0;
      for (const ast::ItemStructField& decl_field :
           owner_node.payload.get<ast::ItemStruct>().fields) {
        if (decl_field.name.name == field_name.name) {
          owner = &entry;
          field_index = i;
          break;
        }
        ++i;
      }
      if (owner != nullptr) {
        break;
      }
    }
    if (owner == nullptr) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                   field_name.span, "unknown field '{}'", field_name.name);
      (void)index;
      return error_type();
    }
    const ir::TypeIdx result = struct_type.fields[field_index];
    (void)module;
    if (expected != nullptr) {
      return unify(*expected, result, node.span, "field");
    }
    return result;
  }
  if (tag == ir::TypeTag::Tuple) {
    const ir::TupleType& tuple_type =
        builder.tuple_types()[builder.types()[receiver].as_tuple()];
    u32 index = 0;
    bool digits = !field_name.name.empty();
    for (char c : field_name.name) {
      if (c < '0' || c > '9') {
        digits = false;
        break;
      }
      index = index * 10 + static_cast<u32>(c - '0');
    }
    if (!digits || index >= tuple_type.elements.size()) {
      const u32 diag = bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                                field_name.span, "unknown tuple field '{}'",
                                field_name.name);
      (void)diag;
      return error_type();
    }
    const ir::TypeIdx result = tuple_type.elements[index];
    if (expected != nullptr) {
      return unify(*expected, result, node.span, "field");
    }
    return result;
  }
  const u32 index = bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                             node.span, "no fields on '{}'", pretty_tag(tag));
  (void)index;
  return error_type();
}

ir::TypeIdx Checker::check_struct_expr(u32 module,
                                       ast::ExprIdx expr,
                                       const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  NominalEntry* nominal =
      resolve_struct_path(module, node.payload.get<ast::ExprStruct>().path);
  if (nominal == nullptr) {
    for (const ast::ExprFieldInit& field :
         node.payload.get<ast::ExprStruct>().init) {
      check_expr(module, field.value, nullptr);
    }
    if (node.payload.get<ast::ExprStruct>().base_expr.is_valid()) {
      check_expr(module, node.payload.get<ast::ExprStruct>().base_expr,
                 nullptr);
    }
    return error_type();
  }
  const ir::TypeIdx struct_type = intern_nominal(*nominal);
  const ast::ItemNode& decl = ast.items[nominal->item];
  const ir::StructType& fields =
      builder.struct_types()[builder.types()[struct_type].as_struct()];
  std::vector<bool> seen(decl.payload.get<ast::ItemStruct>().fields.size(),
                         false);
  for (const ast::ExprFieldInit& field :
       node.payload.get<ast::ExprStruct>().init) {
    u32 index = 0;
    bool found = false;
    for (const ast::ItemStructField& decl_field :
         decl.payload.get<ast::ItemStruct>().fields) {
      if (decl_field.name.name == field.name.name) {
        found = true;
        break;
      }
      ++index;
    }
    if (!found) {
      const u32 diag =
          bag.emit(diag::Severity::Error, kAnalyzerUnknownValue,
                   field.name.span, "unknown field '{}'", field.name.name);
      (void)diag;
      check_expr(module, field.value, nullptr);
      continue;
    }
    seen[index] = true;
    const ir::TypeIdx field_type = fields.fields[index];
    const ir::TypeIdx actual = check_expr(module, field.value, &field_type);
    unify(field_type, actual, ast.exprs[field.value].span, "field");
  }
  if (node.payload.get<ast::ExprStruct>().base_expr.is_valid()) {
    const ir::TypeIdx base = check_expr(
        module, node.payload.get<ast::ExprStruct>().base_expr, nullptr);
    unify(struct_type, base,
          ast.exprs[node.payload.get<ast::ExprStruct>().base_expr].span,
          "struct update base");
  } else {
    for (usize i = 0; i < seen.size(); ++i) {
      if (!seen[i]) {
        const u32 diag =
            bag.emit(diag::Severity::Error, kAnalyzerArityError, node.span,
                     "missing field '{}'",
                     decl.payload.get<ast::ItemStruct>().fields[i].name.name);
        (void)diag;
      }
    }
  }
  if (expected != nullptr) {
    return unify(*expected, struct_type, node.span, "struct");
  }
  return struct_type;
}

ir::TypeIdx Checker::check_question(u32 module,
                                    ast::ExprIdx expr,
                                    const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ir::TypeIdx inner =
      check_expr(module, node.payload.get<ast::ExprQuestion>().inner, nullptr);
  const BlessedEntry* scrutinee = blessed_find(inner);
  if (scrutinee == nullptr) {
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerBadQuestion,
                               node.span, "'?' needs Result or Option");
    (void)index;
    return error_type();
  }
  const BlessedEntry* enclosing = blessed_find(fn_ret);
  if (enclosing == nullptr) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerBadQuestion, node.span,
                 "'?' needs an enclosing Result or Option function");
    (void)index;
    return error_type();
  }
  if (scrutinee->is_result != enclosing->is_result ||
      scrutinee->args.size() != enclosing->args.size()) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerBadQuestion, node.span,
                 "'?' type does not match the function return type");
    (void)index;
    return error_type();
  }
  for (usize i = 0; i < scrutinee->args.size(); ++i) {
    if (!types_equal(scrutinee->args[i], enclosing->args[i])) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerBadQuestion, node.span,
                   "'?' type does not match the function return type");
      (void)index;
      return error_type();
    }
  }
  if (expected != nullptr) {
    return unify(*expected, scrutinee->args[0], node.span, "'?'");
  }
  return scrutinee->args[0];
}

ir::TypeIdx Checker::check_cast(u32 module,
                                ast::ExprIdx expr,
                                const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ir::TypeIdx inner =
      check_expr(module, node.payload.get<ast::ExprCast>().inner, nullptr);
  const ir::TypeIdx target =
      resolve_type(module, node.payload.get<ast::ExprCast>().type, nullptr);
  if (is_error(inner) || is_error(target)) {
    return error_type();
  }
  const ir::TypeTag from = tag_of(inner);
  const ir::TypeTag to = tag_of(target);
  const bool numeric_from =
      is_integer_tag(from) || is_float_tag(from) || from == ir::TypeTag::I1;
  const bool numeric_to =
      is_integer_tag(to) || is_float_tag(to) || to == ir::TypeTag::I1;
  if (from == ir::TypeTag::Never || (numeric_from && numeric_to)) {
    if (expected != nullptr) {
      return unify(*expected, target, node.span, "cast");
    }
    return target;
  }
  const u32 index = bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                             node.span, "invalid cast from '{}' to '{}'",
                             pretty_tag(from), pretty_tag(to));
  (void)index;
  return error_type();
}

ir::TypeIdx Checker::check_index(u32 module,
                                 ast::ExprIdx expr,
                                 const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ir::TypeIdx receiver =
      check_expr(module, node.payload.get<ast::ExprIndex>().receiver, nullptr);
  const ir::TypeIdx position =
      check_expr(module, node.payload.get<ast::ExprIndex>().index, nullptr);
  if (is_error(receiver) || is_error(position)) {
    return error_type();
  }
  if (!is_integer_tag(tag_of(position))) {
    const u32 diag =
        bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch,
                 ast.exprs[node.payload.get<ast::ExprIndex>().index].span,
                 "array index must be an integer");
    (void)diag;
    return error_type();
  }
  if (tag_of(receiver) != ir::TypeTag::Array) {
    const u32 diag =
        bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, node.span,
                 "cannot index '{}'", pretty_tag(tag_of(receiver)));
    (void)diag;
    return error_type();
  }
  const ir::ArrayType& array =
      builder.array_types()[builder.types()[receiver].as_array()];
  if (expected != nullptr) {
    return unify(*expected, array.element, node.span, "index");
  }
  return array.element;
}

void Checker::check_cond(u32 module, ast::CondIdx cond, bool& binds) {
  const ast::Cond& node = ast.conds[cond];
  binds = false;
  if (node.is_pattern) {
    const ir::TypeIdx init = check_expr(module, node.init, nullptr);
    scopes.emplace_back();
    binds = true;
    bind_pattern(module, node.pattern, init);
    if (verify_comp_known && !comp_checked_in_scope(node.init) &&
        !expr_comp_known(module, node.init)) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                   ast.exprs[node.init].span,
                   "comp condition initializer is not comp-known");
      (void)index;
    }
    return;
  }
  const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
  const ir::TypeIdx actual = check_expr(module, node.value, &boolean);
  unify(boolean, actual, ast.exprs[node.value].span, "condition");
  if (verify_comp_known && !comp_checked_in_scope(node.value) &&
      !expr_comp_known(module, node.value)) {
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                               ast.exprs[node.value].span,
                               "comp condition is not comp-known");
    (void)index;
  }
}

ir::TypeIdx Checker::check_if(u32 module,
                              ast::ExprIdx expr,
                              const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  bool binds = false;
  check_cond(module, node.payload.get<ast::ExprIf>().cond, binds);
  const ir::TypeIdx then =
      check_block(module, node.payload.get<ast::ExprIf>().then_block, expected);
  if (binds) {
    scopes.pop_back();
  }
  if (!node.payload.get<ast::ExprIf>().else_block.is_valid()) {
    if (!is_void(then) && !is_error(then) && !is_never(then)) {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerTypeMismatch,
                                 node.span, "if without else yields '()'");
      (void)index;
    }
    const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
    if (expected != nullptr) {
      return unify(*expected, unit, node.span, "if");
    }
    return unit;
  }
  const ir::TypeIdx otherwise =
      check_block(module, node.payload.get<ast::ExprIf>().else_block, expected);
  return unify(then, otherwise, node.span, "if branches");
}

// Exhaustiveness over the plan's bounded scope: bool and enum
// variants by enumeration, integer matches by wildcard (literal
// ranges cannot cover a full integer type), tuples and structs by
// wildcard or a matching constructor pattern.
void Checker::check_exhaustive(u32 module,
                               ir::TypeIdx scrutinee,
                               std::span<const ast::ExprMatchArm> arms,
                               diag::Span span) {
  bool wildcard = false;
  std::vector<bool> covered_bool{false, false};
  for (const ast::ExprMatchArm& arm : arms) {
    if (pattern_is_wildcard(arm.pattern)) {
      wildcard = true;
    }
    collect_bool_literals(arm.pattern, covered_bool);
  }
  if (wildcard) {
    return;
  }
  const ir::TypeTag tag = tag_of(scrutinee);
  if (tag == ir::TypeTag::I1) {
    if (!covered_bool[0] || !covered_bool[1]) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                   "non-exhaustive match: missing '{}'",
                   !covered_bool[0] ? "true" : "false");
      (void)index;
    }
    return;
  }
  if (tag == ir::TypeTag::Enum) {
    const ir::EnumType& enum_type =
        builder.enum_types()[builder.types()[scrutinee].as_enum()];
    // Find the declaring nominal for variant names.
    const ast::ItemNode* decl = nullptr;
    for (NominalEntry& entry : nominals) {
      if (!entry.complete || entry.type.idx != scrutinee.idx) {
        continue;
      }
      const ast::ItemNode& candidate = ast.items[entry.item];
      if (candidate.kind != ast::ItemKind::Enum) {
        continue;
      }
      decl = &candidate;
      break;
    }
    if (decl == nullptr) {
      for (const BlessedEntry& entry : blessed) {
        if (entry.type.idx != scrutinee.idx) {
          continue;
        }
        // Blessed constructors cover by side: Ok/Some is variant 0,
        // Err/None is variant 1. Unconditional patterns cover both.
        bool covered[2] = {false, false};
        for (const ast::ExprMatchArm& arm : arms) {
          mark_blessed_covered(module, arm.pattern, covered);
        }
        if (covered[0] && covered[1]) {
          return;
        }
        const char* missing = covered[0] ? (entry.is_result ? "Err" : "None")
                                         : (entry.is_result ? "Ok" : "Some");
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                     "non-exhaustive match: '{}' not covered", missing);
        (void)index;
        return;
      }
      // Generic instantiations share their nominal's declaration.
      for (const GenericInstance& instance : generic_instances) {
        if (instance.type.idx != scrutinee.idx) {
          continue;
        }
        const ast::ItemNode& candidate =
            ast.items[nominals[instance.nominal].item];
        if (candidate.kind != ast::ItemKind::Enum) {
          continue;
        }
        decl = &candidate;
        break;
      }
      if (decl == nullptr) {
        return;
      }
    }
    std::vector<bool> covered(static_cast<usize>(enum_type.variants.size()),
                              false);
    const std::span<const ast::ItemEnumVariant> variants =
        decl->payload.get<ast::ItemEnum>().variants;
    for (const ast::ExprMatchArm& arm : arms) {
      mark_variant_covered(arm.pattern, variants, covered);
    }
    for (usize i = 0; i < covered.size(); ++i) {
      if (!covered[i]) {
        const u32 index = bag.emit(
            diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
            "non-exhaustive match: '{}' not covered", variants[i].name.name);
        (void)index;
        return;
      }
    }
    return;
  }
  if (is_integer_tag(tag)) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                 "non-exhaustive integer match: add a wildcard arm");
    (void)index;
    return;
  }
  if (tag == ir::TypeTag::Tuple) {
    for (const ast::ExprMatchArm& arm : arms) {
      if (ast.patterns[arm.pattern].kind == ast::PatternKind::Tuple &&
          !ast.patterns[arm.pattern].payload.tuple.path.is_valid()) {
        return;
      }
    }
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                 "non-exhaustive tuple match: add a wildcard arm");
    (void)index;
    return;
  }
  if (tag == ir::TypeTag::Struct) {
    for (const ast::ExprMatchArm& arm : arms) {
      if (ast.patterns[arm.pattern].kind == ast::PatternKind::Struct) {
        return;
      }
    }
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch, span,
                 "non-exhaustive struct match: add a wildcard arm");
    (void)index;
    return;
  }
  const u32 index = bag.emit(diag::Severity::Error, kAnalyzerNonExhaustiveMatch,
                             span, "non-exhaustive match: add a wildcard arm");
  (void)index;
}

bool Checker::pattern_is_wildcard(ast::PatternIdx pattern) const {
  switch (ast.patterns[pattern].kind) {
    case ast::PatternKind::Wildcard: return true;
    case ast::PatternKind::Ident:
    case ast::PatternKind::MutIdent: return true;
    case ast::PatternKind::Or: {
      for (ast::PatternIdx alt :
           ast.patterns[pattern].payload.or_pat.alternatives) {
        if (pattern_is_wildcard(alt)) {
          return true;
        }
      }
      return false;
    }
    default: return false;
  }
}

void Checker::collect_bool_literals(ast::PatternIdx pattern,
                                    std::vector<bool>& covered) const {
  switch (ast.patterns[pattern].kind) {
    case ast::PatternKind::Literal: {
      const ast::Literal& value =
          ast.literals[ast.patterns[pattern].payload.literal.value];
      if (value.kind == ast::LiteralKind::Bool) {
        covered[value.spelling == "true" ? 0 : 1] = true;
      }
      return;
    }
    case ast::PatternKind::Or: {
      for (ast::PatternIdx alt :
           ast.patterns[pattern].payload.or_pat.alternatives) {
        collect_bool_literals(alt, covered);
      }
      return;
    }
    default: return;
  }
}

void Checker::mark_variant_covered(
    ast::PatternIdx pattern,
    std::span<const ast::ItemEnumVariant> variants,
    std::vector<bool>& covered) {
  const ast::PatternNode& node = ast.patterns[pattern];
  switch (node.kind) {
    case ast::PatternKind::Ident: {
      for (usize i = 0; i < covered.size(); ++i) {
        if (variants[i].name.name == node.payload.ident.name.name &&
            variants[i].fields.empty()) {
          covered[i] = true;
          return;
        }
      }
      return;
    }
    case ast::PatternKind::Tuple: {
      if (!node.payload.tuple.path.is_valid() ||
          ast.paths[node.payload.tuple.path].segments.empty()) {
        return;
      }
      const std::string_view name =
          ast.paths[node.payload.tuple.path].segments.back().name;
      for (usize i = 0; i < covered.size(); ++i) {
        if (variants[i].name.name == name) {
          covered[i] = true;
          return;
        }
      }
      return;
    }
    case ast::PatternKind::Or: {
      for (ast::PatternIdx alt : node.payload.or_pat.alternatives) {
        mark_variant_covered(alt, variants, covered);
      }
      return;
    }
    default: return;
  }
}

void Checker::mark_blessed_covered(u32 module,
                                   ast::PatternIdx pattern,
                                   bool covered[2]) {
  const ast::PatternNode& node = ast.patterns[pattern];
  switch (node.kind) {
    case ast::PatternKind::Wildcard:
    case ast::PatternKind::Ident:
    case ast::PatternKind::MutIdent: return;
    case ast::PatternKind::Tuple: {
      if (!node.payload.tuple.path.is_valid()) {
        return;
      }
      PathValue resolved;
      if (!resolve_variant_path(module, node.payload.tuple.path, resolved)) {
        return;
      }
      if (resolved.kind != PathValue::Kind::BlessedCtor) {
        return;
      }
      covered[resolved.blessed_first ? 0 : 1] = true;
      return;
    }
    case ast::PatternKind::Or: {
      for (ast::PatternIdx alt : node.payload.or_pat.alternatives) {
        mark_blessed_covered(module, alt, covered);
      }
      return;
    }
    default: return;
  }
}

ir::TypeIdx Checker::check_match(u32 module,
                                 ast::ExprIdx expr,
                                 const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprIdx scrutinee_expr =
      node.payload.get<ast::ExprMatch>().scrutinee;
  const ir::TypeIdx scrutinee = check_expr(module, scrutinee_expr, nullptr);
  if (verify_comp_known && !comp_checked_in_scope(scrutinee_expr) &&
      !expr_comp_known(module, scrutinee_expr)) {
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                               ast.exprs[scrutinee_expr].span,
                               "comp match scrutinee is not comp-known");
    (void)index;
  }
  ir::TypeIdx result = error_type();
  bool first = true;
  for (const ast::ExprMatchArm& arm : node.payload.get<ast::ExprMatch>().arms) {
    scopes.emplace_back();
    if (!is_error(scrutinee)) {
      bind_pattern(module, arm.pattern, scrutinee);
    }
    const ir::TypeIdx body = check_expr(module, arm.body, expected);
    scopes.pop_back();
    if (first) {
      result = body;
      first = false;
    } else {
      result = unify(result, body, ast.exprs[arm.body].span, "match arms");
    }
  }
  if (!is_error(scrutinee)) {
    check_exhaustive(module, scrutinee, node.payload.get<ast::ExprMatch>().arms,
                     node.span);
  }
  if (expected != nullptr && !first) {
    return unify(*expected, result, node.span, "match");
  }
  return result;
}

ir::TypeIdx Checker::check_expr(u32 module,
                                ast::ExprIdx expr,
                                const ir::TypeIdx* expected) {
  const ir::TypeIdx type = check_expr_inner(module, expr, expected);
  modules[module].expr_types.emplace_back(expr, type);
  return type;
}

ir::TypeIdx Checker::check_array(u32 module,
                                 ast::ExprIdx expr,
                                 const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprArray& array = node.payload.get<ast::ExprArray>();
  // Bounds compiler-time expansion: repeat counts are unbounded
  // literals, and lowering stores per element.
  static constexpr u64 kMaxArrayElements = 1u << 20;
  const ir::TypeIdx* element_expected = nullptr;
  ir::TypeIdx expected_element = error_type();
  u64 expected_count = 0;
  bool has_expected_count = false;
  if (expected != nullptr && tag_of(*expected) == ir::TypeTag::Array) {
    const ir::ArrayType& shape =
        builder.array_types()[builder.types()[*expected].as_array()];
    expected_element = shape.element;
    element_expected = &expected_element;
    expected_count = shape.count;
    has_expected_count = true;
  }
  if (array.repeat.is_valid()) {
    if (array.count > kMaxArrayElements) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, node.span,
                   "array repeat count {} exceeds the limit of {}", array.count,
                   kMaxArrayElements);
      (void)index;
      return error_type();
    }
    const ir::TypeIdx element =
        check_expr(module, array.repeat, element_expected);
    if (has_expected_count && expected_count != array.count) {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerArityError, node.span,
                   "array expects {} elements, found repeat of {}",
                   expected_count, array.count);
      (void)index;
      return error_type();
    }
    const ir::TypeIdx type = builder.array_type(element, array.count);
    if (expected != nullptr) {
      return unify(*expected, type, node.span, "array");
    }
    return type;
  }
  if (array.elements.empty()) {
    const u32 index =
        bag.emit(diag::Severity::Error, kAnalyzerArityError, node.span,
                 "array literal needs elements or a repeat count");
    (void)index;
    return error_type();
  }
  if (has_expected_count && expected_count != array.elements.size()) {
    const u32 index = bag.emit(diag::Severity::Error, kAnalyzerArityError,
                               node.span, "array expects {} elements, found {}",
                               expected_count, array.elements.size());
    (void)index;
    return error_type();
  }
  ir::TypeIdx element = check_expr(module, array.elements[0], element_expected);
  for (usize i = 1; i < array.elements.size(); ++i) {
    const ir::TypeIdx next = check_expr(module, array.elements[i], &element);
    unify(element, next, ast.exprs[array.elements[i]].span, "array element");
  }
  const ir::TypeIdx type = builder.array_type(element, array.elements.size());
  if (expected != nullptr) {
    return unify(*expected, type, node.span, "array");
  }
  return type;
}

ir::TypeIdx Checker::check_expr_inner(u32 module,
                                      ast::ExprIdx expr,
                                      const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  switch (node.kind) {
    case ast::ExprKind::Literal: {
      return check_literal(node.payload.get<ast::ExprLiteral>().value,
                           expected);
    }
    case ast::ExprKind::Path: {
      return check_path_expr(module, node.payload.get<ast::ExprPath>().idx,
                             expected, node.span);
    }
    case ast::ExprKind::Struct: {
      return check_struct_expr(module, expr, expected);
    }
    case ast::ExprKind::Tuple: {
      const ast::ExprTuple& tuple = node.payload.get<ast::ExprTuple>();
      if (tuple.elements.empty()) {
        const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
        if (expected != nullptr) {
          return unify(*expected, unit, node.span, "unit");
        }
        return unit;
      }
      const ir::TupleType* expected_tuple = nullptr;
      ir::TupleType expected_copy;
      if (expected != nullptr && tag_of(*expected) == ir::TypeTag::Tuple) {
        expected_copy =
            builder.tuple_types()[builder.types()[*expected].as_tuple()];
        expected_tuple = &expected_copy;
      }
      std::vector<ir::TypeIdx> elements;
      elements.reserve(tuple.elements.size());
      for (usize i = 0; i < tuple.elements.size(); ++i) {
        const ir::TypeIdx* element_expected = nullptr;
        ir::TypeIdx element_type = error_type();
        if (expected_tuple != nullptr &&
            expected_tuple->elements.size() == tuple.elements.size()) {
          element_type = expected_tuple->elements[i];
          element_expected = &element_type;
        }
        elements.push_back(
            check_expr(module, tuple.elements[i], element_expected));
      }
      ir::TypeSeq seq;
      for (ir::TypeIdx element : elements) {
        seq.push(builder.ref_type(element));
      }
      const ir::TypeIdx type = builder.tuple_type(seq.finish());
      if (expected != nullptr) {
        return unify(*expected, type, node.span, "tuple");
      }
      return type;
    }
    case ast::ExprKind::Array: {
      return check_array(module, expr, expected);
    }
    case ast::ExprKind::Unary: {
      const ir::TypeIdx inner =
          check_expr(module, node.payload.get<ast::ExprUnary>().inner, nullptr);
      if (is_error(inner)) {
        return error_type();
      }
      const ir::TypeTag tag = tag_of(inner);
      switch (node.payload.get<ast::ExprUnary>().op) {
        case ast::UnaryOp::Neg:
          if (is_integer_tag(tag) || is_float_tag(tag)) {
            if (expected != nullptr) {
              return unify(*expected, inner, node.span, "negation");
            }
            return inner;
          }
          break;
        case ast::UnaryOp::Not:
          if (tag == ir::TypeTag::I1) {
            if (expected != nullptr) {
              return unify(*expected, inner, node.span, "not");
            }
            return inner;
          }
          break;
        case ast::UnaryOp::BitNot:
          if (is_integer_tag(tag)) {
            if (expected != nullptr) {
              return unify(*expected, inner, node.span, "bitwise not");
            }
            return inner;
          }
          break;
      }
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, node.span,
                   "invalid unary operand '{}'", pretty_tag(tag));
      (void)index;
      return error_type();
    }
    case ast::ExprKind::Borrow: {
      // Place-ness is a borrow-checking concern; here the
      // inner type only determines the reference shape.
      const ir::TypeIdx pointee = check_expr(
          module, node.payload.get<ast::ExprBorrow>().inner, nullptr);
      if (is_error(pointee)) {
        return error_type();
      }
      const ir::TypeIdx type = builder.reference_type(
          pointee, node.payload.get<ast::ExprBorrow>().is_mut);
      if (expected != nullptr) {
        return unify(*expected, type, node.span, "borrow");
      }
      return type;
    }
    case ast::ExprKind::Binary: {
      if (node.payload.get<ast::ExprBinary>().op == ast::BinaryOp::And ||
          node.payload.get<ast::ExprBinary>().op == ast::BinaryOp::Or) {
        const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
        const ir::TypeIdx left = check_expr(
            module, node.payload.get<ast::ExprBinary>().lhs, &boolean);
        const ir::TypeIdx right = check_expr(
            module, node.payload.get<ast::ExprBinary>().rhs, &boolean);
        unify(boolean, left,
              ast.exprs[node.payload.get<ast::ExprBinary>().lhs].span,
              "logical operand");
        unify(boolean, right,
              ast.exprs[node.payload.get<ast::ExprBinary>().rhs].span,
              "logical operand");
        if (expected != nullptr) {
          return unify(*expected, boolean, node.span, "logical");
        }
        return boolean;
      }
      const ir::TypeIdx operands = check_binary_operands(
          module, node.payload.get<ast::ExprBinary>().lhs,
          node.payload.get<ast::ExprBinary>().rhs, node.span, "binary");
      if (is_error(operands)) {
        return error_type();
      }
      const ir::TypeTag tag = tag_of(operands);
      switch (node.payload.get<ast::ExprBinary>().op) {
        case ast::BinaryOp::Eq:
        case ast::BinaryOp::NotEq: {
          const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
          if (expected != nullptr) {
            return unify(*expected, boolean, node.span, "comparison");
          }
          return boolean;
        }
        case ast::BinaryOp::Gt:
        case ast::BinaryOp::Lt:
        case ast::BinaryOp::GtEq:
        case ast::BinaryOp::LtEq:
          if (is_integer_tag(tag) || is_float_tag(tag)) {
            const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
            if (expected != nullptr) {
              return unify(*expected, boolean, node.span, "comparison");
            }
            return boolean;
          }
          break;
        default:
          if (is_integer_tag(tag) || is_float_tag(tag)) {
            if (expected != nullptr) {
              return unify(*expected, operands, node.span, "arithmetic");
            }
            return operands;
          }
          break;
      }
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation, node.span,
                   "invalid binary operand '{}'", pretty_tag(tag));
      (void)index;
      return error_type();
    }
    case ast::ExprKind::Cast: {
      return check_cast(module, expr, expected);
    }
    case ast::ExprKind::Call: {
      return check_call(module, expr, expected);
    }
    case ast::ExprKind::MethodCall: {
      return check_method_call(module, expr, expected);
    }
    case ast::ExprKind::Field: {
      return check_field(module, expr, expected);
    }
    case ast::ExprKind::Index: {
      return check_index(module, expr, expected);
    }
    case ast::ExprKind::Question: {
      return check_question(module, expr, expected);
    }
    case ast::ExprKind::If: {
      return check_if(module, expr, expected);
    }
    case ast::ExprKind::Match: {
      return check_match(module, expr, expected);
    }
    case ast::ExprKind::Loop: {
      const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
      ++loop_depth;
      check_block(module, node.payload.get<ast::ExprLoop>().body, &unit);
      --loop_depth;
      if (expected != nullptr) {
        return unify(*expected, unit, node.span, "loop");
      }
      return unit;
    }
    case ast::ExprKind::While: {
      bool binds = false;
      check_cond(module, node.payload.get<ast::ExprWhile>().cond, binds);
      const ir::TypeIdx unit = builder.primitive(ir::TypeTag::Void);
      ++loop_depth;
      check_block(module, node.payload.get<ast::ExprWhile>().body, &unit);
      --loop_depth;
      if (binds) {
        scopes.pop_back();
      }
      if (expected != nullptr) {
        return unify(*expected, unit, node.span, "while");
      }
      return unit;
    }
    case ast::ExprKind::Block: {
      const ast::ExprBlock& block = node.payload.get<ast::ExprBlock>();
      if (!block.is_comp) {
        return check_block(module, block.block, expected);
      }
      ++comp_depth;
      const bool was_verifying = verify_comp_known;
      verify_comp_known = true;
      const ir::TypeIdx type = check_block(module, block.block, expected);
      verify_comp_known = was_verifying;
      --comp_depth;
      return type;
    }
    case ast::ExprKind::Return: {
      if (comp_depth > 0) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerInvalidComp, node.span,
                     "'ret' must not cross a comp block boundary");
        (void)index;
        return error_type();
      }
      if (!in_fn) {
        const u32 index = bag.emit(diag::Severity::Error, kAnalyzerBadReturn,
                                   node.span, "'ret' outside of a function");
        (void)index;
        return error_type();
      }
      if (!node.payload.get<ast::ExprReturn>().value.is_valid()) {
        unify(fn_ret, builder.primitive(ir::TypeTag::Void), node.span,
              "return");
      } else {
        const ir::TypeIdx value = check_expr(
            module, node.payload.get<ast::ExprReturn>().value, &fn_ret);
        unify(fn_ret, value,
              ast.exprs[node.payload.get<ast::ExprReturn>().value].span,
              "return");
      }
      return builder.never_type();
    }
    case ast::ExprKind::Break:
    case ast::ExprKind::Continue: {
      if (loop_depth == 0) {
        if (node.kind == ast::ExprKind::Break) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerBreakOutsideLoop,
                       node.span, "'break' outside of a loop");
          (void)index;
        } else {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerBreakOutsideLoop,
                       node.span, "'continue' outside of a loop");
          (void)index;
        }
        return error_type();
      }
      return builder.never_type();
    }
    case ast::ExprKind::Range: {
      const u32 index =
          bag.emit(diag::Severity::Error, kAnalyzerUnsupportedExpr, node.span,
                   "range expressions arrive post-MVP");
      (void)index;
      return error_type();
    }
  }
}

ir::TypeIdx Checker::check_block(u32 module,
                                 ast::BlockIdx block,
                                 const ir::TypeIdx* expected) {
  const ast::Block& node = ast.blocks[block];
  scopes.emplace_back();
  for (ast::StmtIdx stmt : node.statements) {
    check_stmt(module, stmt);
  }
  ir::TypeIdx result = builder.primitive(ir::TypeTag::Void);
  if (node.value.is_valid()) {
    result = check_expr(module, node.value, expected);
    if (expected != nullptr) {
      result = unify(*expected, result, ast.exprs[node.value].span, "block");
    }
    if (verify_comp_known && !expr_comp_known(module, node.value)) {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                                 ast.exprs[node.value].span,
                                 "comp block value is not comp-known");
      (void)index;
    }
  } else if (expected != nullptr) {
    result = unify(*expected, result, node.span, "block");
  }
  scopes.pop_back();
  return result;
}

ir::TypeIdx Checker::check_place(u32 module, ast::ExprIdx place) {
  const ast::ExprNode& node = ast.exprs[place];
  switch (node.kind) {
    case ast::ExprKind::Path: {
      const ast::PathIdx path = node.payload.get<ast::ExprPath>().idx;
      PathValue resolved;
      if (!resolve_value_path(module, path, resolved)) {
        return error_type();
      }
      if (resolved.kind != PathValue::Kind::Local) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerBadAssignment, node.span,
                     "cannot assign to this place");
        (void)index;
        return error_type();
      }
      // Locals shadow everything, but the resolution above may have
      // found the name through another namespace; confirm mutability
      // through the scope entry.
      const Local* local = lookup_local(ast.paths[path].segments.back().name);
      if (local == nullptr || !local->is_mut) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerBadAssignment, node.span,
                     "cannot assign to an immutable binding");
        (void)index;
        return error_type();
      }
      if (verify_comp_known && !local->comp_known) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown, node.span,
                     "comp assignment place is not comp-known");
        (void)index;
        return error_type();
      }
      return local->type;
    }
    case ast::ExprKind::Field: {
      const ir::TypeIdx receiver =
          check_place(module, node.payload.get<ast::ExprField>().receiver);
      if (is_error(receiver)) {
        return error_type();
      }
      if (tag_of(receiver) != ir::TypeTag::Struct) {
        return error_type();
      }
      return check_field(module, place, nullptr);
    }
    case ast::ExprKind::Index: {
      const ir::TypeIdx receiver =
          check_place(module, node.payload.get<ast::ExprIndex>().receiver);
      if (is_error(receiver)) {
        return error_type();
      }
      return check_index(module, place, nullptr);
    }
    default: {
      const u32 index = bag.emit(diag::Severity::Error, kAnalyzerBadAssignment,
                                 node.span, "cannot assign to this place");
      (void)index;
      return error_type();
    }
  }
}

void Checker::check_stmt(u32 module, ast::StmtIdx stmt) {
  const ast::StmtNode& node = ast.stmts[stmt];
  switch (node.kind) {
    case ast::StmtKind::Decl: {
      const ast::StmtDecl& decl = node.payload.get<ast::StmtDecl>();
      const ir::TypeIdx* expected = nullptr;
      ir::TypeIdx ascribed = error_type();
      if (decl.type.is_valid()) {
        ascribed = resolve_type(module, decl.type, nullptr);
        expected = &ascribed;
      }
      bool entered_comp = false;
      if (decl.is_comp) {
        ++comp_depth;
        entered_comp = true;
      }
      const ir::TypeIdx init = check_expr(module, decl.init, expected);
      if (expected != nullptr) {
        unify(*expected, init, ast.exprs[decl.init].span, "declaration");
      }
      if ((decl.is_comp || verify_comp_known) &&
          !comp_checked_in_scope(decl.init) &&
          !expr_comp_known(module, decl.init)) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerNotCompKnown,
                     ast.exprs[decl.init].span,
                     "comp declaration initializer is not comp-known");
        (void)index;
      }
      bind_comp_known = decl.is_comp;
      const bool refutable = bind_pattern(module, decl.pattern, init);
      bind_comp_known = false;
      if (entered_comp) {
        --comp_depth;
      }
      if (refutable) {
        const u32 index =
            bag.emit(diag::Severity::Error, kAnalyzerRefutableLet,
                     ast.patterns[decl.pattern].span,
                     "refutable pattern in declaration; use match");
        (void)index;
      }
      return;
    }
    case ast::StmtKind::Reassign: {
      const ast::StmtReassign& reassign = node.payload.get<ast::StmtReassign>();
      if (comp_depth == 0 &&
          ast.exprs[reassign.place].kind == ast::ExprKind::Path) {
        const ast::PathIdx path =
            ast.exprs[reassign.place].payload.get<ast::ExprPath>().idx;
        const std::span<const ast::Ident> segments = ast.paths[path].segments;
        if (segments.size() == 1) {
          if (const Local* local = lookup_local(segments[0].name)) {
            if (local->comp_known) {
              const u32 index = bag.emit(
                  diag::Severity::Error, kAnalyzerInvalidComp, node.span,
                  "cannot reassign a comp binding at runtime");
              (void)index;
              return;
            }
          }
        }
      }
      const ir::TypeIdx place = check_place(module, reassign.place);
      const ir::TypeIdx value = check_expr(
          module, node.payload.get<ast::StmtReassign>().value, &place);
      if (verify_comp_known &&
          !comp_checked_in_scope(node.payload.get<ast::StmtReassign>().value) &&
          !expr_comp_known(module,
                           node.payload.get<ast::StmtReassign>().value)) {
        const u32 index = bag.emit(
            diag::Severity::Error, kAnalyzerNotCompKnown,
            ast.exprs[node.payload.get<ast::StmtReassign>().value].span,
            "comp assignment value is not comp-known");
        (void)index;
      }
      if (node.payload.get<ast::StmtReassign>().compound) {
        const ir::TypeTag tag = tag_of(place);
        if (!is_integer_tag(tag) && !is_float_tag(tag)) {
          const u32 index =
              bag.emit(diag::Severity::Error, kAnalyzerInvalidOperation,
                       node.span, "compound assignment needs a numeric place");
          (void)index;
          return;
        }
      }
      unify(place, value,
            ast.exprs[node.payload.get<ast::StmtReassign>().value].span,
            "assignment");
      return;
    }
    case ast::StmtKind::Expr: {
      const ir::TypeIdx type =
          check_expr(module, node.payload.get<ast::StmtExpr>().value, nullptr);
      const diag::Span span =
          ast.exprs[node.payload.get<ast::StmtExpr>().value].span;
      if (is_void(type) || is_error(type) || is_never(type)) {
        return;
      }
      if (is_must_use(type)) {
        const u32 index = bag.emit(
            diag::Severity::Warning, kAnalyzerMustUse, span,
            "unused Result/Option value; bind or discard it explicitly");
        (void)index;
        return;
      }
      const u32 index = bag.emit(
          diag::Severity::Warning, kAnalyzerMustUse, span,
          "unused non-() value; discard it explicitly with `_ := ...`");
      (void)index;
      return;
    }
  }
}
}  // namespace analyzer
