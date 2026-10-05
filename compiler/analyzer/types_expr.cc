// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/checker.h"
#include "analyzer/diag_code.h"
#include "analyzer/fmt.h"
#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "base/nesting.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
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
        if (resolved.kind != PathValue::Kind::TupleVariant) {
          const u32 index = bag.emit<i18n::Key::AnalyzerNotTupleVariant>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::TypeMismatch, node.span);
          (void)index;
          bind_error_idents(module, pattern);
          return true;
        }
        std::vector<ir::TypeIdx> payloads;
        if (!is_error(resolved.type)) {
          unify(type, resolved.type, node.span, "tuple variant pattern");
          payloads = variant_payloads(resolved, type, node.span);
        } else {
          // Generic enum: the scrutinee selects the instantiation.
          const GenericInstance* instance =
              generic_instance_for(nominal_index(resolved.enom), type);
          if (instance == nullptr) {
            const u32 index =
                bag.emit<i18n::Key::AnalyzerVariantNotInMatchedType>(
                    diag::Severity::Error, diag::Stage::Analyzer,
                    DiagCode::TypeMismatch, node.span);
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
          const u32 index = bag.emit<i18n::Key::AnalyzerVariantFieldArity>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::ArityError, node.span, payloads.size(),
              elements.size());
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
        const u32 index = bag.emit<i18n::Key::AnalyzerTuplePatternArity>(
            diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityError,
            node.span, elements.size(), tuple_type.elements.size());
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
      ir::TypeIdx struct_type = error_type();
      usize kept = type_params.size();
      if (nominal_params(*nominal).empty()) {
        struct_type = intern_nominal(*nominal);
      } else {
        const GenericInstance* instance =
            generic_instance_for(nominal_index(nominal), type);
        if (instance == nullptr) {
          const u32 index = bag.emit<i18n::Key::AnalyzerStructNotInMatchedType>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::TypeMismatch, node.span);
          (void)index;
          bind_error_idents(module, pattern);
          return false;
        }
        struct_type = instance->type;
        kept = push_generic_scope(*instance);
      }
      unify(type, struct_type, node.span, "struct pattern");
      const ast::ItemNode& decl = ast.items[nominal->item];
      const ir::StructType& struct_type_shape =
          builder.struct_types()[builder.types()[struct_type].as_struct()];
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
          const u32 diag = bag.emit<i18n::Key::AnalyzerUnknownField>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::UnknownValue, field.name.span, field.name.name);
          (void)diag;
          bind_error_idents(module, field.pattern);
          continue;
        }
        if (bind_pattern(module, field.pattern,
                         struct_type_shape.fields[index])) {
          refutable = true;
        }
      }
      pop_generic_scope(kept);
      return refutable;
    }
    case ast::PatternKind::Ref: {
      const ir::TypeTag tag = tag_of(type);
      if ((node.payload.ref.is_mut && tag != ir::TypeTag::MutRef) ||
          (!node.payload.ref.is_mut && tag != ir::TypeTag::Ref)) {
        const u32 index = bag.emit<i18n::Key::AnalyzerReferencePatternType>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::TypeMismatch, node.span);
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
                bag.emit<i18n::Key::AnalyzerOrPatternBindsDifferentNames>(
                    diag::Severity::Error, diag::Stage::Analyzer,
                    DiagCode::InvalidOperation,
                    ast.patterns[alternatives[i]].span);
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
    case ast::ExprKind::Deref:
      return expr_comp_known(module, node.payload.get<ast::ExprDeref>().inner);
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
    case ast::ExprKind::Range:
    case ast::ExprKind::Closure: return false;
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
  const u32 index = bag.emit<i18n::Key::AnalyzerTypeMismatch>(
      diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
      span, what, pretty_tag(tag_of(left)), pretty_tag(tag_of(right)));
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
    const u32 index = bag.emit<i18n::Key::AnalyzerCallArityMismatch>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityError,
        span, what, params.size() - fixed, args.size());
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
        const u32 index = bag.emit<i18n::Key::AnalyzerCompArgument>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::NotCompKnown, ast.exprs[args[i]].span);
        (void)index;
      } else {
        const u32 index = bag.emit<i18n::Key::AnalyzerCompParameterArgument>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::NotCompKnown, ast.exprs[args[i]].span);
        (void)index;
      }
    }
  }
}

// A formatting helper of the staged `fmt` package: recognized by the
// package its item is declared in, never by a user module (locals
// shadow the prelude first, and a user `write` is not staged).
bool Checker::is_core_fmt(const CheckedModule::FnSig* fn) const {
  if (fn->name != "write" && fn->name != "format") {
    return false;
  }
  return tree.is_staged_item("fmt", fn->item);
}

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
                                     ast::ExprIdx expr,
                                     std::span<const ast::TypeIdx> type_args,
                                     ast::PathIdx path,
                                     const ir::TypeIdx* expected,
                                     diag::Span span) {
  PathValue resolved;
  if (!resolve_value_path(module, path, type_args, resolved)) {
    return error_type();
  }
  switch (resolved.kind) {
    case PathValue::Kind::Local:
    case PathValue::Kind::Static:
    case PathValue::Kind::UnitVariant: {
      if (resolved.kind == PathValue::Kind::Static && comp_depth > 0 &&
          !is_literal_const(module, path)) {
        const u32 index =
            bag.emit<i18n::Key::AnalyzerStaticReadInCompEvaluation>(
                diag::Severity::Error, diag::Stage::Analyzer,
                DiagCode::InvalidComp, span);
        (void)index;
        return error_type();
      }
      if (resolved.kind == PathValue::Kind::UnitVariant) {
        ir::TypeIdx owner = resolved.type;
        if (is_error(owner)) {
          // Generic enum: the expectation selects the instantiation.
          const GenericInstance* instance =
              expected != nullptr ? generic_find(type_origin(*expected))
                                  : nullptr;
          if (instance == nullptr ||
              instance->nominal != nominal_index(resolved.enom)) {
            const u32 index = bag.emit<i18n::Key::AnalyzerCannotInferType>(
                diag::Severity::Error, diag::Stage::Analyzer,
                DiagCode::InvalidOperation, span);
            (void)index;
            return error_type();
          }
          owner = *expected;
        }
        modules[module].variants.push_back(
            {path, owner, resolved.variant, cur_inst});
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
    case PathValue::Kind::Function: {
      // A named function in value position coerces to a closure
      // value: its code with a null environment. Intrinsics lower
      // through their own path and comp parameters ride
      // specializations, so both stay arguments-only here, as do
      // generic and associated functions until callable type
      // parameters land.
      const CheckedModule::FnSig* fn = resolved.function;
      bool plain = fn->item.is_valid() &&
                   ast.items[fn->item].kind != ast::ItemKind::Intrinsic;
      for (bool flag : comp_param_flags(fn->item)) {
        plain = plain && !flag;
      }
      if (!plain) {
        const u32 index = bag.emit<i18n::Key::AnalyzerCalleeNeedsArguments>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::InvalidOperation, span);
        (void)index;
        return error_type();
      }
      ir::TypeSeq seq;
      for (ir::TypeIdx param : fn->params) {
        seq.push(storage_copy(param));
      }
      const ir::TypeIdx closure_type =
          builder.func_type(seq.finish(), storage_copy(fn->ret));
      for (u32 m = 0; m < static_cast<u32>(modules.size()); ++m) {
        for (u32 i = 0; i < static_cast<u32>(modules[m].functions.size());
             ++i) {
          if (&modules[m].functions[i] == fn) {
            modules[module].closure_fns.push_back({expr, m, i, cur_inst});
            break;
          }
        }
      }
      if (expected != nullptr) {
        return unify(*expected, closure_type, span, "path");
      }
      return closure_type;
    }
    case PathValue::Kind::GenericFn:
    case PathValue::Kind::AssocFunction:
    case PathValue::Kind::TupleVariant: {
      const u32 index = bag.emit<i18n::Key::AnalyzerCalleeNeedsArguments>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidOperation, span);
      (void)index;
      return error_type();
    }
    case PathValue::Kind::Type: {
      const u32 index = bag.emit<i18n::Key::ParserExpectedValueFoundType>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidOperation, span);
      (void)index;
      return error_type();
    }
  }
}

// Verifies a literal format string against argument element types;
// other comp-known strings verify in lowering, which holds the bytes.
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
  const FmtTemplate parsed =
      parse_format_string(unescape_format_string(literal.spelling));
  if (parsed.error != FmtError::None) {
    const u32 index = bag.emit<i18n::Key::AnalyzerInvalidFormatString>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::InvalidOperation, ast.exprs[fmt_expr].span);
    (void)index;
    return false;
  }
  if (parsed.placeholders != elements.size()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerFormatPlaceholderArity>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityError,
        ast.exprs[fmt_expr].span, parsed.placeholders, elements.size());
    (void)index;
    return false;
  }
  for (ir::TypeIdx element : elements) {
    if (!is_formattable_tag(tag_of(element))) {
      const u32 index = bag.emit<i18n::Key::CodegenArgumentNotFormattable>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidOperation, ast.exprs[args_expr].span);
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
    const u32 index = bag.emit<i18n::Key::AnalyzerWriteInCompEvaluation>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::InvalidComp,
        span);
    (void)index;
    return error_type();
  }
  if (args.size() != 3) {
    const u32 index = bag.emit<i18n::Key::AnalyzerWriteArity>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityError,
        span, args.size());
    (void)index;
    return error_type();
  }
  const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
  const ir::TypeIdx fmt_type = check_expr(module, args[0], &str);
  unify(str, fmt_type, ast.exprs[args[0]].span, "format string");
  if (!expr_comp_known(module, args[0])) {
    const u32 index = bag.emit<i18n::Key::AnalyzerFormatStringNotCompKnown>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::NotCompKnown,
        ast.exprs[args[0]].span);
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
    const u32 index = bag.emit<i18n::Key::AnalyzerBufferNotMutableSlice>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
        ast.exprs[args[1]].span);
    (void)index;
    return error_type();
  }
  const ir::TypeIdx args_type = check_expr(module, args[2], nullptr);
  // `()` reads as the empty tuple (see types.md).
  const bool empty_args =
      !is_error(args_type) && tag_of(args_type) == ir::TypeTag::Void;
  if (!is_error(args_type) && !empty_args &&
      tag_of(args_type) != ir::TypeTag::Tuple) {
    const u32 index = bag.emit<i18n::Key::AnalyzerArgumentsNotTuple>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
        ast.exprs[args[2]].span);
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
    const u32 index = bag.emit<i18n::Key::AnalyzerWriteOutcomeNotInScope>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownType,
        span);
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
    const u32 index = bag.emit<i18n::Key::AnalyzerFormatInCompEvaluation>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::InvalidComp,
        span);
    (void)index;
    return error_type();
  }
  if (args.size() != 2) {
    const u32 index = bag.emit<i18n::Key::AnalyzerFormatArity>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityError,
        span, args.size());
    (void)index;
    return error_type();
  }
  const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
  const ir::TypeIdx fmt_type = check_expr(module, args[0], &str);
  unify(str, fmt_type, ast.exprs[args[0]].span, "format string");
  if (!expr_comp_known(module, args[0])) {
    const u32 index = bag.emit<i18n::Key::AnalyzerFormatStringNotCompKnown>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::NotCompKnown,
        ast.exprs[args[0]].span);
    (void)index;
    return error_type();
  }
  const ir::TypeIdx args_type = check_expr(module, args[1], nullptr);
  const bool empty_args =
      !is_error(args_type) && tag_of(args_type) == ir::TypeTag::Void;
  if (!is_error(args_type) && !empty_args &&
      tag_of(args_type) != ir::TypeTag::Tuple) {
    const u32 index = bag.emit<i18n::Key::AnalyzerArgumentsNotTuple>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
        ast.exprs[args[1]].span);
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
    const u32 index = bag.emit<i18n::Key::AnalyzerStringNotInScope>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownType,
        span);
    (void)index;
    return error_type();
  }
  const ir::TypeIdx result = intern_nominal(*string_type);
  if (expected != nullptr) {
    return unify(*expected, result, span, "call");
  }
  return result;
}

// A closure literal: parameters bind from annotations or the
// expected function type, a non-empty capture list is refused
// until captures land, and the body checks as a function body
// with its own scope and return slot. The value's type is the
// signature they make.
ir::TypeIdx Checker::check_closure(u32 module,
                                   ast::ExprIdx expr,
                                   const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprClosure& closure = node.payload.get<ast::ExprClosure>();
  // The expectation's signature, copied out before resolving
  // anything: interning below may move the tables it borrows.
  std::vector<ir::TypeIdx> expected_params;
  ir::TypeIdx expected_ret = error_type();
  bool has_expected_sig = false;
  if (expected != nullptr && !is_error(*expected) &&
      tag_of(*expected) == ir::TypeTag::Func) {
    const ir::FuncType& sig =
        builder.func_types()[builder.types()[*expected].as_func()];
    for (ir::TypeIdx param : sig.params) {
      expected_params.push_back(param);
    }
    expected_ret = sig.ret;
    has_expected_sig = true;
  }
  // Captures, checked before the closure's own scope opens: a name
  // here can only be an outer one. The list declares the modes, so
  // there is nothing to infer; the checks are that each name exists
  // where the closure is written, that the mode is available, and
  // that the body leaves no declared capture unused.
  std::vector<CaptureEntry> entries;
  entries.reserve(closure.captures.size());
  // The scope the closure body may see: inside an enclosing closure,
  // only that closure's bindings, its captures, and what is visible
  // there in turn.
  const usize visible_from =
      closure_bounds.empty() ? 0 : closure_bounds.back().scope;
  for (const ast::Capture& capture : closure.captures) {
    const std::string_view name = capture.name.name;
    bool duplicate = false;
    for (const CaptureEntry& prior : entries) {
      if (prior.name == name) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      const u32 index = bag.emit<i18n::Key::AnalyzerCaptureDuplicate>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidCapture, capture.name.span, name);
      (void)index;
      continue;
    }
    const Local* local = lookup_local(name);
    if (local == nullptr || scope_of(name) < visible_from) {
      // Not a local at all, or one an enclosing closure does not
      // capture: the env of this closure is built inside that
      // closure, so the name must be captured level by level.
      const bool nested = local != nullptr;
      const u32 index =
          nested ? bag.emit<i18n::Key::AnalyzerCaptureNotVisible>(
                       diag::Severity::Error, diag::Stage::Analyzer,
                       DiagCode::InvalidCapture, capture.name.span, name)
                 : bag.emit<i18n::Key::AnalyzerCaptureUnknown>(
                       diag::Severity::Error, diag::Stage::Analyzer,
                       DiagCode::InvalidCapture, capture.name.span, name);
      (void)index;
      continue;
    }
    if (local->comp_known) {
      const u32 index = bag.emit<i18n::Key::AnalyzerCaptureCompBinding>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidCapture, capture.name.span, name);
      (void)index;
      continue;
    }
    if (capture.mode == ast::CaptureMode::Mut && !local->is_mut) {
      const u32 index = bag.emit<i18n::Key::AnalyzerCaptureNeedsMut>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidCapture, capture.name.span, name);
      (void)index;
    }
    if (capture.mode == ast::CaptureMode::Move) {
      std::vector<u32> visited;
      if (!capture_is_copy(local->type, visited)) {
        const u32 index = bag.emit<i18n::Key::AnalyzerMoveCaptureUnsupported>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::InvalidCapture, capture.name.span, name);
        (void)index;
      }
    }
    // A name the enclosing closure binds is a use of its capture:
    // the inner environment is built from the outer one.
    if (!closure_bounds.empty() && local->capture != NO_CAPTURE &&
        local->capture < closure_bounds.back().captures.size()) {
      closure_bounds.back().captures[local->capture].used = true;
    }
    entries.push_back(
        {name, capture.mode, local->type, capture.name.span, false});
  }
  // Parameter types: annotations first, expectation second, and
  // an annotation request when neither names one.
  std::vector<ir::TypeIdx> param_types;
  param_types.reserve(closure.params.size());
  bool params_ok = true;
  if (has_expected_sig && expected_params.size() != closure.params.size()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerClosureArityMismatch>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityMismatch,
        node.span, expected_params.size(), closure.params.size());
    (void)index;
    params_ok = false;
  }
  for (usize i = 0; i < closure.params.size(); ++i) {
    const ast::ClosureParam& param = closure.params[i];
    if (param.type.is_valid()) {
      param_types.push_back(resolve_type(module, param.type, nullptr));
    } else if (params_ok && has_expected_sig) {
      param_types.push_back(expected_params[i]);
    } else {
      const u32 index =
          bag.emit<i18n::Key::AnalyzerClosureParamNeedsAnnotation>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::InvalidOperation, param.name.span, param.name.name);
      (void)index;
      param_types.push_back(error_type());
    }
  }
  // The body checks as a function body: its own scope and return
  // slot, with loops reset so `break` cannot cross the boundary.
  // `ret` targets the closure, which is why the slot is saved. An
  // erroneous slot takes the first return it sees, so returns meet
  // the trailing value below; a named expectation anchors both.
  const bool saved_in_fn = in_fn;
  const u32 saved_loop = loop_depth;
  const ir::TypeIdx saved_ret = fn_ret;
  in_fn = true;
  loop_depth = 0;
  fn_ret = has_expected_sig ? expected_ret : error_type();
  scopes.emplace_back();
  // Capture bindings first, so a parameter of the same name shadows
  // them and the capture is reported unused, which is what a list
  // entry the body cannot reach means. A `&` capture reads; the
  // other modes own their place in the environment and may write it.
  for (usize i = 0; i < entries.size(); ++i) {
    const CaptureEntry& entry = entries[i];
    scopes.back().push_back({entry.name, entry.type,
                             entry.mode != ast::CaptureMode::Shared, false,
                             static_cast<u32>(i)});
  }
  for (usize i = 0; i < closure.params.size(); ++i) {
    const ast::ClosureParam& param = closure.params[i];
    if (param.is_wildcard) {
      continue;
    }
    scopes.back().push_back(
        {param.name.name, param_types[i], param.is_mut, false});
  }
  closure_bounds.push_back({scopes.size() - 1, std::move(entries)});
  const ir::TypeIdx* body_expected = has_expected_sig ? &expected_ret : nullptr;
  const ir::TypeIdx body_type = check_expr(module, closure.body, body_expected);
  const ir::TypeIdx ret = has_expected_sig
                              ? expected_ret
                              : unify(fn_ret, body_type, node.span, "closure");
  std::vector<CaptureEntry> declared =
      std::move(closure_bounds.back().captures);
  closure_bounds.pop_back();
  scopes.pop_back();
  in_fn = saved_in_fn;
  loop_depth = saved_loop;
  fn_ret = saved_ret;
  for (const CaptureEntry& entry : declared) {
    if (!entry.used) {
      const u32 index = bag.emit<i18n::Key::AnalyzerCaptureUnused>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidCapture, entry.span, entry.name);
      (void)index;
    }
  }
  ir::TypeSeq seq;
  for (ir::TypeIdx param : param_types) {
    seq.push(storage_copy(param));
  }
  const ir::TypeIdx closure_type =
      builder.func_type(seq.finish(), storage_copy(ret));
  CheckedModule::ClosureLit lit{expr, {}, ret, closure.body, cur_inst, {}};
  for (usize i = 0; i < closure.params.size(); ++i) {
    const ast::ClosureParam& param = closure.params[i];
    if (param.is_wildcard) {
      continue;
    }
    lit.params.push_back(
        {param.name.name, param_types[i], param.is_mut, static_cast<u32>(i)});
  }
  for (const CaptureEntry& entry : declared) {
    lit.captures.push_back({entry.name, entry.mode, entry.type});
  }
  modules[module].closures.push_back(std::move(lit));
  if (expected != nullptr) {
    return unify(*expected, closure_type, node.span, "closure");
  }
  return closure_type;
}

// A call through a function value: arity and argument types unify
// against the signature, and the call records for lowering, which
// emits it indirectly.
ir::TypeIdx Checker::check_indirect_call(u32 module,
                                         ast::ExprIdx expr,
                                         ast::ExprIdx callee,
                                         const ir::FuncType& sig,
                                         const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  const diag::Span span = node.span;
  // Copied out before checking anything: argument checking
  // interns types and may move the tables the signature borrows.
  std::vector<ir::TypeIdx> params;
  params.reserve(sig.params.size());
  for (ir::TypeIdx param : sig.params) {
    params.push_back(param);
  }
  const ir::TypeIdx ret = sig.ret;
  // The diagnostics name the callee when it is a plain local, and
  // the shape otherwise.
  std::string_view what = "closure";
  if (ast.exprs[callee].kind == ast::ExprKind::Path) {
    const ast::Path& path =
        ast.paths[ast.exprs[callee].payload.get<ast::ExprPath>().idx];
    if (path.segments.size() == 1) {
      what = path.segments[0].name;
    }
  }
  check_call_args(module, args, params, {}, span, what, false);
  if (args.size() != params.size()) {
    return error_type();
  }
  modules[module].indirect_calls.push_back({callee, cur_inst});
  if (expected != nullptr) {
    return unify(*expected, ret, span, "call");
  }
  return ret;
}

// Calls through a resolved callee path: free and associated
// functions, tuple variant constructors, and the `print` intrinsic.
ir::TypeIdx Checker::check_call(u32 module,
                                ast::ExprIdx expr,
                                const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprIdx callee = node.payload.get<ast::ExprCall>().callee;
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  const diag::Span span = node.span;
  if (ast.exprs[callee].kind != ast::ExprKind::Path) {
    // A closure literal in callee position calls through its value;
    // anything else is not callable.
    const ir::TypeIdx callee_type = check_expr(module, callee, nullptr);
    if (is_error(callee_type)) {
      return error_type();
    }
    if (tag_of(callee_type) == ir::TypeTag::Func) {
      const ir::FuncType& sig =
          builder.func_types()[builder.types()[callee_type].as_func()];
      return check_indirect_call(module, expr, callee, sig, expected);
    }
    const u32 index = bag.emit<i18n::Key::AnalyzerCallNonPath>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::InvalidOperation, ast.exprs[callee].span);
    (void)index;
    for (ast::ExprIdx arg : args) {
      check_expr(module, arg, nullptr);
    }
    return error_type();
  }
  // No fallback for `print`, `println`, or `panic`: an unresolved name
  // is an error like any other, so every name in scope traces to a
  // manifest entry. The prelude provides them whenever `core` is
  // selected, and the missing-dependency hint names the package when it
  // is not.
  const ast::PathIdx path = ast.exprs[callee].payload.get<ast::ExprPath>().idx;
  PathValue resolved;
  if (!resolve_value_path(
          module, path,
          ast.exprs[callee].payload.get<ast::ExprPath>().type_args, resolved)) {
    for (ast::ExprIdx arg : args) {
      check_expr(module, arg, nullptr);
    }
    return error_type();
  }
  if (resolved.kind == PathValue::Kind::GenericFn) {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    const CheckedModule::FnSig* fn = resolve_generic_fn(
        module, resolved.generic_item, call.args, call.type_args, span);
    if (fn == nullptr) {
      for (ast::ExprIdx arg : call.args) {
        check_expr(module, arg, nullptr);
      }
      return error_type();
    }
    record_call(module, callee, fn);
    check_call_args(module, call.args, fn->params, comp_param_flags(fn->item),
                    span, fn->name, false);
    if (expected != nullptr) {
      return unify(*expected, fn->ret, span, "call");
    }
    return fn->ret;
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
  if (resolved.kind == PathValue::Kind::TupleVariant) {
    ir::TypeIdx enum_type = resolved.type;
    std::vector<ir::TypeIdx> payloads;
    if (!is_error(enum_type)) {
      payloads = variant_payloads(resolved, enum_type, span);
    } else {
      // Generic enum: the expectation selects the instantiation. With
      // no expectation, a payload whose declared type is exactly a type
      // parameter binds that parameter to the argument's type.
      const u32 nominal = nominal_index(resolved.enom);
      const GenericInstance* instance =
          expected != nullptr ? generic_find(type_origin(*expected)) : nullptr;
      if (instance == nullptr || instance->nominal != nominal) {
        instance = infer_from_payload_args(module, nominal, resolved, args);
      }
      if (instance == nullptr) {
        const u32 index = bag.emit<i18n::Key::AnalyzerCannotInferType>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::InvalidOperation, span);
        (void)index;
        for (ast::ExprIdx arg : args) {
          check_expr(module, arg, nullptr);
        }
        return error_type();
      }
      const usize kept = push_generic_scope(*instance);
      payloads = variant_payloads(resolved, instance->type, span);
      pop_generic_scope(kept);
      enum_type = instance->type;
    }
    modules[module].variants.push_back(
        {path, enum_type, resolved.variant, cur_inst});
    if (args.size() != payloads.size()) {
      const u32 index = bag.emit<i18n::Key::AnalyzerVariantCallArity>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityError,
          span, payloads.size(), args.size());
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
        const u32 index = bag.emit<i18n::Key::AnalyzerCompArgument>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::NotCompKnown, ast.exprs[args[i]].span);
        (void)index;
      }
    }
    if (expected != nullptr) {
      return unify(*expected, enum_type, span, "call");
    }
    return enum_type;
  }
  // A local or static holding a function value calls through it;
  // anything else of non-function type is not callable.
  if ((resolved.kind == PathValue::Kind::Local ||
       resolved.kind == PathValue::Kind::Static) &&
      !is_error(resolved.type) && tag_of(resolved.type) == ir::TypeTag::Func) {
    const ir::FuncType& sig =
        builder.func_types()[builder.types()[resolved.type].as_func()];
    return check_indirect_call(module, expr, callee, sig, expected);
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerNotCallable>(
      diag::Severity::Error, diag::Stage::Analyzer, DiagCode::InvalidOperation,
      span);
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
  ir::TypeIdx receiver = check_expr(
      module, node.payload.get<ast::ExprMethodCall>().receiver, nullptr);
  if (is_error(receiver)) {
    for (ast::ExprIdx arg : node.payload.get<ast::ExprMethodCall>().args) {
      check_expr(module, arg, nullptr);
    }
    return error_type();
  }
  // A field's type is a storage copy of its declaration while the
  // method entry is keyed on the declared type, so look through the
  // copy. Values without one pass through unchanged.
  receiver = type_origin(receiver);
  const std::string_view name =
      node.payload.get<ast::ExprMethodCall>().name.name;
  const diag::Span span = node.span;
  ir::TypeIdx nominal = receiver;
  const ir::TypeTag tag = tag_of(receiver);
  if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
    nominal = builder.ref_types()[builder.types()[receiver].as_ref()].pointee;
  }
  const bool spec_only = node.payload.get<ast::ExprMethodCall>().spec_only;
  const CheckedModule::MethodInfo* method =
      lookup_method(nominal, name, module, node.span, spec_only);
  if (method == nullptr) {
    if (spec_only) {
      // The `for` desugar's generated `next` reaches the cursor
      // through a spec or not at all, so an inherent method of the
      // same name cannot stand in for `Iterator`.
      const u32 index = bag.emit<i18n::Key::AnalyzerForRequiresIterator>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownValue,
          span);
      (void)index;
    } else {
      const u32 index = bag.emit<i18n::Key::AnalyzerUnknownMethod>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownValue,
          node.payload.get<ast::ExprMethodCall>().name.span, name);
      (void)index;
    }
    for (ast::ExprIdx arg : node.payload.get<ast::ExprMethodCall>().args) {
      check_expr(module, arg, nullptr);
    }
    return error_type();
  }
  if (method->receiver == CheckedModule::ReceiverKind::None) {
    const u32 index = bag.emit<i18n::Key::AnalyzerAssociatedFunctionAsMethod>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::InvalidOperation,
        node.payload.get<ast::ExprMethodCall>().name.span, name);
    (void)index;
    return error_type();
  }
  record_call(module, expr, method);
  // The receiver coerces to a declared borrow; coercion itself is
  // erased at this level and materialized by lowering.
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
    } else if (coerces_to_shared(declared, receiver)) {
      // `&mut T` reaching a `&Self` receiver is a shared reborrow, the
      // same coercion an argument position gets.
      coerced = true;
    }
    if (!coerced) {
      const u32 index = bag.emit<i18n::Key::AnalyzerReceiverTypeMismatch>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
          ast.exprs[node.payload.get<ast::ExprMethodCall>().receiver].span);
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
    // A field's type is a storage copy of the declared type, so owner
    // lookup follows the copy back to its origin.
    const ir::TypeIdx origin = type_origin(receiver);
    for (NominalEntry& entry : nominals) {
      if (!entry.complete || entry.type.idx != origin.idx) {
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
      // A generic struct instance carries no NominalEntry; its
      // declaration lives on the owning nominal.
      for (const GenericInstance& instance : generic_instances) {
        if (instance.type.idx != origin.idx) {
          continue;
        }
        owner = &nominals[instance.nominal];
        break;
      }
      if (owner != nullptr) {
        u32 i = 0;
        for (const ast::ItemStructField& decl_field :
             ast.items[owner->item].payload.get<ast::ItemStruct>().fields) {
          if (decl_field.name.name == field_name.name) {
            field_index = i;
            break;
          }
          ++i;
        }
      }
    }
    if (owner == nullptr) {
      const u32 index = bag.emit<i18n::Key::AnalyzerUnknownField>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownValue,
          field_name.span, field_name.name);
      (void)index;
      return error_type();
    }
    // A missing name on a resolved owner must not index past the end.
    bool named = false;
    for (const ast::ItemStructField& decl_field :
         ast.items[owner->item].payload.get<ast::ItemStruct>().fields) {
      if (decl_field.name.name == field_name.name) {
        named = true;
        break;
      }
    }
    if (!named) {
      const u32 index = bag.emit<i18n::Key::AnalyzerUnknownField>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownValue,
          field_name.span, field_name.name);
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
      const u32 diag = bag.emit<i18n::Key::AnalyzerUnknownTupleField>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownValue,
          field_name.span, field_name.name);
      (void)diag;
      return error_type();
    }
    const ir::TypeIdx result = tuple_type.elements[index];
    if (expected != nullptr) {
      return unify(*expected, result, node.span, "field");
    }
    return result;
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerNoFields>(
      diag::Severity::Error, diag::Stage::Analyzer, DiagCode::InvalidOperation,
      node.span, pretty_tag(tag));
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
  ir::TypeIdx struct_type = error_type();
  const GenericInstance* instance = nullptr;
  if (nominal_params(*nominal).empty()) {
    struct_type = intern_nominal(*nominal);
  } else {
    // A generic struct takes its instantiation from the expectation;
    // there is no path-level type argument to carry it. The expectation
    // may be a field storage copy, so it is followed to its origin.
    instance =
        expected != nullptr ? generic_find(type_origin(*expected)) : nullptr;
    if (instance == nullptr || instance->nominal != nominal_index(nominal)) {
      const u32 index = bag.emit<i18n::Key::AnalyzerCannotInferType>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidOperation, node.span);
      (void)index;
      for (const ast::ExprFieldInit& field :
           node.payload.get<ast::ExprStruct>().init) {
        check_expr(module, field.value, nullptr);
      }
      return error_type();
    }
    struct_type = instance->type;
  }
  const usize kept =
      instance == nullptr ? type_params.size() : push_generic_scope(*instance);
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
      const u32 diag = bag.emit<i18n::Key::AnalyzerUnknownField>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownValue,
          field.name.span, field.name.name);
      (void)diag;
      check_expr(module, field.value, nullptr);
      continue;
    }
    if (seen[index]) {
      const u32 diag = bag.emit<i18n::Key::AnalyzerDuplicateField>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::DuplicateDefinition, field.name.span, field.name.name);
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
        const u32 diag = bag.emit<i18n::Key::AnalyzerMissingField>(
            diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityError,
            node.span, decl.payload.get<ast::ItemStruct>().fields[i].name.name);
        (void)diag;
      }
    }
  }
  if (expected != nullptr) {
    const ir::TypeIdx result =
        unify(*expected, struct_type, node.span, "struct");
    pop_generic_scope(kept);
    return result;
  }
  pop_generic_scope(kept);
  return struct_type;
}

// `?` propagates within one enum type: the scrutinee and the enclosing
// return type must be the same type. The first variant yields its
// first payload; any other variant returns the scrutinee unchanged.
// See docs/adr/0009-result-option-library-enums.md.
ir::TypeIdx Checker::check_question(u32 module,
                                    ast::ExprIdx expr,
                                    const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ir::TypeIdx inner =
      check_expr(module, node.payload.get<ast::ExprQuestion>().inner, nullptr);
  if (is_error(inner)) {
    return error_type();
  }
  if (tag_of(inner) != ir::TypeTag::Enum) {
    const u32 index = bag.emit<i18n::Key::AnalyzerTryNeedsEnumOperand>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::BadQuestion,
        node.span);
    (void)index;
    return error_type();
  }
  if (tag_of(fn_ret) != ir::TypeTag::Enum) {
    const u32 index = bag.emit<i18n::Key::AnalyzerTryNeedsEnclosingFunction>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::BadQuestion,
        node.span);
    (void)index;
    return error_type();
  }
  if (inner.idx != fn_ret.idx) {
    const u32 index = bag.emit<i18n::Key::AnalyzerTryTypeMismatch>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::BadQuestion,
        node.span);
    (void)index;
    return error_type();
  }
  const ir::EnumType& shape =
      builder.enum_types()[builder.types()[inner].as_enum()];
  if (shape.variants.size() < 2) {
    const u32 index = bag.emit<i18n::Key::AnalyzerTryNeedsSuccessFailure>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::BadQuestion,
        node.span);
    (void)index;
    return error_type();
  }
  const ir::EnumVariantType& first =
      builder.enum_variant_types()[shape.variants.head()];
  if (first.fields.empty()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerTryNeedsValueVariant>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::BadQuestion,
        node.span);
    (void)index;
    return error_type();
  }
  const ir::TypeIdx ok = first.fields.head();
  if (expected != nullptr) {
    return unify(*expected, ok, node.span, "'?'");
  }
  return ok;
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
  const u32 index = bag.emit<i18n::Key::AnalyzerInvalidCast>(
      diag::Severity::Error, diag::Stage::Analyzer, DiagCode::InvalidOperation,
      node.span, pretty_tag(from), pretty_tag(to));
  (void)index;
  return error_type();
}

ir::TypeIdx Checker::check_index(u32 module,
                                 ast::ExprIdx expr,
                                 const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprIndex& index = node.payload.get<ast::ExprIndex>();
  const ir::TypeIdx receiver = check_expr(module, index.receiver, nullptr);
  const ir::TypeIdx position = check_expr(module, index.index, nullptr);
  if (is_error(receiver) || is_error(position)) {
    return error_type();
  }
  ir::TypeIdx element = error_type();
  if (range_element(position, element)) {
    bool unsized = false;
    const ir::TypeIdx run =
        check_run_index(receiver, element, expected, node.span, unsized);
    if (is_error(run)) {
      return error_type();
    }
    if (unsized) {
      // `[E]` is unsized, so a bare run of a fixed array cannot be a
      // value: the borrow is the spelling that names it.
      const u32 diag = bag.emit<i18n::Key::AnalyzerArrayRunNeedsBorrow>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidOperation, node.span);
      (void)diag;
      return error_type();
    }
    return run;
  }
  return check_element_index(receiver, position, index.index, expected,
                             node.span);
}

ir::TypeIdx Checker::check_element_index(ir::TypeIdx receiver,
                                         ir::TypeIdx position,
                                         ast::ExprIdx index_expr,
                                         const ir::TypeIdx* expected,
                                         diag::Span span) {
  if (!is_integer_tag(tag_of(position))) {
    const u32 diag = bag.emit<i18n::Key::AnalyzerArrayIndexNotInteger>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
        ast.exprs[index_expr].span);
    (void)diag;
    return error_type();
  }
  if (tag_of(receiver) == ir::TypeTag::Array) {
    const ir::ArrayType& array =
        builder.array_types()[builder.types()[receiver].as_array()];
    if (expected != nullptr) {
      return unify(*expected, array.element, span, "index");
    }
    return array.element;
  }
  if (tag_of(receiver) == ir::TypeTag::Ref ||
      tag_of(receiver) == ir::TypeTag::MutRef) {
    const ir::TypeIdx pointee =
        builder.ref_types()[builder.types()[receiver].as_ref()].pointee;
    if (tag_of(pointee) == ir::TypeTag::Slice) {
      const ir::SliceType& slice =
          builder.slice_types()[builder.types()[pointee].as_slice()];
      if (expected != nullptr) {
        return unify(*expected, slice.element, span, "index");
      }
      return slice.element;
    }
  }
  const u32 diag = bag.emit<i18n::Key::AnalyzerCannotIndex>(
      diag::Severity::Error, diag::Stage::Analyzer, DiagCode::InvalidOperation,
      span, pretty_tag(tag_of(receiver)));
  (void)diag;
  return error_type();
}

ir::TypeIdx Checker::check_run_index(ir::TypeIdx receiver,
                                     ir::TypeIdx element,
                                     const ir::TypeIdx* expected,
                                     diag::Span span,
                                     bool& unsized) {
  unsized = false;
  if (!is_integer_tag(tag_of(element))) {
    const u32 diag = bag.emit<i18n::Key::AnalyzerRangeEndpointNotInteger>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
        span);
    (void)diag;
    return error_type();
  }
  const ir::TypeTag tag = tag_of(receiver);
  if (tag == ir::TypeTag::Array) {
    const ir::ArrayType& array =
        builder.array_types()[builder.types()[receiver].as_array()];
    unsized = true;
    const ir::TypeIdx run = builder.slice_type(array.element);
    if (expected != nullptr) {
      return unify(*expected, run, span, "index");
    }
    return run;
  }
  if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
    const ir::TypeIdx pointee =
        builder.ref_types()[builder.types()[receiver].as_ref()].pointee;
    if (tag_of(pointee) == ir::TypeTag::Slice) {
      // Re-slicing keeps the view's kind: `sl[1..]` on `&mut [T]`
      // still writes.
      if (expected != nullptr) {
        return unify(*expected, receiver, span, "index");
      }
      return receiver;
    }
  }
  if (tag == ir::TypeTag::Str) {
    const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
    if (expected != nullptr) {
      return unify(*expected, str, span, "index");
    }
    return str;
  }
  const u32 diag = bag.emit<i18n::Key::AnalyzerCannotIndexWithRange>(
      diag::Severity::Error, diag::Stage::Analyzer, DiagCode::InvalidOperation,
      span, pretty_tag(tag));
  (void)diag;
  return error_type();
}

// A range expression is `Range<T>` data. The element type comes from
// the endpoints: an expected `Range<E>` pins it, otherwise the present
// endpoints agree, with a bare integer literal adapting to the other
// side exactly as it does in a binary operation. With neither endpoint
// present (`..`) the language's unsuffixed default applies.
ir::TypeIdx Checker::check_range(u32 module,
                                 ast::ExprIdx expr,
                                 const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprRange& range = node.payload.get<ast::ExprRange>();
  ir::TypeIdx hint = error_type();
  const ir::TypeIdx* endpoint_expected = nullptr;
  if (expected != nullptr) {
    ir::TypeIdx element = error_type();
    if (range_element(*expected, element) && !is_error(element)) {
      hint = element;
      endpoint_expected = &hint;
    }
  }
  ir::TypeIdx start = error_type();
  if (range.start.is_valid()) {
    start = check_expr(module, range.start, endpoint_expected);
  }
  ir::TypeIdx end = error_type();
  if (range.end.is_valid()) {
    end = check_expr(module, range.end, endpoint_expected);
  }
  if (range.start.is_valid() && range.end.is_valid() && !is_error(start) &&
      !is_error(end) && !types_equal(start, end)) {
    if (is_integer_tag(tag_of(end)) && is_bare_int_literal(range.start)) {
      start = check_expr(module, range.start, &end);
    } else if (is_integer_tag(tag_of(start)) &&
               is_bare_int_literal(range.end)) {
      end = check_expr(module, range.end, &start);
    }
  }
  if (range.start.is_valid() && is_error(start)) {
    return error_type();
  }
  if (range.end.is_valid() && is_error(end)) {
    return error_type();
  }
  ir::TypeIdx element = error_type();
  if (range.start.is_valid()) {
    element = start;
  } else if (range.end.is_valid()) {
    element = end;
  } else {
    element = builder.primitive(ir::TypeTag::I32);
  }
  if (range.start.is_valid() && range.end.is_valid() &&
      !types_equal(start, end)) {
    const u32 diag = bag.emit<i18n::Key::AnalyzerRangeEndpointTypeMismatch>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
        node.span, pretty_tag(tag_of(start)), pretty_tag(tag_of(end)));
    (void)diag;
    return error_type();
  }
  NominalEntry* entry = builtin_nominal("Range");
  if (entry == nullptr) {
    const u32 diag = bag.emit<i18n::Key::AnalyzerRangeNeedsStdCore>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::UnknownType,
        node.span);
    (void)diag;
    return error_type();
  }
  return instantiate_generic(nominal_index(entry), {element}, node.span);
}

bool Checker::range_element(ir::TypeIdx type, ir::TypeIdx& element) const {
  const GenericInstance* instance = generic_find(type_origin(type));
  if (instance == nullptr || instance->args.size() != 1) {
    return false;
  }
  const NominalEntry& entry = nominals[instance->nominal];
  if (entry.name != "Range") {
    return false;
  }
  element = instance->args[0];
  return true;
}

// `&a[run]`: a fixed array's run has no place of its own, so it never
// reaches `check_index`, which would reject the bare form. Checking
// the operands here lets the borrow name the run instead. A view is
// already a reference, so borrowing one has no place behind it and is
// rejected.
ir::TypeIdx Checker::check_borrow_of_index(u32 module,
                                           ast::ExprIdx expr,
                                           const ast::ExprBorrow& borrow,
                                           const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprIndex& index =
      ast.exprs[borrow.inner].payload.get<ast::ExprIndex>();
  const diag::Span span = ast.exprs[borrow.inner].span;
  const ir::TypeIdx receiver = check_expr(module, index.receiver, nullptr);
  const ir::TypeIdx position = check_expr(module, index.index, nullptr);
  if (is_error(receiver) || is_error(position)) {
    return error_type();
  }
  ir::TypeIdx element = error_type();
  ir::TypeIdx pointee = error_type();
  if (range_element(position, element)) {
    bool unsized = false;
    pointee = check_run_index(receiver, element, nullptr, span, unsized);
    if (is_error(pointee)) {
      return error_type();
    }
    if (!unsized) {
      const u32 diag = bag.emit<i18n::Key::AnalyzerViewIsAlreadyReference>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidOperation, span);
      (void)diag;
      return error_type();
    }
  } else {
    pointee =
        check_element_index(receiver, position, index.index, nullptr, span);
    if (is_error(pointee)) {
      return error_type();
    }
  }
  // The operands were checked here rather than through `check_expr` on
  // the index, so record its own type for lowering.
  modules[module].expr_types.push_back({borrow.inner, pointee, cur_inst});
  const ir::TypeIdx type = builder.reference_type(pointee, borrow.is_mut);
  if (expected != nullptr) {
    return unify(*expected, type, node.span, "borrow");
  }
  return type;
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
      const u32 index = bag.emit<i18n::Key::AnalyzerCompConditionInitializer>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::NotCompKnown,
          ast.exprs[node.init].span);
      (void)index;
    }
    return;
  }
  const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
  const ir::TypeIdx actual = check_expr(module, node.value, &boolean);
  unify(boolean, actual, ast.exprs[node.value].span, "condition");
  if (verify_comp_known && !comp_checked_in_scope(node.value) &&
      !expr_comp_known(module, node.value)) {
    const u32 index = bag.emit<i18n::Key::AnalyzerCompCondition>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::NotCompKnown,
        ast.exprs[node.value].span);
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
      const u32 index = bag.emit<i18n::Key::AnalyzerIfWithoutElse>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TypeMismatch,
          node.span);
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
  (void)module;
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
      const u32 index = bag.emit<i18n::Key::AnalyzerMatchMissingVariant>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::NonExhaustiveMatch, span,
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
        const u32 index = bag.emit<i18n::Key::AnalyzerMatchNotCovered>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::NonExhaustiveMatch, span, variants[i].name.name);
        (void)index;
        return;
      }
    }
    return;
  }
  if (is_integer_tag(tag)) {
    const u32 index = bag.emit<i18n::Key::AnalyzerMatchNotExhaustiveInteger>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::NonExhaustiveMatch, span);
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
    const u32 index = bag.emit<i18n::Key::AnalyzerMatchNotExhaustiveTuple>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::NonExhaustiveMatch, span);
    (void)index;
    return;
  }
  if (tag == ir::TypeTag::Struct) {
    for (const ast::ExprMatchArm& arm : arms) {
      if (ast.patterns[arm.pattern].kind == ast::PatternKind::Struct) {
        return;
      }
    }
    const u32 index = bag.emit<i18n::Key::AnalyzerMatchNotExhaustiveStruct>(
        diag::Severity::Error, diag::Stage::Analyzer,
        DiagCode::NonExhaustiveMatch, span);
    (void)index;
    return;
  }
  const u32 index = bag.emit<i18n::Key::AnalyzerMatchNotExhaustive>(
      diag::Severity::Error, diag::Stage::Analyzer,
      DiagCode::NonExhaustiveMatch, span);
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

ir::TypeIdx Checker::check_match(u32 module,
                                 ast::ExprIdx expr,
                                 const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprIdx scrutinee_expr =
      node.payload.get<ast::ExprMatch>().scrutinee;
  const ir::TypeIdx scrutinee = check_expr(module, scrutinee_expr, nullptr);
  if (verify_comp_known && !comp_checked_in_scope(scrutinee_expr) &&
      !expr_comp_known(module, scrutinee_expr)) {
    const u32 index = bag.emit<i18n::Key::AnalyzerCompMatchScrutinee>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::NotCompKnown,
        ast.exprs[scrutinee_expr].span);
    (void)index;
  }
  ir::TypeIdx result = error_type();
  bool first = true;
  for (const ast::ExprMatchArm& arm : node.payload.get<ast::ExprMatch>().arms) {
    scopes.emplace_back();
    if (!is_error(scrutinee)) {
      const bool refutable = bind_pattern(module, arm.pattern, scrutinee);
      // The `for` desugar marks its `Some` arm: the item pattern must
      // match every item, so a refutable one is an error here rather
      // than a filter. Hand-written matches keep lowering's verdict.
      if (refutable && ast.patterns[arm.pattern].for_pattern) {
        const u32 index = bag.emit<i18n::Key::AnalyzerRefutablePatternInFor>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::RefutableLet, ast.patterns[arm.pattern].span);
        (void)index;
      }
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

void Checker::report_too_deep(diag::Span span) {
  if (reported_too_deep_) {
    return;
  }
  reported_too_deep_ = true;
  const u32 index = bag.emit<i18n::Key::ParserNestingTooDeep>(
      diag::Severity::Error, diag::Stage::Analyzer, DiagCode::TooDeep, span,
      nesting_.limit());
  (void)index;
}

ir::TypeIdx Checker::check_expr(u32 module,
                                ast::ExprIdx expr,
                                const ir::TypeIdx* expected) {
  // Every expression routes through here, so this one check bounds the
  // whole walk. The type is still recorded, so the table stays complete
  // and a later lookup of this expression finds an error rather than
  // nothing.
  const ir::TypeIdx type =
      nesting_.exhausted()
          ? (report_too_deep(ast.exprs[expr].span), error_type())
          : [&] {
              const base::NestingScope scope(nesting_);
              return check_expr_inner(module, expr, expected);
            }();
  modules[module].expr_types.push_back({expr, type, cur_inst});
  return type;
}

ir::TypeIdx Checker::check_array(u32 module,
                                 ast::ExprIdx expr,
                                 const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprArray& array = node.payload.get<ast::ExprArray>();
  // Bounds compiler-time expansion: repeat counts are unbounded
  // literals, and lowering stores per element.
  static constexpr u64 MAX_ARRAY_ELEMENTS = 1u << 20;
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
    if (array.count > MAX_ARRAY_ELEMENTS) {
      const u32 index = bag.emit<i18n::Key::AnalyzerArrayRepeatTooLarge>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidOperation, node.span, array.count,
          MAX_ARRAY_ELEMENTS);
      (void)index;
      return error_type();
    }
    const ir::TypeIdx element =
        check_expr(module, array.repeat, element_expected);
    if (has_expected_count && expected_count != array.count) {
      const u32 index = bag.emit<i18n::Key::AnalyzerArrayRepeatArity>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityError,
          node.span, expected_count, array.count);
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
    const u32 index = bag.emit<i18n::Key::AnalyzerArrayLiteralWithoutCount>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityError,
        node.span);
    (void)index;
    return error_type();
  }
  if (has_expected_count && expected_count != array.elements.size()) {
    const u32 index = bag.emit<i18n::Key::AnalyzerArrayArity>(
        diag::Severity::Error, diag::Stage::Analyzer, DiagCode::ArityError,
        node.span, expected_count, array.elements.size());
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
      return check_path_expr(
          module, expr, node.payload.get<ast::ExprPath>().type_args,
          node.payload.get<ast::ExprPath>().idx, expected, node.span);
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
        // Origin-recorded like every other slot copy, so structural
        // equality sees through the contiguity copies.
        seq.push(storage_copy(element));
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
      const u32 index = bag.emit<i18n::Key::AnalyzerInvalidUnaryOperand>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidOperation, node.span, pretty_tag(tag));
      (void)index;
      return error_type();
    }
    case ast::ExprKind::Borrow: {
      // Place-ness is a borrow-checking concern; here the
      // inner type only determines the reference shape.
      const ast::ExprBorrow& borrow = node.payload.get<ast::ExprBorrow>();
      if (ast.exprs[borrow.inner].kind == ast::ExprKind::Index) {
        return check_borrow_of_index(module, expr, borrow, expected);
      }
      const ir::TypeIdx pointee = check_expr(module, borrow.inner, nullptr);
      if (is_error(pointee)) {
        return error_type();
      }
      const ir::TypeIdx type = builder.reference_type(pointee, borrow.is_mut);
      if (expected != nullptr) {
        return unify(*expected, type, node.span, "borrow");
      }
      return type;
    }
    case ast::ExprKind::Deref: {
      // The place a reference addresses. Assignment through a shared
      // reference would need a second reference alive, so the
      // mutability of the operand is preserved.
      const ir::TypeIdx inner =
          check_expr(module, node.payload.get<ast::ExprDeref>().inner, nullptr);
      if (is_error(inner)) {
        return error_type();
      }
      const ir::TypeTag tag = tag_of(inner);
      if (tag != ir::TypeTag::Ref && tag != ir::TypeTag::MutRef) {
        const u32 index = bag.emit<i18n::Key::AnalyzerCannotDereference>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::InvalidOperation, node.span, pretty_tag(tag_of(inner)));
        (void)index;
        return error_type();
      }
      const ir::TypeIdx pointee =
          builder.ref_types()[builder.types()[inner.idx].as_ref()].pointee;
      if (expected != nullptr) {
        return unify(*expected, pointee, node.span, "dereference");
      }
      return pointee;
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
          // What the backend can compare: integers (bool included),
          // floats, and addresses. A function value or an aggregate has
          // no comparison, and the emitter would have no case for it.
          const bool comparable =
              is_integer_tag(tag) || is_float_tag(tag) ||
              tag == ir::TypeTag::I1 || tag == ir::TypeTag::Ptr ||
              tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef;
          if (!comparable) {
            break;
          }
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
        case ast::BinaryOp::Add:
        case ast::BinaryOp::Sub:
        case ast::BinaryOp::Mul:
        case ast::BinaryOp::Div:
          if (is_integer_tag(tag) || is_float_tag(tag)) {
            if (expected != nullptr) {
              return unify(*expected, operands, node.span, "arithmetic");
            }
            return operands;
          }
          break;
        case ast::BinaryOp::Pow: {
          // The backend has no power operation yet; refuse it here
          // rather than let it reach lowering's unsupported path.
          const u32 index = bag.emit<i18n::Key::AnalyzerPowerNotImplemented>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::InvalidOperation, node.span);
          (void)index;
          return error_type();
        }
        default:
          // Remainder and the bitwise and shift operators are
          // integer-only.
          if (is_integer_tag(tag)) {
            if (expected != nullptr) {
              return unify(*expected, operands, node.span, "arithmetic");
            }
            return operands;
          }
          break;
      }
      const u32 index = bag.emit<i18n::Key::AnalyzerInvalidBinaryOperand>(
          diag::Severity::Error, diag::Stage::Analyzer,
          DiagCode::InvalidOperation, node.span, pretty_tag(tag));
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
        const u32 index = bag.emit<i18n::Key::AnalyzerReturnCrossesCompBlock>(
            diag::Severity::Error, diag::Stage::Analyzer, DiagCode::InvalidComp,
            node.span);
        (void)index;
        return error_type();
      }
      if (!in_fn) {
        const u32 index = bag.emit<i18n::Key::AnalyzerReturnOutsideFunction>(
            diag::Severity::Error, diag::Stage::Analyzer, DiagCode::BadReturn,
            node.span);
        (void)index;
        return error_type();
      }
      if (!node.payload.get<ast::ExprReturn>().value.is_valid()) {
        fn_ret = unify(fn_ret, builder.primitive(ir::TypeTag::Void), node.span,
                       "return");
      } else {
        const ir::TypeIdx value = check_expr(
            module, node.payload.get<ast::ExprReturn>().value, &fn_ret);
        // The anchor sticks once named: a declared return type never
        // drifts, while an error slot takes the first return it sees
        // so later returns and the trailing value have something to
        // meet. Only broken declarations start erroneous, where the
        // bag already holds errors.
        if (is_error(fn_ret)) {
          fn_ret =
              unify(fn_ret, value,
                    ast.exprs[node.payload.get<ast::ExprReturn>().value].span,
                    "return");
        } else {
          unify(fn_ret, value,
                ast.exprs[node.payload.get<ast::ExprReturn>().value].span,
                "return");
        }
      }
      return builder.never_type();
    }
    case ast::ExprKind::Break:
    case ast::ExprKind::Continue: {
      if (loop_depth == 0) {
        if (node.kind == ast::ExprKind::Break) {
          const u32 index = bag.emit<i18n::Key::AnalyzerBreakOutsideLoop>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::BreakOutsideLoop, node.span);
          (void)index;
        } else {
          const u32 index = bag.emit<i18n::Key::AnalyzerContinueOutsideLoop>(
              diag::Severity::Error, diag::Stage::Analyzer,
              DiagCode::BreakOutsideLoop, node.span);
          (void)index;
        }
        return error_type();
      }
      return builder.never_type();
    }
    case ast::ExprKind::Range: {
      return check_range(module, expr, expected);
    }
    case ast::ExprKind::Closure: {
      return check_closure(module, expr, expected);
    }
  }
}

ir::TypeIdx Checker::check_block(u32 module,
                                 ast::BlockIdx block,
                                 const ir::TypeIdx* expected) {
  if (nesting_.exhausted()) {
    report_too_deep(ast.blocks[block].span);
    return error_type();
  }
  const base::NestingScope scope(nesting_);
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
    if (verify_comp_known && !comp_checked_in_scope(node.value) &&
        !expr_comp_known(module, node.value)) {
      const u32 index = bag.emit<i18n::Key::AnalyzerCompBlockValue>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::NotCompKnown,
          ast.exprs[node.value].span);
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
      if (!resolve_value_path(module, path,
                              node.payload.get<ast::ExprPath>().type_args,
                              resolved)) {
        return error_type();
      }
      if (resolved.kind != PathValue::Kind::Local) {
        const u32 index = bag.emit<i18n::Key::AnalyzerNotAnAssignablePlace>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::BadAssignment, node.span);
        (void)index;
        return error_type();
      }
      // Locals shadow everything, but the resolution above may have
      // found the name through another namespace; confirm mutability
      // through the scope entry.
      const Local* local = lookup_local(ast.paths[path].segments.back().name);
      if (local == nullptr || !local->is_mut) {
        const u32 index = bag.emit<i18n::Key::AnalyzerAssignToImmutable>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::BadAssignment, node.span);
        (void)index;
        return error_type();
      }
      if (verify_comp_known && !local->comp_known) {
        const u32 index = bag.emit<i18n::Key::AnalyzerCompAssignPlace>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::NotCompKnown, node.span);
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
      // The field check diagnoses a receiver that is neither a struct
      // nor a tuple; returning early here let the assignment through
      // silently and reached an internal lowering error instead.
      return check_field(module, place, nullptr);
    }
    case ast::ExprKind::Index: {
      const ast::ExprIndex& index = node.payload.get<ast::ExprIndex>();
      const ir::TypeIdx receiver = check_place(module, index.receiver);
      if (is_error(receiver)) {
        return error_type();
      }
      // A slice behind a shared reference reads only, exactly like
      // `*r` on a `&T` place: the element is reachable, not writable.
      if (tag_of(receiver) == ir::TypeTag::Ref) {
        const u32 index = bag.emit<i18n::Key::AnalyzerAssignThroughShared>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::BadAssignment, node.span);
        (void)index;
        return error_type();
      }
      const ir::TypeIdx position = check_expr(module, index.index, nullptr);
      if (is_error(position)) {
        return error_type();
      }
      ir::TypeIdx element = error_type();
      if (range_element(position, element)) {
        const u32 diag = bag.emit<i18n::Key::AnalyzerCannotAssignToRun>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::BadAssignment, node.span);
        (void)diag;
        return error_type();
      }
      return check_element_index(receiver, position, index.index, nullptr,
                                 node.span);
    }
    case ast::ExprKind::Deref: {
      const ir::TypeIdx inner =
          check_place(module, node.payload.get<ast::ExprDeref>().inner);
      if (is_error(inner)) {
        return error_type();
      }
      if (tag_of(inner) != ir::TypeTag::MutRef) {
        const u32 index = bag.emit<i18n::Key::AnalyzerAssignThroughShared>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::BadAssignment, node.span);
        (void)index;
        return error_type();
      }
      return builder.ref_types()[builder.types()[inner.idx].as_ref()].pointee;
    }
    default: {
      const u32 index = bag.emit<i18n::Key::AnalyzerNotAnAssignablePlace>(
          diag::Severity::Error, diag::Stage::Analyzer, DiagCode::BadAssignment,
          node.span);
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
            bag.emit<i18n::Key::AnalyzerCompDeclarationInitializer>(
                diag::Severity::Error, diag::Stage::Analyzer,
                DiagCode::NotCompKnown, ast.exprs[decl.init].span);
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
            bag.emit<i18n::Key::AnalyzerRefutablePatternInDeclaration>(
                diag::Severity::Error, diag::Stage::Analyzer,
                DiagCode::RefutableLet, ast.patterns[decl.pattern].span);
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
              const u32 index =
                  bag.emit<i18n::Key::AnalyzerCompBindingReassigned>(
                      diag::Severity::Error, diag::Stage::Analyzer,
                      DiagCode::InvalidComp, node.span);
              (void)index;
              return;
            }
          }
        }
      }
      const ir::TypeIdx place = check_place(module, reassign.place);
      // A failed place is already reported; checking the value against
      // it would silently accept any type through the error slot.
      const ir::TypeIdx value =
          check_expr(module, node.payload.get<ast::StmtReassign>().value,
                     is_error(place) ? nullptr : &place);
      if (is_error(place)) {
        return;
      }
      if (verify_comp_known &&
          !comp_checked_in_scope(node.payload.get<ast::StmtReassign>().value) &&
          !expr_comp_known(module,
                           node.payload.get<ast::StmtReassign>().value)) {
        const u32 index = bag.emit<i18n::Key::AnalyzerCompAssignValue>(
            diag::Severity::Error, diag::Stage::Analyzer,
            DiagCode::NotCompKnown,
            ast.exprs[node.payload.get<ast::StmtReassign>().value].span);
        (void)index;
      }
      if (node.payload.get<ast::StmtReassign>().compound) {
        const ir::TypeTag tag = tag_of(place);
        if (!is_integer_tag(tag) && !is_float_tag(tag)) {
          const u32 index =
              bag.emit<i18n::Key::AnalyzerCompoundAssignNeedsNumeric>(
                  diag::Severity::Error, diag::Stage::Analyzer,
                  DiagCode::InvalidOperation, node.span);
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
      if (is_void(type) || is_error(type) || is_never(type)) {
        return;
      }
      const u32 index = bag.emit<i18n::Key::AnalyzerUnusedValue>(
          diag::Severity::Warning, diag::Stage::Analyzer, DiagCode::MustUse,
          ast.exprs[node.payload.get<ast::StmtExpr>().value].span);
      (void)index;
      return;
    }
  }
}

}  // namespace analyzer
