// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "analyzer/comp_evaluator.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "comp/comp_value.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace analyzer {

CompEvaluator::CompEvaluator(const CheckedPackage& pkg,
                             const ast::AstArena& ast,
                             ir::StorageBuilder& builder,
                             ir::PointerWidth width)
    : pkg(pkg), ast(ast), builder(builder), width(width) {
  generic_insts_.reserve(pkg.generic_insts.size());
  for (u32 i = 0; i < static_cast<u32>(pkg.generic_insts.size()); ++i) {
    generic_insts_.emplace(pkg.generic_insts[i].idx, i);
  }
  type_origins_.reserve(pkg.type_origins.size());
  for (usize i = pkg.type_origins.size(); i > 0; --i) {
    type_origins_.emplace(pkg.type_origins[i - 1].first.idx,
                          pkg.type_origins[i - 1].second.idx);
  }
  for (const CheckedModule& checked : pkg.modules) {
    for (const CheckedModule::StructInfo& info : checked.structs) {
      structs_.emplace(info.type.idx, &info);
    }
    for (const CheckedModule::EnumInfo& info : checked.enums) {
      enums_.emplace(info.type.idx, &info);
    }
    for (const CheckedModule::VariantUse& use : checked.variants) {
      variants_.emplace(
          (static_cast<u64>(use.path.idx) << 32) | static_cast<u64>(use.inst),
          &use);
    }
  }
}

ir::TypeIdx CompEvaluator::usize_type() {
  return builder.primitive(width == ir::PointerWidth::W64 ? ir::TypeTag::U64
                                                          : ir::TypeTag::U32);
}

ir::TypeIdx CompEvaluator::type_origin(ir::TypeIdx type) const {
  const auto found = type_origins_.find(type.idx);
  return found == type_origins_.end() ? type : ir::TypeIdx(found->second);
}

const CheckedModule::StructInfo* CompEvaluator::struct_info(
    ir::TypeIdx type) const {
  const ir::TypeIdx origin = type_origin(type);
  const auto found = structs_.find(origin.idx);
  return found == structs_.end() ? nullptr : found->second;
}

bool CompEvaluator::struct_field_index(ir::TypeIdx type,
                                       std::string_view name,
                                       u32& index_out) {
  const CheckedModule::StructInfo* info = struct_info(type);
  if (info == nullptr) {
    return false;
  }
  const auto& fields = info->fields;
  for (u32 i = 0; i < static_cast<u32>(fields.size()); ++i) {
    if (fields[i] == name) {
      index_out = i;
      return true;
    }
  }
  return false;
}

bool CompEvaluator::variant_index(ir::TypeIdx enum_type,
                                  std::string_view name,
                                  u32& index_out) {
  const auto found = enums_.find(enum_type.idx);
  if (found == enums_.end()) {
    return false;
  }
  const CheckedModule::EnumInfo* info = found->second;
  for (u32 i = 0; i < static_cast<u32>(info->variants.size()); ++i) {
    if (info->variants[i] == name) {
      index_out = i;
      return true;
    }
  }
  return false;
}

ir::TypeIdx CompEvaluator::field_type_of(ir::TypeIdx base,
                                         u32 index,
                                         diag::Span span) {
  const ir::TypeTag tag = tag_of(base);
  if (tag == ir::TypeTag::Struct) {
    const ir::StructType& struct_type =
        builder.state().struct_types[builder.state().types[base].as_struct()];
    if (index < struct_type.fields.size()) {
      return struct_type.fields[index];
    }
  } else if (tag == ir::TypeTag::Tuple) {
    const ir::TupleType& tuple_type =
        builder.state().tuple_types[builder.state().types[base].as_tuple()];
    if (index < tuple_type.elements.size()) {
      return tuple_type.elements[index];
    }
  } else if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
    const ir::TypeIdx pointee =
        builder.state().ref_types[builder.state().types[base].as_ref()].pointee;
    return field_type_of(pointee, index, span);
  }
  fail_internal(span, "field type without declaration");
  return error_type();
}

bool CompEvaluator::fail(diag::Span span, std::string_view what) {
  if (!failed_) {
    failed_ = true;
    fail_internal_ = false;
    fail_span_ = span;
    fail_what_ = what;
  }
  return false;
}

bool CompEvaluator::fail_internal(diag::Span span, std::string_view what) {
  if (!failed_) {
    failed_ = true;
    fail_internal_ = true;
    fail_span_ = span;
    fail_what_ = what;
  }
  return false;
}

bool CompEvaluator::spend(diag::Span span) {
  if (quota_ == 0) {
    return fail(span, "comp evaluation quota exhausted");
  }
  --quota_;
  return true;
}

void CompEvaluator::clear_failure() {
  failed_ = false;
  fail_internal_ = false;
  fail_span_ = diag::Span{};
  fail_what_ = {};
}

const CheckedModule::StaticInfo* CompEvaluator::lookup_static_in(
    u32 mod,
    std::string_view name) const {
  for (const auto& info : pkg.modules[mod].statics) {
    if (info.name == name) {
      return &info;
    }
  }
  for (const Import& import : pkg.tree.modules[mod]->imports) {
    if (import.ns != Namespace::Value || import.name != name) {
      continue;
    }
    for (const auto& info : pkg.modules[import.target_module].statics) {
      if (info.name == import.member) {
        return &info;
      }
    }
  }
  return nullptr;
}

const CheckedModule::VariantUse* CompEvaluator::variant_use_in(
    ast::PathIdx path,
    u32 inst) const {
  const auto found = variants_.find((static_cast<u64>(path.idx) << 32) |
                                    static_cast<u64>(inst));
  return found == variants_.end() ? nullptr : found->second;
}

std::vector<ir::TypeIdx> CompEvaluator::variant_payload(ir::TypeIdx enum_type,
                                                        u32 variant) const {
  const ir::EnumType& enum_ty =
      builder.state().enum_types[builder.state().types[enum_type].as_enum()];
  std::vector<ir::TypeIdx> payloads;
  u32 at = enum_ty.variants.head().idx + variant;
  const ir::EnumVariantType& variant_ty =
      builder.state().enum_variant_types[ir::EnumVariantTypeIdx(at)];
  for (ir::TypeIdx field : variant_ty.fields) {
    payloads.push_back(field);
  }
  return payloads;
}

u32 CompEvaluator::generic_inst_index(ir::TypeIdx type) const {
  const auto found = generic_insts_.find(type.idx);
  return found == generic_insts_.end() ? NO_INST : found->second;
}

u32 CompEvaluator::callee_inst(const CheckedModule::CallTarget* target) const {
  if (target == nullptr) {
    return NO_INST;
  }
  if (target->is_method) {
    const CheckedModule& def = pkg.modules[target->module];
    if (target->index >= def.methods.size()) {
      return NO_INST;
    }
    return generic_inst_index(def.methods[target->index].self_type);
  }
  const CheckedModule& def = pkg.modules[target->module];
  if (target->index >= def.functions.size()) {
    return NO_INST;
  }
  return def.functions[target->index].inst;
}

bool CompEvaluator::evaluate(
    u32 module,
    ast::ExprIdx expr,
    u32 inst,
    std::vector<std::pair<std::string_view, CompVal>>* outer,
    CompVal& out) {
  CompScope scope;
  scope.outer = outer;
  scope.frames.emplace_back();
  quota_ = COMP_BRANCH_QUOTA;
  call_depth_ = 0;
  inst_ = inst;
  return comp_eval_expr(module, expr, scope, out);
}

bool CompEvaluator::evaluate_item(u32 module,
                                  ast::ExprIdx init,
                                  u32 inst,
                                  CompVal& out) {
  quota_ = COMP_BRANCH_QUOTA;
  call_depth_ = 0;
  inst_ = inst;
  return comp_eval_const_item(module, init, out);
}

ir::TypeIdx CompEvaluator::expr_type_in(u32 mod, ast::ExprIdx expr) {
  for (const auto& entry : pkg.modules[mod].expr_types) {
    if (entry.expr == expr && entry.inst == inst_) {
      return entry.type;
    }
  }
  return error_type();
}

const analyzer::CheckedModule::CallTarget* CompEvaluator::call_target_in(
    u32 mod,
    ast::ExprIdx callee) const {
  const analyzer::CheckedModule& checked = pkg.modules[mod];
  const auto found = checked.call_target_by_key.find(
      analyzer::CheckedModule::call_target_key(callee, inst_));
  return found == checked.call_target_by_key.end()
             ? nullptr
             : &checked.call_targets[found->second];
}

bool CompEvaluator::comp_eval_literal(u32 mod,
                                      ast::ExprIdx expr,
                                      CompVal& out) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::Literal& lit =
      ast.literals[node.payload.get<ast::ExprLiteral>().value];
  const ir::TypeIdx type = expr_type_in(mod, expr);
  if (tag_of(type) == ir::TypeTag::Error) {
    return fail(node.span, "comp operand without type");
  }
  out.type = type;
  if (lit.kind == ast::LiteralKind::Bool) {
    out.value.tag = CompValue::Tag::Bool;
    out.value.bool_value = lit.spelling == "true";
    return true;
  }
  if (lit.kind == ast::LiteralKind::String) {
    out.value.tag = CompValue::Tag::Str;
    out.value.str_value = comp::unescape(lit.spelling);
    return true;
  }
  if (lit.kind == ast::LiteralKind::Integer) {
    out.value.tag = CompValue::Tag::Int;
    out.value.int_value = comp::parse_numeric_value(lit.spelling);
    if (lit.is_negative) {
      out.value.int_value = 0 - out.value.int_value;
    }
    return true;
  }
  return fail(node.span, "literal is not comp-evaluable");
}

bool CompEvaluator::comp_bind_pattern(u32 mod,
                                      ast::PatternIdx pattern,
                                      const CompVal& value,
                                      CompScope& scope,
                                      diag::Span span) {
  const ast::PatternNode& node = ast.patterns[pattern];
  switch (node.kind) {
    case ast::PatternKind::Wildcard: return true;
    case ast::PatternKind::Ident:
      scope.frames.back().emplace_back(node.payload.ident.name.name, value);
      return true;
    case ast::PatternKind::MutIdent:
      scope.frames.back().emplace_back(node.payload.mut_ident.name.name, value);
      return true;
    case ast::PatternKind::Tuple: {
      if (node.payload.tuple.path.is_valid()) {
        // Variant patterns bind through matching.
        return comp_match_pattern(mod, pattern, value, scope, span);
      }
      if (value.value.tag != CompValue::Tag::Tuple) {
        return fail(span, "pattern is not comp-evaluable");
      }
      const ir::TupleType& shape =
          builder.state()
              .tuple_types[builder.state().types[value.type].as_tuple()];
      const std::span<const ast::PatternIdx> elements =
          node.payload.tuple.elements;
      if (elements.size() != value.value.fields.size() ||
          elements.size() != shape.elements.size()) {
        return fail(span, "tuple pattern arity");
      }
      for (usize i = 0; i < elements.size(); ++i) {
        const CompVal field{value.value.fields[i], shape.elements[i]};
        if (!comp_bind_pattern(mod, elements[i], field, scope, span)) {
          return false;
        }
      }
      return true;
    }
    case ast::PatternKind::Struct: {
      if (value.value.tag != CompValue::Tag::Struct) {
        return fail(span, "pattern is not comp-evaluable");
      }
      for (const ast::FieldPattern& field : node.payload.strukt.fields) {
        u32 index = 0;
        if (!struct_field_index(value.type, field.name.name, index) ||
            index >= value.value.fields.size()) {
          return fail(field.name.span, "pattern field without value");
        }
        const ir::TypeIdx member_type = field_type_of(value.type, index, span);
        if (failed_) {
          return false;
        }
        const CompVal member{value.value.fields[index], member_type};
        if (!comp_bind_pattern(mod, field.pattern, member, scope, span)) {
          return false;
        }
      }
      return true;
    }
    case ast::PatternKind::Ref:
    case ast::PatternKind::Or:
      // Transparent dereference and first-match alternatives bind
      // exactly like matching.
      return comp_match_pattern(mod, pattern, value, scope, span);
    case ast::PatternKind::Literal: break;
  }
  return fail(node.span, "pattern is not comp-evaluable");
}

bool CompEvaluator::comp_match_pattern(u32 mod,
                                       ast::PatternIdx pattern,
                                       const CompVal& value,
                                       CompScope& scope,
                                       diag::Span span) {
  const ast::PatternNode& node = ast.patterns[pattern];
  switch (node.kind) {
    case ast::PatternKind::Wildcard: return true;
    case ast::PatternKind::Ident:
    case ast::PatternKind::MutIdent:
      return comp_bind_pattern(mod, pattern, value, scope, span);
    case ast::PatternKind::Literal: {
      CompVal expected;
      expected.type = value.type;
      const ast::Literal& lit = ast.literals[node.payload.literal.value];
      if (lit.kind == ast::LiteralKind::Bool) {
        if (value.value.tag != CompValue::Tag::Bool) {
          return false;
        }
        return value.value.bool_value == (lit.spelling == "true");
      }
      if (lit.kind == ast::LiteralKind::Integer) {
        if (value.value.tag != CompValue::Tag::Int) {
          return false;
        }
        u64 expected = comp::parse_numeric_value(lit.spelling);
        if (lit.is_negative) {
          expected = 0 - expected;
        }
        return value.value.int_value ==
               (expected & comp::mask(tag_of(value.type)));
      }
      if (lit.kind == ast::LiteralKind::String) {
        if (value.value.tag != CompValue::Tag::Str) {
          return false;
        }
        return value.value.str_value == comp::unescape(lit.spelling);
      }
      return fail(node.span, "pattern is not comp-evaluable");
    }
    case ast::PatternKind::Tuple: {
      if (!node.payload.tuple.path.is_valid()) {
        if (value.value.tag != CompValue::Tag::Tuple) {
          return false;
        }
        const std::span<const ast::PatternIdx> elements =
            node.payload.tuple.elements;
        if (elements.size() != value.value.fields.size()) {
          return false;
        }
        const usize mark = scope.frames.back().size();
        for (usize i = 0; i < elements.size(); ++i) {
          const CompVal field{value.value.fields[i], value.type};
          if (!comp_match_pattern(mod, elements[i], field, scope, span)) {
            scope.frames.back().resize(mark);
            return false;
          }
        }
        return true;
      }
      const std::span<const ast::Ident> segments =
          ast.paths[node.payload.tuple.path].segments;
      if (segments.empty()) {
        return fail(span, "pattern is not comp-evaluable");
      }
      const std::string_view name = segments.back().name;
      std::vector<CompVal> payloads;
      if (value.value.tag == CompValue::Tag::Enum) {
        u32 variant = 0;
        if (!variant_index(value.type, name, variant) ||
            variant != value.value.variant) {
          return false;
        }
        const std::vector<ir::TypeIdx> types =
            variant_payload(value.type, variant);
        if (types.size() != value.value.fields.size()) {
          return fail(span, "variant arity");
        }
        for (usize i = 0; i < types.size(); ++i) {
          payloads.push_back(CompVal{value.value.fields[i], types[i]});
        }
      } else {
        return false;
      }
      const std::span<const ast::PatternIdx> elements =
          node.payload.tuple.elements;
      if (elements.size() != payloads.size()) {
        return false;
      }
      const usize mark = scope.frames.back().size();
      for (usize i = 0; i < elements.size(); ++i) {
        if (!comp_match_pattern(mod, elements[i], payloads[i], scope, span)) {
          scope.frames.back().resize(mark);
          return false;
        }
      }
      return true;
    }
    case ast::PatternKind::Struct: {
      if (value.value.tag != CompValue::Tag::Struct) {
        return false;
      }
      const usize mark = scope.frames.back().size();
      for (const ast::FieldPattern& field : node.payload.strukt.fields) {
        u32 index = 0;
        if (!struct_field_index(value.type, field.name.name, index) ||
            index >= value.value.fields.size()) {
          scope.frames.back().resize(mark);
          return fail(field.name.span, "pattern field without value");
        }
        const CompVal member{value.value.fields[index],
                             field_type_of(value.type, index, span)};
        if (failed_) {
          scope.frames.back().resize(mark);
          return false;
        }
        if (!comp_match_pattern(mod, field.pattern, member, scope, span)) {
          scope.frames.back().resize(mark);
          return false;
        }
      }
      return true;
    }
    case ast::PatternKind::Ref:
      return comp_match_pattern(mod, node.payload.ref.inner, value, scope,
                                span);
    case ast::PatternKind::Or: {
      const usize mark = scope.frames.back().size();
      for (ast::PatternIdx alt : node.payload.or_pat.alternatives) {
        if (comp_match_pattern(mod, alt, value, scope, span)) {
          return true;
        }
        if (failed_) {
          scope.frames.back().resize(mark);
          return false;
        }
        scope.frames.back().resize(mark);
      }
      return false;
    }
  }
}

bool CompEvaluator::comp_eval_struct(u32 mod,
                                     ast::ExprIdx expr,
                                     CompScope& scope,
                                     CompVal& out) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprStruct& strukt = node.payload.get<ast::ExprStruct>();
  out.type = expr_type_in(mod, expr);
  if (tag_of(out.type) != ir::TypeTag::Struct) {
    return fail(node.span, "struct without value");
  }
  const auto* info = struct_info(out.type);
  if (info == nullptr) {
    return fail(node.span, "struct without declaration");
  }
  std::vector<CompValue> fields(info->fields.size());
  std::vector<bool> seen(info->fields.size(), false);
  for (const ast::ExprFieldInit& field : strukt.init) {
    u32 index = 0;
    if (!struct_field_index(out.type, field.name.name, index)) {
      return fail(field.name.span, "unknown field");
    }
    CompVal member;
    if (!comp_eval_expr(mod, field.value, scope, member)) {
      return false;
    }
    fields[index] = std::move(member.value);
    seen[index] = true;
  }
  if (strukt.base_expr.is_valid()) {
    CompVal base;
    if (!comp_eval_expr(mod, strukt.base_expr, scope, base)) {
      return false;
    }
    if (base.value.tag != CompValue::Tag::Struct ||
        base.value.fields.size() != fields.size()) {
      return fail(node.span, "struct base without value");
    }
    for (usize i = 0; i < fields.size(); ++i) {
      if (!seen[i]) {
        fields[i] = base.value.fields[i];
      }
    }
  }
  out.value.tag = CompValue::Tag::Struct;
  out.value.fields = std::move(fields);
  return true;
}

bool CompEvaluator::comp_eval_block(u32 mod,
                                    ast::BlockIdx block,
                                    CompScope& scope,
                                    CompFlow& out) {
  const ast::Block& node = ast.blocks[block];
  scope.frames.emplace_back();
  for (ast::StmtIdx stmt : node.statements) {
    if (failed_) {
      scope.frames.pop_back();
      return false;
    }
    CompFlow flow;
    if (!comp_eval_stmt(mod, stmt, scope, flow)) {
      scope.frames.pop_back();
      return false;
    }
    if (flow.kind != CompFlow::Kind::Value) {
      scope.frames.pop_back();
      out = std::move(flow);
      return true;
    }
  }
  if (failed_) {
    scope.frames.pop_back();
    return false;
  }
  if (!node.value.is_valid()) {
    scope.frames.pop_back();
    out.kind = CompFlow::Kind::Value;
    out.value.value.tag = CompValue::Tag::Void;
    out.value.type = builder.primitive(ir::TypeTag::Void);
    return true;
  }
  // A trailing `ret` is the block's value in the syntax and a return
  // in the flow: the evaluator needs the difference, so an early
  // return reached through a branch fails loudly rather than splicing
  // a wrong value.
  if (ast.exprs[node.value].kind == ast::ExprKind::Return) {
    const ast::ExprReturn& ret =
        ast.exprs[node.value].payload.get<ast::ExprReturn>();
    CompVal value;
    if (ret.value.is_valid()) {
      if (!comp_eval_expr(mod, ret.value, scope, value)) {
        scope.frames.pop_back();
        return false;
      }
    } else {
      value.value.tag = CompValue::Tag::Void;
      value.type = builder.primitive(ir::TypeTag::Void);
    }
    scope.frames.pop_back();
    out.kind = CompFlow::Kind::Return;
    out.value = std::move(value);
    return true;
  }
  CompVal value;
  const bool ok = comp_eval_expr(mod, node.value, scope, value);
  scope.frames.pop_back();
  if (!ok) {
    return false;
  }
  out.kind = CompFlow::Kind::Value;
  out.value = std::move(value);
  return true;
}

bool CompEvaluator::comp_eval_stmt(u32 mod,
                                   ast::StmtIdx stmt,
                                   CompScope& scope,
                                   CompFlow& out) {
  const ast::StmtNode& node = ast.stmts[stmt];
  switch (node.kind) {
    case ast::StmtKind::Decl: {
      const ast::StmtDecl& decl = node.payload.get<ast::StmtDecl>();
      CompVal init;
      if (!comp_eval_expr(mod, decl.init, scope, init)) {
        return false;
      }
      if (!comp_bind_pattern(mod, decl.pattern, init, scope, node.span)) {
        return false;
      }
      out.kind = CompFlow::Kind::Value;
      return true;
    }
    case ast::StmtKind::Reassign: {
      const ast::StmtReassign& reassign = node.payload.get<ast::StmtReassign>();
      if (reassign.compound) {
        return fail(node.span, "compound assignment in comp evaluation");
      }
      if (ast.exprs[reassign.place].kind != ast::ExprKind::Path) {
        return fail(node.span, "place without binding");
      }
      const ast::PathIdx path =
          ast.exprs[reassign.place].payload.get<ast::ExprPath>().idx;
      const std::span<const ast::Ident> segments = ast.paths[path].segments;
      if (segments.size() != 1) {
        return fail(node.span, "place without binding");
      }
      CompVal value;
      if (!comp_eval_expr(mod, reassign.value, scope, value)) {
        return false;
      }
      for (usize i = scope.frames.size(); i-- > 0;) {
        for (auto& binding : scope.frames[i]) {
          if (binding.first == segments[0].name) {
            binding.second = std::move(value);
            out.kind = CompFlow::Kind::Value;
            return true;
          }
        }
      }
      if (scope.outer != nullptr) {
        for (auto& binding : *scope.outer) {
          if (binding.first == segments[0].name) {
            binding.second = std::move(value);
            out.kind = CompFlow::Kind::Value;
            return true;
          }
        }
      }
      return fail(node.span, "place without binding");
    }
    case ast::StmtKind::Expr: {
      const ast::StmtExpr& expr = node.payload.get<ast::StmtExpr>();
      const ast::ExprKind kind = ast.exprs[expr.value].kind;
      if (kind == ast::ExprKind::Break) {
        out.kind = CompFlow::Kind::Break;
        return true;
      }
      if (kind == ast::ExprKind::Continue) {
        out.kind = CompFlow::Kind::Continue;
        return true;
      }
      // A `ret` in an evaluated function body ends the evaluation with
      // its value. A `ret` inside a comp block never reaches here: the
      // checker rejects crossing the block boundary.
      if (kind == ast::ExprKind::Return) {
        CompVal value;
        const ast::ExprReturn& ret =
            ast.exprs[expr.value].payload.get<ast::ExprReturn>();
        if (ret.value.is_valid()) {
          if (!comp_eval_expr(mod, ret.value, scope, value)) {
            return false;
          }
        } else {
          value.value.tag = CompValue::Tag::Void;
          value.type = builder.primitive(ir::TypeTag::Void);
        }
        out.kind = CompFlow::Kind::Return;
        out.value = std::move(value);
        return true;
      }
      CompVal discarded;
      if (!comp_eval_expr(mod, expr.value, scope, discarded)) {
        return false;
      }
      out.kind = CompFlow::Kind::Value;
      return true;
    }
  }
}

// Unrolls a loop body while its condition holds. `always` covers
// `loop`, which has no condition expression.
bool CompEvaluator::comp_eval_loop(u32 mod,
                                   ast::BlockIdx body,
                                   CompScope& scope,
                                   CompFlow& out,
                                   bool always,
                                   diag::Span span,
                                   ast::ExprIdx cond) {
  while (!failed_) {
    // Every turn of the loop is a backward jump, and the quota counts
    // it so a comp-known loop cannot run without bound.
    if (!spend(span)) {
      return false;
    }
    if (!always) {
      CompVal test;
      if (!comp_eval_expr(mod, cond, scope, test)) {
        return false;
      }
      if (test.value.tag != CompValue::Tag::Bool) {
        return fail(span, "condition without value");
      }
      if (!test.value.bool_value) {
        out.kind = CompFlow::Kind::Value;
        return true;
      }
    }
    CompFlow flow;
    if (!comp_eval_block(mod, body, scope, flow)) {
      return false;
    }
    if (flow.kind == CompFlow::Kind::Break) {
      out.kind = CompFlow::Kind::Value;
      return true;
    }
    if (flow.kind == CompFlow::Kind::Return) {
      out = std::move(flow);
      return true;
    }
  }
  return false;
}

// Interprets a resolved function body with comp actuals bound to
// comp formals. Runtime formals stay unbound: bodies touching them
// diagnose instead of evaluating.
bool CompEvaluator::comp_run_fn(u32 def_module,
                                ast::ItemIdx item,
                                const std::vector<ir::TypeIdx>& params,
                                ir::TypeIdx ret,
                                u32 inst,
                                u32 caller_module,
                                const std::span<const ast::ExprIdx>& args,
                                CompScope& caller_scope,
                                diag::Span span,
                                CompVal& out) {
  if (!item.is_valid()) {
    return fail(span, "callee without body");
  }
  const ast::ItemFn& fn = ast.items[item].payload.get<ast::ItemFn>();
  if (args.size() != params.size() || args.size() != fn.params.size()) {
    return fail(span, "call arity");
  }
  if (!spend(span)) {
    return false;
  }
  if (call_depth_ >= COMP_MAX_CALL_DEPTH) {
    return fail(span, "comp call depth exhausted");
  }
  ++call_depth_;
  const u32 saved_inst = inst_;
  inst_ = inst;
  CompScope callee_scope;
  callee_scope.frames.emplace_back();
  bool ok = true;
  for (usize i = 0; i < args.size() && ok; ++i) {
    // A `comp fn`'s arguments all bind as comp values; an ordinary
    // callee binds only its `comp` parameters and leaves the rest to
    // the runtime call the evaluator cannot make.
    if (!fn.params[i].is_comp && !fn.is_comp) {
      continue;
    }
    CompVal arg;
    if (!comp_eval_expr(caller_module, args[i], caller_scope, arg)) {
      ok = false;
      break;
    }
    if (!comp_bind_pattern(def_module, fn.params[i].pattern, arg, callee_scope,
                           ast.exprs[args[i]].span)) {
      ok = false;
    }
  }
  CompFlow flow;
  if (ok) {
    if (!fn.body.is_valid() ||
        !comp_eval_block(def_module, fn.body, callee_scope, flow)) {
      ok = false;
    }
  }
  --call_depth_;
  inst_ = saved_inst;
  if (!ok) {
    return false;
  }
  out.type = ret;
  if (flow.kind == CompFlow::Kind::Return ||
      flow.kind == CompFlow::Kind::Value) {
    out.value = flow.value.value;
    return true;
  }
  return fail(span, "control escapes the comp call");
}

bool CompEvaluator::comp_eval_assoc_call(
    u32 mod,
    ast::ExprIdx expr,
    CompScope& scope,
    CompVal& out,
    const analyzer::CheckedModule::CallTarget* target) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  if (target->index >= def.methods.size()) {
    return fail(node.span, "callee without target");
  }
  const analyzer::CheckedModule::MethodInfo& info = def.methods[target->index];
  if (info.receiver != analyzer::CheckedModule::ReceiverKind::None) {
    return fail(node.span, "method without receiver");
  }
  return comp_run_fn(target->module, info.item, info.params, info.ret,
                     callee_inst(target), mod, call.args, scope, node.span,
                     out);
}

// Native compile-time evaluation for string intrinsics. Other
// intrinsics touch runtime state and never evaluate.
bool CompEvaluator::comp_eval_intrinsic(
    u32 mod,
    const analyzer::CheckedModule::FnSig& sig,
    const std::span<const ast::ExprIdx>& args,
    CompScope& scope,
    diag::Span span,
    CompVal& out) {
  if (!spend(span)) {
    return false;
  }
  const ast::ItemIntrinsic& intrinsic =
      ast.items[sig.item].payload.get<ast::ItemIntrinsic>();
  const std::string_view name = intrinsic.name.name;
  if (name != "str_len" && name != "str_byte" && name != "str_slice") {
    return fail(span, "intrinsic is not comp-evaluable");
  }
  CompVal receiver;
  if (!comp_eval_expr(mod, args[0], scope, receiver)) {
    return false;
  }
  if (receiver.value.tag != CompValue::Tag::Str) {
    return fail(span, "string without value");
  }
  const std::string& bytes = receiver.value.str_value;
  const ir::TypeIdx usize_ty = usize_type();
  if (name == "str_len") {
    out.type = usize_ty;
    out.value.tag = CompValue::Tag::Int;
    out.value.int_value = static_cast<u64>(bytes.size());
    return true;
  }
  CompVal first;
  if (!comp_eval_expr(mod, args[1], scope, first)) {
    return false;
  }
  if (first.value.tag != CompValue::Tag::Int) {
    return fail(span, "index without value");
  }
  const u64 index = first.value.int_value;
  if (name == "str_byte") {
    if (index >= bytes.size()) {
      return fail(span, "index out of bounds");
    }
    out.type = builder.primitive(ir::TypeTag::U8);
    out.value.tag = CompValue::Tag::Int;
    out.value.int_value = static_cast<u8>(bytes[static_cast<usize>(index)]);
    return true;
  }
  CompVal second;
  if (!comp_eval_expr(mod, args[2], scope, second)) {
    return false;
  }
  if (second.value.tag != CompValue::Tag::Int) {
    return fail(span, "index without value");
  }
  const u64 end = second.value.int_value;
  if (index > end || end > bytes.size()) {
    return fail(span, "slice out of bounds");
  }
  out.type = receiver.type;
  out.value.tag = CompValue::Tag::Str;
  out.value.str_value =
      bytes.substr(static_cast<usize>(index), static_cast<usize>(end - index));
  return true;
}

bool CompEvaluator::comp_eval_call(u32 mod,
                                   ast::ExprIdx expr,
                                   CompScope& scope,
                                   CompVal& out) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
  if (ast.exprs[call.callee].kind != ast::ExprKind::Path) {
    return fail(node.span, "callee without target");
  }
  const ast::PathIdx path =
      ast.exprs[call.callee].payload.get<ast::ExprPath>().idx;
  const std::span<const ast::Ident> segments = ast.paths[path].segments;
  if (segments.size() == 1) {
    const std::string_view name = segments[0].name;
    if (name == "print" || name == "println" || name == "panic") {
      return fail(node.span, "intrinsic is not comp-evaluable");
    }
  }
  const analyzer::CheckedModule::CallTarget* target =
      call_target_in(mod, call.callee);
  if (target == nullptr) {
    if (const auto* use = variant_use_in(path, inst_)) {
      const std::vector<ir::TypeIdx> payloads =
          variant_payload(use->enum_type, use->variant);
      if (call.args.size() != payloads.size()) {
        return fail(node.span, "variant arity");
      }
      out.type = use->enum_type;
      out.value.tag = CompValue::Tag::Enum;
      out.value.variant = use->variant;
      for (ast::ExprIdx arg : call.args) {
        CompVal field;
        if (!comp_eval_expr(mod, arg, scope, field)) {
          return false;
        }
        out.value.fields.push_back(std::move(field.value));
      }
      return true;
    }
    return fail(node.span, "callee without target");
  }
  if (target->is_method) {
    return comp_eval_assoc_call(mod, expr, scope, out, target);
  }
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  if (target->index >= def.functions.size()) {
    return fail(node.span, "callee without target");
  }
  const analyzer::CheckedModule::FnSig& sig = def.functions[target->index];
  if (ast.items[sig.item].kind == ast::ItemKind::Intrinsic) {
    return comp_eval_intrinsic(mod, sig, call.args, scope, node.span, out);
  }
  return comp_run_fn(target->module, sig.item, sig.params, sig.ret,
                     callee_inst(target), mod, call.args, scope, node.span,
                     out);
}

bool CompEvaluator::comp_eval_method_call(u32 mod,
                                          ast::ExprIdx expr,
                                          CompScope& scope,
                                          CompVal& out) {
  const ast::ExprNode& node = ast.exprs[expr];
  if (!spend(node.span)) {
    return false;
  }
  const ast::ExprMethodCall& method = node.payload.get<ast::ExprMethodCall>();
  CompVal receiver;
  if (!comp_eval_expr(mod, method.receiver, scope, receiver)) {
    return false;
  }
  out.type = expr_type_in(mod, expr);
  const analyzer::CheckedModule::CallTarget* target = call_target_in(mod, expr);
  if (target == nullptr || !target->is_method) {
    return fail(node.span, "method without target");
  }
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  if (target->index >= def.methods.size()) {
    return fail(node.span, "method without target");
  }
  const analyzer::CheckedModule::MethodInfo& info = def.methods[target->index];
  if (info.receiver == analyzer::CheckedModule::ReceiverKind::None) {
    return fail(node.span, "method without receiver");
  }
  if (!info.item.is_valid()) {
    return fail(node.span, "method without body");
  }
  const ast::ItemFn& fn = ast.items[info.item].payload.get<ast::ItemFn>();
  if (method.args.size() + 1 != info.params.size() ||
      method.args.size() + 1 != fn.params.size()) {
    return fail(node.span, "call arity");
  }
  if (call_depth_ >= COMP_MAX_CALL_DEPTH) {
    return fail(node.span, "comp call depth exhausted");
  }
  ++call_depth_;
  CompScope callee_scope;
  callee_scope.frames.emplace_back();
  bool ok = comp_bind_pattern(target->module, fn.params[0].pattern, receiver,
                              callee_scope, node.span);
  for (usize i = 0; i < method.args.size() && ok; ++i) {
    CompVal arg;
    if (!comp_eval_expr(mod, method.args[i], scope, arg)) {
      ok = false;
      break;
    }
    if (!comp_bind_pattern(target->module, fn.params[i + 1].pattern, arg,
                           callee_scope, ast.exprs[method.args[i]].span)) {
      ok = false;
    }
  }
  CompFlow flow;
  if (ok) {
    if (!fn.body.is_valid() ||
        !comp_eval_block(target->module, fn.body, callee_scope, flow)) {
      ok = false;
    }
  }
  --call_depth_;
  if (!ok) {
    return false;
  }
  out.type = info.ret;
  if (flow.kind == CompFlow::Kind::Return ||
      flow.kind == CompFlow::Kind::Value) {
    out.value = flow.value.value;
    return true;
  }
  return fail(node.span, "control escapes the comp call");
}

bool CompEvaluator::comp_eval_const_item(u32 mod,
                                         ast::ExprIdx init,
                                         CompVal& out) {
  // A const initializer sees items, never a caller's comp bindings, so
  // it evaluates in a scope of its own; the depth counts as a call so a
  // const chain cannot recurse without bound.
  if (!spend(ast.exprs[init].span)) {
    return false;
  }
  if (call_depth_ >= COMP_MAX_CALL_DEPTH) {
    return fail(ast.exprs[init].span, "const evaluation is too deep");
  }
  ++call_depth_;
  CompScope scope;
  scope.frames.emplace_back();
  const bool ok = comp_eval_expr(mod, init, scope, out);
  --call_depth_;
  return ok;
}

bool CompEvaluator::comp_eval_expr(u32 mod,
                                   ast::ExprIdx expr,
                                   CompScope& scope,
                                   CompVal& out) {
  if (failed_) {
    return false;
  }
  const ast::ExprNode& node = ast.exprs[expr];
  switch (node.kind) {
    case ast::ExprKind::Literal: return comp_eval_literal(mod, expr, out);
    case ast::ExprKind::Path: {
      const ast::PathIdx path = node.payload.get<ast::ExprPath>().idx;
      const std::span<const ast::Ident> segments = ast.paths[path].segments;
      if (segments.size() == 1) {
        if (const CompVal* bound = comp::lookup(scope, segments[0].name)) {
          out = *bound;
          return true;
        }
        if (const auto* info = lookup_static_in(mod, segments[0].name)) {
          if (info->is_const && info->init.is_valid()) {
            return comp_eval_const_item(mod, info->init, out);
          }
        }
      }
      if (const auto* use = variant_use_in(path, inst_)) {
        const std::vector<ir::TypeIdx> payloads =
            variant_payload(use->enum_type, use->variant);
        if (!payloads.empty()) {
          return fail(node.span, "variant without call");
        }
        out.type = use->enum_type;
        out.value.tag = CompValue::Tag::Enum;
        out.value.variant = use->variant;
        return true;
      }
      return fail(node.span, "path is not comp-evaluable");
    }
    case ast::ExprKind::Unary: {
      const ast::ExprUnary& unary = node.payload.get<ast::ExprUnary>();
      CompVal inner;
      if (!comp_eval_expr(mod, unary.inner, scope, inner)) {
        return false;
      }
      out.type = expr_type_in(mod, expr);
      if (unary.op == ast::UnaryOp::Not) {
        if (inner.value.tag != CompValue::Tag::Bool) {
          return fail(node.span, "unary operand without value");
        }
        out.value.tag = CompValue::Tag::Bool;
        out.value.bool_value = !inner.value.bool_value;
        return true;
      }
      if (inner.value.tag != CompValue::Tag::Int) {
        return fail(node.span, "unary operand without value");
      }
      const ir::TypeTag tag = tag_of(inner.type);
      const u64 mask = comp::mask(tag);
      out.value.tag = CompValue::Tag::Int;
      if (unary.op == ast::UnaryOp::BitNot) {
        out.value.int_value = ~inner.value.int_value & mask;
        return true;
      }
      out.value.int_value =
          (static_cast<u64>(0) - (inner.value.int_value & mask)) & mask;
      return true;
    }
    case ast::ExprKind::Borrow: {
      // Borrows are transparent in comp evaluation: the value flows
      // through, and references never escape except as `str`.
      return comp_eval_expr(mod, node.payload.get<ast::ExprBorrow>().inner,
                            scope, out);
    }
    case ast::ExprKind::Deref: return false;
    case ast::ExprKind::Binary: return comp_eval_binary(mod, expr, scope, out);
    case ast::ExprKind::Cast: {
      const ast::ExprCast& cast = node.payload.get<ast::ExprCast>();
      CompVal inner;
      if (!comp_eval_expr(mod, cast.inner, scope, inner)) {
        return false;
      }
      out.type = expr_type_in(mod, expr);
      const ir::TypeTag target = tag_of(out.type);
      if (target == ir::TypeTag::I1) {
        out.value.tag = CompValue::Tag::Bool;
        out.value.bool_value = comp::truth(inner);
        return true;
      }
      if (inner.value.tag == CompValue::Tag::Bool) {
        out.value.tag = CompValue::Tag::Int;
        out.value.int_value = inner.value.bool_value ? 1 : 0;
        return true;
      }
      if (inner.value.tag != CompValue::Tag::Int) {
        return fail(node.span, "cast without value");
      }
      out.value.tag = CompValue::Tag::Int;
      u64 bits = inner.value.int_value & comp::mask(tag_of(inner.type));
      if (comp::is_signed(target) && comp::is_signed(tag_of(inner.type))) {
        const u32 bytes = comp::int_bytes(target);
        const u64 sign = bytes >= 8 ? static_cast<u64>(1) << 63
                                    : (static_cast<u64>(1) << (bytes * 8 - 1));
        bits &= comp::mask(target);
        if ((bits & sign) != 0) {
          bits |= ~comp::mask(target);
        }
      } else {
        bits &= comp::mask(target);
      }
      out.value.int_value = bits;
      return true;
    }
    case ast::ExprKind::Call: return comp_eval_call(mod, expr, scope, out);
    case ast::ExprKind::MethodCall:
      return comp_eval_method_call(mod, expr, scope, out);
    case ast::ExprKind::Field: {
      const ast::ExprField& field = node.payload.get<ast::ExprField>();
      CompVal base;
      if (!comp_eval_expr(mod, field.receiver, scope, base)) {
        return false;
      }
      out.type = expr_type_in(mod, expr);
      if (base.value.tag == CompValue::Tag::Tuple) {
        u32 index = 0;
        bool digits = !field.name.name.empty();
        for (char c : field.name.name) {
          if (c < '0' || c > '9') {
            digits = false;
            break;
          }
          index = index * 10 + static_cast<u32>(c - '0');
        }
        if (!digits || index >= base.value.fields.size()) {
          return fail(field.name.span, "unknown tuple field");
        }
        out.value = base.value.fields[index];
        return true;
      }
      if (base.value.tag == CompValue::Tag::Struct) {
        u32 index = 0;
        if (!struct_field_index(base.type, field.name.name, index) ||
            index >= base.value.fields.size()) {
          return fail(field.name.span, "unknown field");
        }
        out.value = base.value.fields[index];
        return true;
      }
      return fail(node.span, "field without value");
    }
    case ast::ExprKind::Question: {
      const ast::ExprQuestion& question = node.payload.get<ast::ExprQuestion>();
      CompVal inner;
      if (!comp_eval_expr(mod, question.inner, scope, inner)) {
        return false;
      }
      out.type = expr_type_in(mod, expr);
      if (inner.value.tag != CompValue::Tag::Enum) {
        return fail(node.span, "'?' without value");
      }
      // Propagation escapes the comp evaluation, matching the runtime
      // early return that `?` lowers to.
      if (inner.value.variant != 0) {
        return fail(node.span, "comp evaluation propagated a failure");
      }
      if (inner.value.fields.empty()) {
        return fail(node.span, "'?' without a success payload");
      }
      out.value = inner.value.fields[0];
      return true;
    }
    case ast::ExprKind::If: {
      const ast::ExprIf& if_node = node.payload.get<ast::ExprIf>();
      const ast::Cond& cond = ast.conds[if_node.cond];
      CompVal test;
      if (!comp_eval_expr(mod, cond.value, scope, test)) {
        return false;
      }
      if (test.value.tag != CompValue::Tag::Bool) {
        return fail(node.span, "condition without value");
      }
      out.type = expr_type_in(mod, expr);
      CompFlow flow;
      if (test.value.bool_value) {
        if (!comp_eval_block(mod, if_node.then_block, scope, flow)) {
          return false;
        }
      } else if (if_node.else_block.is_valid()) {
        if (!comp_eval_block(mod, if_node.else_block, scope, flow)) {
          return false;
        }
      } else {
        out.value.tag = CompValue::Tag::Void;
        return true;
      }
      if (flow.kind != CompFlow::Kind::Value) {
        return fail(node.span, "control escapes the comp branch");
      }
      out.value = flow.value.value;
      return true;
    }
    case ast::ExprKind::Match: {
      const ast::ExprMatch& match = node.payload.get<ast::ExprMatch>();
      CompVal scrutinee;
      if (!comp_eval_expr(mod, match.scrutinee, scope, scrutinee)) {
        return false;
      }
      out.type = expr_type_in(mod, expr);
      for (const ast::ExprMatchArm& arm : match.arms) {
        scope.frames.emplace_back();
        const bool matched = comp_match_pattern(
            mod, arm.pattern, scrutinee, scope, ast.exprs[arm.body].span);
        if (failed_) {
          scope.frames.pop_back();
          return false;
        }
        if (!matched) {
          scope.frames.pop_back();
          continue;
        }
        CompFlow flow;
        const bool ok = comp_eval_expr(mod, arm.body, scope, flow.value);
        scope.frames.pop_back();
        if (!ok) {
          return false;
        }
        out.value = flow.value.value;
        return true;
      }
      return fail(node.span, "match without value");
    }
    case ast::ExprKind::Block: {
      out.type = expr_type_in(mod, expr);
      CompFlow flow;
      if (!comp_eval_block(mod, node.payload.get<ast::ExprBlock>().block, scope,
                           flow)) {
        return false;
      }
      if (flow.kind != CompFlow::Kind::Value) {
        return fail(node.span, "control escapes the comp block");
      }
      out.value = flow.value.value;
      return true;
    }
    case ast::ExprKind::Loop: {
      out.type = expr_type_in(mod, expr);
      CompFlow flow;
      if (!comp_eval_loop(mod, node.payload.get<ast::ExprLoop>().body, scope,
                          flow, true, node.span)) {
        return false;
      }
      out.value.tag = CompValue::Tag::Void;
      return true;
    }
    case ast::ExprKind::While: {
      const ast::ExprWhile& while_node = node.payload.get<ast::ExprWhile>();
      const ast::Cond& cond = ast.conds[while_node.cond];
      out.type = expr_type_in(mod, expr);
      CompFlow flow;
      if (!comp_eval_loop(mod, while_node.body, scope, flow, false, node.span,
                          cond.value)) {
        return false;
      }
      out.value.tag = CompValue::Tag::Void;
      return true;
    }
    case ast::ExprKind::Break:
    case ast::ExprKind::Continue:
    case ast::ExprKind::Return:
    case ast::ExprKind::Range:
    case ast::ExprKind::Closure: break;
    case ast::ExprKind::Tuple: {
      out.type = expr_type_in(mod, expr);
      out.value.tag = CompValue::Tag::Tuple;
      for (ast::ExprIdx element : node.payload.get<ast::ExprTuple>().elements) {
        CompVal field;
        if (!comp_eval_expr(mod, element, scope, field)) {
          return false;
        }
        out.value.fields.push_back(std::move(field.value));
      }
      return true;
    }
    case ast::ExprKind::Array: {
      const ast::ExprArray& array = node.payload.get<ast::ExprArray>();
      out.type = expr_type_in(mod, expr);
      out.value.tag = CompValue::Tag::Array;
      if (array.repeat.is_valid()) {
        CompVal element;
        if (!comp_eval_expr(mod, array.repeat, scope, element)) {
          return false;
        }
        for (u64 i = 0; i < array.count; ++i) {
          out.value.fields.push_back(element.value);
        }
        return true;
      }
      for (ast::ExprIdx element : array.elements) {
        CompVal field;
        if (!comp_eval_expr(mod, element, scope, field)) {
          return false;
        }
        out.value.fields.push_back(std::move(field.value));
      }
      return true;
    }
    case ast::ExprKind::Index: {
      const ast::ExprIndex& index = node.payload.get<ast::ExprIndex>();
      CompVal base;
      if (!comp_eval_expr(mod, index.receiver, scope, base)) {
        return false;
      }
      CompVal position;
      if (!comp_eval_expr(mod, index.index, scope, position)) {
        return false;
      }
      if (base.value.tag != CompValue::Tag::Array ||
          position.value.tag != CompValue::Tag::Int) {
        return fail(node.span, "index without value");
      }
      if (position.value.int_value >= base.value.fields.size()) {
        return fail(node.span, "index out of bounds");
      }
      out.type = expr_type_in(mod, expr);
      out.value =
          base.value.fields[static_cast<usize>(position.value.int_value)];
      return true;
    }
    case ast::ExprKind::Struct: return comp_eval_struct(mod, expr, scope, out);
  }
  return fail(node.span, "expression is not comp-evaluable");
}

bool CompEvaluator::comp_eval_binary(u32 mod,
                                     ast::ExprIdx expr,
                                     CompScope& scope,
                                     CompVal& out) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprBinary& bin = node.payload.get<ast::ExprBinary>();
  CompVal lhs;
  if (!comp_eval_expr(mod, bin.lhs, scope, lhs)) {
    return false;
  }
  CompVal rhs;
  if (!comp_eval_expr(mod, bin.rhs, scope, rhs)) {
    return false;
  }
  out.type = expr_type_in(mod, expr);
  if (bin.op == ast::BinaryOp::And || bin.op == ast::BinaryOp::Or) {
    if (lhs.value.tag != CompValue::Tag::Bool ||
        rhs.value.tag != CompValue::Tag::Bool) {
      return fail(node.span, "logical operand without value");
    }
    out.value.tag = CompValue::Tag::Bool;
    out.value.bool_value = bin.op == ast::BinaryOp::And
                               ? (lhs.value.bool_value && rhs.value.bool_value)
                               : (lhs.value.bool_value || rhs.value.bool_value);
    return true;
  }
  if (bin.op == ast::BinaryOp::Eq || bin.op == ast::BinaryOp::NotEq) {
    bool equal = false;
    if (lhs.value.tag == CompValue::Tag::Int &&
        rhs.value.tag == CompValue::Tag::Int) {
      equal = (lhs.value.int_value & comp::mask(tag_of(lhs.type))) ==
              (rhs.value.int_value & comp::mask(tag_of(rhs.type)));
    } else if (lhs.value.tag == CompValue::Tag::Bool &&
               rhs.value.tag == CompValue::Tag::Bool) {
      equal = lhs.value.bool_value == rhs.value.bool_value;
    } else if (lhs.value.tag == CompValue::Tag::Str &&
               rhs.value.tag == CompValue::Tag::Str) {
      equal = lhs.value.str_value == rhs.value.str_value;
    } else {
      return fail(node.span, "comparison without value");
    }
    out.value.tag = CompValue::Tag::Bool;
    out.value.bool_value = bin.op == ast::BinaryOp::Eq ? equal : !equal;
    return true;
  }
  if (lhs.value.tag != CompValue::Tag::Int ||
      rhs.value.tag != CompValue::Tag::Int) {
    return fail(node.span, "operand without value");
  }
  const ir::TypeTag tag = tag_of(lhs.type);
  const u64 mask = comp::mask(tag);
  const u64 left = lhs.value.int_value & mask;
  const u64 right = rhs.value.int_value & mask;
  if (bin.op == ast::BinaryOp::Gt || bin.op == ast::BinaryOp::Lt ||
      bin.op == ast::BinaryOp::GtEq || bin.op == ast::BinaryOp::LtEq) {
    bool ordered = false;
    if (comp::is_signed(tag)) {
      const i64 sl = comp::sign_extend(left, tag);
      const i64 sr = comp::sign_extend(right, tag);
      switch (bin.op) {
        case ast::BinaryOp::Gt: ordered = sl > sr; break;
        case ast::BinaryOp::Lt: ordered = sl < sr; break;
        case ast::BinaryOp::GtEq: ordered = sl >= sr; break;
        default: ordered = sl <= sr; break;
      }
    } else {
      switch (bin.op) {
        case ast::BinaryOp::Gt: ordered = left > right; break;
        case ast::BinaryOp::Lt: ordered = left < right; break;
        case ast::BinaryOp::GtEq: ordered = left >= right; break;
        default: ordered = left <= right; break;
      }
    }
    out.value.tag = CompValue::Tag::Bool;
    out.value.bool_value = ordered;
    return true;
  }
  out.value.tag = CompValue::Tag::Int;
  switch (bin.op) {
    case ast::BinaryOp::Add:
      out.value.int_value = (left + right) & mask;
      return true;
    case ast::BinaryOp::Sub:
      out.value.int_value = (left - right) & mask;
      return true;
    case ast::BinaryOp::Mul:
      out.value.int_value = (left * right) & mask;
      return true;
    case ast::BinaryOp::Div:
    case ast::BinaryOp::Mod: {
      if (right == 0) {
        return fail(node.span, "division by zero in comp evaluation");
      }
      if (comp::is_signed(tag)) {
        const i64 sl = comp::sign_extend(left, tag);
        const i64 sr = comp::sign_extend(right, tag);
        static constexpr i64 MIN = static_cast<i64>(static_cast<u64>(1) << 63);
        if (sl == MIN && sr == -1 && comp::int_bytes(tag) == 8) {
          out.value.int_value = static_cast<u64>(MIN);
          return true;
        }
        const i64 quotient = sl / sr;
        const i64 result = bin.op == ast::BinaryOp::Div ? quotient : (sl % sr);
        out.value.int_value = static_cast<u64>(result) & mask;
        return true;
      }
      out.value.int_value =
          (bin.op == ast::BinaryOp::Div ? (left / right) : (left % right)) &
          mask;
      return true;
    }
    case ast::BinaryOp::BitAnd:
      out.value.int_value = (left & right) & mask;
      return true;
    case ast::BinaryOp::BitOr:
      out.value.int_value = (left | right) & mask;
      return true;
    case ast::BinaryOp::BitXor:
      out.value.int_value = (left ^ right) & mask;
      return true;
    case ast::BinaryOp::Shl:
    case ast::BinaryOp::Shr: {
      if (right >= 64) {
        return fail(node.span, "shift out of range in comp evaluation");
      }
      if (bin.op == ast::BinaryOp::Shl) {
        out.value.int_value = (left << right) & mask;
        return true;
      }
      if (comp::is_signed(tag)) {
        out.value.int_value =
            static_cast<u64>(comp::sign_extend(left, tag) >> right) & mask;
        return true;
      }
      out.value.int_value = (left >> right) & mask;
      return true;
    }
    default: break;
  }
  return fail(node.span, "operator is not comp-evaluable");
}

// Formats into a caller buffer by compile-time expansion: literal
// pieces copy directly, arguments convert per type. Truncation is
// silent; total reports the untruncated size.
}  // namespace analyzer
