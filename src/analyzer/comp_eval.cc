// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "analyzer/comp_eval.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/const_ir_eval.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "base/name.h"
#include "comp/comp_value.h"
#include "comp/ir_interpreter.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "text/unescape.h"

namespace analyzer {

CompEval::CompEval(CompInputs inputs,
                   const ast::AstArena& ast,
                   ir::StorageBuilder& builder,
                   ir::PointerWidth width)
    : inputs_(inputs), ast(ast), builder(builder), width(width) {
  generic_insts_.reserve(inputs_.generic_insts.size());
  for (u32 i = 0; i < static_cast<u32>(inputs_.generic_insts.size()); ++i) {
    generic_insts_.emplace(inputs_.generic_insts[i].idx, i);
  }
  type_origins_.reserve(inputs_.type_origins.size());
  for (usize i = inputs_.type_origins.size(); i > 0; --i) {
    type_origins_.emplace(inputs_.type_origins[i - 1].first.idx,
                          inputs_.type_origins[i - 1].second.idx);
  }
  for (const CheckedModule& checked : inputs_.modules) {
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
    for (const CheckedModule::ValueSplice& splice : checked.value_splices) {
      const ast::ExprNode& node = ast.exprs[splice.expr];
      if (node.kind == ast::ExprKind::Path) {
        path_splices_.emplace(node.payload.get<ast::ExprPath>().idx.idx,
                              splice.expr.idx);
      }
    }
  }
}

ir::TypeIdx CompEval::usize_type() {
  return builder.primitive(ir::TypeTag::Usize);
}

ir::TypeIdx CompEval::type_origin(ir::TypeIdx type) const {
  const auto found = type_origins_.find(type.idx);
  return found == type_origins_.end() ? type : ir::TypeIdx(found->second);
}

const CheckedModule::StructInfo* CompEval::struct_info(ir::TypeIdx type) const {
  const ir::TypeIdx origin = type_origin(type);
  const auto found = structs_.find(origin.idx);
  return found == structs_.end() ? nullptr : found->second;
}

bool CompEval::struct_field_index(ir::TypeIdx type,
                                  std::string_view name,
                                  u32& index_out) {
  const CheckedModule::StructInfo* info = struct_info(type);
  if (info == nullptr) {
    return false;
  }
  const auto& fields = info->fields;
  for (u32 i = 0; i < static_cast<u32>(fields.size()); ++i) {
    if (base::same_name(fields[i], name)) {
      index_out = i;
      return true;
    }
  }
  return false;
}

bool CompEval::variant_index(ir::TypeIdx enum_type,
                             std::string_view name,
                             u32& index_out) {
  const auto found = enums_.find(type_origin(enum_type).idx);
  if (found == enums_.end()) {
    return false;
  }
  const CheckedModule::EnumInfo* info = found->second;
  for (u32 i = 0; i < static_cast<u32>(info->variants.size()); ++i) {
    if (base::same_name(info->variants[i], name)) {
      index_out = i;
      return true;
    }
  }
  return false;
}

ir::TypeIdx CompEval::field_type_of(ir::TypeIdx base,
                                    u32 index,
                                    diag::Span span) {
  const ir::TypeTag tag = tag_of(base);
  if (tag == ir::TypeTag::Struct) {
    const ir::StructType struct_type =
        builder.state().struct_types[builder.state().types[base].as_struct()];
    if (index < struct_type.fields.size()) {
      return struct_type.fields[index];
    }
  } else if (tag == ir::TypeTag::Tuple) {
    const ir::TupleType tuple_type =
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

bool CompEval::fail_as(Failure kind, diag::Span span, std::string_view what) {
  if (failure_ == Failure::None) {
    failure_ = kind;
    fail_span_ = span;
    fail_what_ = what;
  }
  return false;
}

bool CompEval::fail(diag::Span span, std::string_view what) {
  return fail_as(Failure::Program, span, what);
}

bool CompEval::fail_internal(diag::Span span, std::string_view what) {
  return fail_as(Failure::Internal, span, what);
}

bool CompEval::fail_overflow(diag::Span span, std::string_view what) {
  return fail_as(Failure::Overflow, span, what);
}

bool CompEval::fail_div_zero(diag::Span span, std::string_view what) {
  return fail_as(Failure::DivZero, span, what);
}

bool CompEval::fail_oob(diag::Span span, std::string_view what) {
  return fail_as(Failure::OutOfBounds, span, what);
}

bool CompEval::fail_budget(diag::Span span, std::string_view what) {
  return fail_as(Failure::Budget, span, what);
}

bool CompEval::fail_expansion_cap(diag::Span span) {
  return fail_as(Failure::ExpansionCap, span,
                 "comp expansion exceeded its iteration cap");
}

void CompEval::clear_failure() {
  failure_ = Failure::None;
  fail_span_ = diag::Span{};
  fail_what_ = {};
}

const CheckedModule::StaticInfo* CompEval::lookup_static_in(
    u32 mod,
    std::string_view name) const {
  return find_static(inputs_.modules, inputs_.tree, mod, name);
}

const CheckedModule::VariantUse* CompEval::variant_use_in(ast::PathIdx path,
                                                          u32 inst) const {
  const auto found = variants_.find((static_cast<u64>(path.idx) << 32) |
                                    static_cast<u64>(inst));
  if (found != variants_.end()) {
    return found->second;
  }
  if (inst == PARAM_INST) {
    return nullptr;
  }
  const auto declared = variants_.find((static_cast<u64>(path.idx) << 32) |
                                       static_cast<u64>(PARAM_INST));
  return declared == variants_.end() ? nullptr : declared->second;
}

// A range expression is `Range<T>` data: a struct of two `Bound<T>`
// values. The checker records the variant uses and payload types, so
// this builds the same value the runtime lowering stores.
std::vector<ir::TypeIdx> CompEval::variant_payload(ir::TypeIdx enum_type,
                                                   u32 variant) const {
  const ir::EnumType& enum_ty =
      builder.state()
          .enum_types[builder.state().types[type_origin(enum_type)].as_enum()];
  std::vector<ir::TypeIdx> payloads;
  u32 at = enum_ty.variants.head().idx + variant;
  const ir::EnumVariantType variant_ty =
      builder.state().enum_variant_types[ir::EnumVariantTypeIdx(at)];
  for (ir::TypeIdx field : variant_ty.fields) {
    payloads.push_back(field);
  }
  return payloads;
}

u32 CompEval::generic_inst_index(ir::TypeIdx type) const {
  const auto found = generic_insts_.find(type.idx);
  return found == generic_insts_.end() ? NO_INST : found->second;
}

u32 CompEval::callee_inst(const CheckedModule::CallTarget* target) const {
  if (target == nullptr) {
    return NO_INST;
  }
  if (target->is_method) {
    const CheckedModule& def = inputs_.modules[target->module];
    if (target->index >= def.methods.size()) {
      return NO_INST;
    }
    const CheckedModule::MethodInfo& info = def.methods[target->index];
    // A method with its own type arguments carries the instantiation its
    // body was checked under; a plain method's is its receiver's. The
    // definition-site key the generic pass writes names no
    // instantiation, so it falls back rather than reading a body checked
    // against declared parameters as if it were a value.
    if (info.method_inst != NO_INST && info.method_inst != PARAM_INST) {
      return info.method_inst;
    }
    return generic_inst_index(info.self_type);
  }
  const CheckedModule& def = inputs_.modules[target->module];
  if (target->index >= def.functions.size()) {
    return NO_INST;
  }
  const u32 inst = def.functions[target->index].inst;
  // A free function's definition-site row keys a body that only the
  // parametric pass reads, so nothing is left to read here.
  return inst == PARAM_INST ? NO_INST : inst;
}

CompEval::EvalResult CompEval::evaluate(
    u32 module,
    ast::ExprIdx expr,
    u32 inst,
    std::vector<std::pair<std::string_view, CompVal>>* outer) {
  clear_failure();
  CompScope scope;
  scope.outer = outer;
  scope.frames.emplace_back();
  quota_ = COMP_BRANCH_QUOTA;
  call_depth_ = 0;
  inst_ = inst;
  CompVal value;
  if (!try_ir_eval(module, expr, scope, value)) {
    return base::make_err(
        EvalFailure{failure_, fail_span_, std::move(fail_what_)});
  }
  return base::make_ok(std::move(value));
}

CompEval::EvalResult CompEval::evaluate_item(u32 module,
                                             ast::ExprIdx init,
                                             u32 inst) {
  clear_failure();
  quota_ = COMP_BRANCH_QUOTA;
  call_depth_ = 0;
  inst_ = inst;
  CompScope scope;
  scope.frames.emplace_back();
  CompVal value;
  if (!try_ir_eval(module, init, scope, value)) {
    return base::make_err(
        EvalFailure{failure_, fail_span_, std::move(fail_what_)});
  }
  return base::make_ok(std::move(value));
}

ir::TypeIdx CompEval::expr_type_in(u32 mod, ast::ExprIdx expr) {
  const analyzer::CheckedModule::ExprType* entry =
      inputs_.modules[mod].expr_type_of(expr, inst_);
  return entry != nullptr ? entry->type : error_type();
}

ir::TypeIdx CompEval::expr_type_in_fallback(u32 mod, ast::ExprIdx expr) {
  const ir::TypeIdx concrete = expr_type_in(mod, expr);
  if (tag_of(concrete) != ir::TypeTag::Error) {
    return concrete;
  }
  const u32 saved = inst_;
  inst_ = PARAM_INST;
  const ir::TypeIdx declared = expr_type_in(mod, expr);
  inst_ = saved;
  return declared;
}

ir::TypeIdx CompEval::expr_type_at(u32 mod, u32 inst, ast::ExprIdx expr) {
  const u32 saved = inst_;
  inst_ = inst;
  const ir::TypeIdx type = expr_type_in(mod, expr);
  inst_ = saved;
  return type;
}

const analyzer::CheckedModule::CallTarget*
CompEval::call_target_at(u32 mod, u32 inst, ast::ExprIdx callee) const {
  const analyzer::CheckedModule& module = inputs_.modules[mod];
  if (const auto* target = module.callee_of(callee, inst)) {
    return target;
  }
  // A generic body reached from a comp call is checked once at its
  // definition site, so its inner calls are keyed by PARAM_INST rather
  // than the caller's concrete instantiation. The call target itself
  // names the method to dispatch; its body's expressions carry their
  // own instantiation keys.
  return module.callee_of(callee, PARAM_INST);
}

const analyzer::CheckedModule::CallTarget* CompEval::call_target_in(
    u32 mod,
    ast::ExprIdx callee) const {
  return call_target_at(mod, inst_, callee);
}

const CheckedModule::ValueSplice* CompEval::value_splice_in(u32 mod,
                                                            ast::ExprIdx expr,
                                                            u32 inst) const {
  return inputs_.modules[mod].value_splice_of(expr, inst);
}

bool CompEval::comp_bind_pattern(u32 mod,
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
    case ast::PatternKind::Name: {
      const ast::Path& path = ast.paths[node.payload.name.path];
      scope.frames.back().emplace_back(path.segments.back().name, value);
      return true;
    }
    case ast::PatternKind::MutIdent:
      scope.frames.back().emplace_back(node.payload.mut_ident.name.name, value);
      return true;
    case ast::PatternKind::Tuple: {
      if (node.payload.tuple.path.is_valid()) {
        // Variant patterns bind through matching.
        return comp_match_pattern(mod, pattern, value, scope, span);
      }
      if (value.value.tag != comp::CompValue::Tag::Tuple) {
        return fail(span, "pattern is not comp-evaluable");
      }
      const ir::TupleType shape =
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
      if (value.value.tag != comp::CompValue::Tag::Struct) {
        return fail(span, "pattern is not comp-evaluable");
      }
      for (const ast::FieldPattern& field : node.payload.strukt.fields) {
        u32 index = 0;
        if (!struct_field_index(value.type, field.name.name, index) ||
            index >= value.value.fields.size()) {
          return fail(field.name.span, "pattern field without value");
        }
        const ir::TypeIdx member_type = field_type_of(value.type, index, span);
        if (failed()) {
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
    case ast::PatternKind::Literal:
    case ast::PatternKind::Range: break;
  }
  return fail(node.span, "pattern is not comp-evaluable");
}

bool CompEval::comp_match_pattern(u32 mod,
                                  ast::PatternIdx pattern,
                                  const CompVal& value,
                                  CompScope& scope,
                                  diag::Span span) {
  const ast::PatternNode& node = ast.patterns[pattern];
  switch (node.kind) {
    case ast::PatternKind::Wildcard: return true;
    case ast::PatternKind::Ident:
    case ast::PatternKind::MutIdent:
    case ast::PatternKind::Name:
      return comp_bind_pattern(mod, pattern, value, scope, span);
    case ast::PatternKind::Range: {
      if (value.value.tag != comp::CompValue::Tag::Int) {
        return false;
      }
      const ast::PatternRange& range = node.payload.range;
      const auto literal_value = [&](ast::LiteralIdx idx) {
        const ast::Literal& lit = ast.literals[idx];
        if (lit.kind == ast::LiteralKind::Char) {
          return static_cast<u64>(text::char_literal_value(lit.spelling));
        }
        u64 magnitude = comp::parse_numeric_value(lit.spelling);
        return lit.is_negative ? 0 - magnitude : magnitude;
      };
      const u64 v = value.value.int_value;
      const u64 lo = literal_value(range.start);
      const u64 hi = literal_value(range.end);
      if (v < lo) {
        return false;
      }
      return range.inclusive ? v <= hi : v < hi;
    }
    case ast::PatternKind::Literal: {
      CompVal expected;
      expected.type = value.type;
      const ast::Literal& lit = ast.literals[node.payload.literal.value];
      if (lit.kind == ast::LiteralKind::Bool) {
        if (value.value.tag != comp::CompValue::Tag::Bool) {
          return false;
        }
        return value.value.bool_value == (lit.spelling == "true");
      }
      if (lit.kind == ast::LiteralKind::Integer) {
        if (value.value.tag != comp::CompValue::Tag::Int) {
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
        if (value.value.tag != comp::CompValue::Tag::Str) {
          return false;
        }
        return comp::string_bytes(value.value, strings()) ==
               comp::unescape(lit.spelling);
      }
      return fail(node.span, "pattern is not comp-evaluable");
    }
    case ast::PatternKind::Tuple: {
      if (!node.payload.tuple.path.is_valid()) {
        if (value.value.tag != comp::CompValue::Tag::Tuple) {
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
      if (value.value.tag == comp::CompValue::Tag::Enum) {
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
      if (value.value.tag != comp::CompValue::Tag::Struct) {
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
        if (failed()) {
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
        if (failed()) {
          scope.frames.back().resize(mark);
          return false;
        }
        scope.frames.back().resize(mark);
      }
      return false;
    }
  }
}

ir::TypeIdx CompEval::type_of(EvalSite site, ast::ExprIdx expr) {
  return expr_type_at(site.module, site.inst, expr);
}

ir::TypeIdx CompEval::layout_type_of(EvalSite site, ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  if (node.kind != ast::ExprKind::Call) {
    return ir::TypeIdx(base::INVALID_IDX);
  }
  const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
  if (call.type_args.empty()) {
    return ir::TypeIdx(base::INVALID_IDX);
  }
  if (!call.args.empty()) {
    const ir::TypeIdx measured =
        expr_type_at(site.module, site.inst, call.args[0]);
    if (tag_of(measured) != ir::TypeTag::Error) {
      return measured;
    }
  }
  const CheckedModule::CallTarget* target =
      call_target_at(site.module, site.inst, call.callee);
  if (target != nullptr &&
      target->index < inputs_.modules[target->module].functions.size()) {
    const CheckedModule::FnSig& sig =
        inputs_.modules[target->module].functions[target->index];
    for (const FnInstance& candidate : inputs_.fn_insts) {
      if (candidate.item.idx == sig.item.idx && candidate.inst == sig.inst &&
          candidate.args.size() == 1) {
        return candidate.args[0];
      }
    }
  }
  return ir::TypeIdx(base::INVALID_IDX);
}

bool CompEval::path_value(EvalSite site,
                          comp::CompScope& scope,
                          ast::PathIdx path,
                          comp::CompVal& out) {
  const std::span<const ast::Ident> segments = ast.paths[path].segments;
  if (segments.size() != 1) {
    return false;
  }
  if (const comp::CompVal* bound = comp::lookup(scope, segments[0].name)) {
    out = *bound;
    return true;
  }
  if (const auto* info = lookup_static_in(site.module, segments[0].name)) {
    if (info->is_const && info->init.is_valid()) {
      if (call_depth_ >= COMP_MAX_CALL_DEPTH) {
        return fail_budget(ast.paths[path].segments[0].span,
                           "const evaluation is too deep");
      }
      CompVal value;
      const u32 saved_inst = inst_;
      inst_ = site.inst;
      CompScope nested;
      nested.frames.emplace_back();
      ++call_depth_;
      const bool ok = try_ir_eval(site.module, info->init, nested, value);
      --call_depth_;
      inst_ = saved_inst;
      if (!ok) {
        return false;
      }
      out = std::move(value);
      return true;
    }
  }
  const auto found = path_splices_.find(path.idx);
  if (found == path_splices_.end()) {
    return false;
  }
  const ast::ExprIdx splice_expr(found->second);
  const CheckedModule::ValueSplice* splice =
      value_splice_in(site.module, splice_expr, site.inst);
  if (splice == nullptr) {
    return false;
  }
  out.type = splice->type;
  out.value.tag = comp::CompValue::Tag::Int;
  out.value.int_value = splice->value;
  return true;
}

bool CompEval::comp_fn(EvalSite site,
                       ast::ExprIdx call_expr,
                       CompFnInfo& info) {
  const ast::ExprNode& call_node = ast.exprs[call_expr];
  const bool is_method = call_node.kind == ast::ExprKind::MethodCall;
  if (call_node.kind != ast::ExprKind::Call && !is_method) {
    return false;
  }
  const ast::ExprIdx target_expr =
      is_method ? call_expr : call_node.payload.get<ast::ExprCall>().callee;
  const CheckedModule::CallTarget* target =
      call_target_at(site.module, site.inst, target_expr);
  if (target == nullptr) {
    return false;
  }
  const CheckedModule& target_mod = inputs_.modules[target->module];
  if (target->index >= target_mod.functions.size()) {
    return false;
  }
  const CheckedModule::FnSig& sig = target_mod.functions[target->index];
  if (!sig.item.is_valid()) {
    return false;
  }
  const ast::ItemNode& item = ast.items[sig.item];
  if (item.kind != ast::ItemKind::Fn) {
    return false;
  }
  const ast::ItemFn& fn = item.payload.get<ast::ItemFn>();
  if (!fn.body.is_valid()) {
    return false;
  }
  info.body = fn.body;
  info.module = target->module;
  info.inst = callee_inst(target);
  info.ret = sig.ret;
  info.is_method = is_method;
  info.variadic = !fn.params.empty() && fn.params.back().is_variadic;
  info.element = sig.variadic_element;
  for (usize i = 0; i < fn.params.size() && i < sig.params.size(); ++i) {
    info.params.push_back(fn.params[i].pattern);
    info.param_types.push_back(sig.params[i]);
  }
  if (info.variadic && info.params.size() < fn.params.size()) {
    info.params.push_back(fn.params.back().pattern);
    info.param_types.emplace_back(base::INVALID_IDX);
  }
  const usize arg_count =
      is_method ? 1 + call_node.payload.get<ast::ExprMethodCall>().args.size()
                : call_node.payload.get<ast::ExprCall>().args.size();
  return info.variadic ? info.params.size() - 1 <= arg_count
                       : info.params.size() == arg_count;
}

bool CompEval::field_index(ir::TypeIdx type,
                           std::string_view name,
                           u32& index_out) {
  return struct_field_index(type, name, index_out);
}

EmitContext CompEval::emit_context_for(EvalSite site, comp::CompScope& scope) {
  return EmitContext(*this, scope, site);
}

bool CompEval::try_ir_eval(u32 mod,
                           ast::ExprIdx expr,
                           CompScope& scope,
                           CompVal& out) {
  if (quota_ == 0) {
    fail_budget(ast.exprs[expr].span, "comp evaluation quota exhausted");
    return false;
  }
  --quota_;
  const ir::TypeIdx type = expr_type_in(mod, expr);
  if (tag_of(type) == ir::TypeTag::Error) {
    fail_internal(ast.exprs[expr].span, "comp operand without a type");
    return false;
  }
  const EmitContext ctx = emit_context_for({mod, inst_}, scope);
  comp::CompVal ir_value;
  comp::IrRunError ir_error;
  diag::Span span = ast.exprs[expr].span;
  if (!evaluate_const_ir(ast, expr, type, builder, width, ctx, quota_,
                         call_depth_, ir_value, &ir_error, &span)) {
    switch (ir_error.kind) {
      case comp::IrRunError::Kind::DivisionByZero:
        fail_div_zero(span, ir_error.what);
        break;
      case comp::IrRunError::Kind::OutOfBounds:
        fail_oob(span, ir_error.what);
        break;
      case comp::IrRunError::Kind::Overflow:
        fail_overflow(span, ir_error.what);
        break;
      case comp::IrRunError::Kind::Quota:
        fail_budget(span, ir_error.what);
        break;
      case comp::IrRunError::Kind::ExpansionCap:
        fail_expansion_cap(span);
        break;
      case comp::IrRunError::Kind::Depth:
        fail_budget(span, ir_error.what);
        break;
      case comp::IrRunError::Kind::Unsupported:
        fail(span, ir_error.what.empty() ? "comp expression is not evaluable"
                                         : ir_error.what);
        break;
      case comp::IrRunError::Kind::Internal:
        fail_internal(span, ir_error.what.empty()
                                ? "comp evaluation reported an internal error"
                                : ir_error.what);
        break;
    }
    return false;
  }
  out = std::move(ir_value);
  return true;
}

}  // namespace analyzer
