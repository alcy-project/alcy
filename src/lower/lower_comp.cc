// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include <cstdlib>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/fmt.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/span.h"
#include "fmt/format.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/opcode.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "lower/lowerer.h"

namespace lower {

bool Lowerer::comp_is_signed(ir::TypeTag tag) {
  return tag == ir::TypeTag::I8 || tag == ir::TypeTag::I16 ||
         tag == ir::TypeTag::I32 || tag == ir::TypeTag::I64;
}

u32 Lowerer::comp_int_bytes(ir::TypeTag tag) {
  switch (tag) {
    case ir::TypeTag::I8:
    case ir::TypeTag::U8: return 1;
    case ir::TypeTag::I16:
    case ir::TypeTag::U16: return 2;
    case ir::TypeTag::I32:
    case ir::TypeTag::U32: return 4;
    default: return 8;
  }
}

u64 Lowerer::comp_mask(ir::TypeTag tag) {
  const u32 bytes = comp_int_bytes(tag);
  return bytes >= 8 ? ~static_cast<u64>(0)
                    : ((static_cast<u64>(1) << (bytes * 8)) - 1);
}

bool Lowerer::comp_fail(diag::Span span, std::string_view what) {
  unsupported(span, what);
  return false;
}

bool Lowerer::comp_tick(diag::Span span) {
  if (comp_budget_ == 0) {
    return comp_fail(span, "comp evaluation budget exhausted");
  }
  --comp_budget_;
  return true;
}

ir::TypeIdx Lowerer::expr_type_in(u32 mod, ast::ExprIdx expr) {
  for (const auto& entry : pkg.modules[mod].expr_types) {
    if (entry.expr == expr && entry.inst == comp_inst_) {
      return entry.type;
    }
  }
  return error_type();
}

const analyzer::CheckedModule::CallTarget* Lowerer::call_target_in(
    u32 mod,
    ast::ExprIdx callee) const {
  for (const auto& entry : pkg.modules[mod].call_targets) {
    if (entry.callee == callee && entry.inst == comp_inst_) {
      return &entry;
    }
  }
  return nullptr;
}

const Lowerer::CompVal* Lowerer::comp_lookup(const CompScope& scope,
                                             std::string_view name) {
  for (usize i = scope.frames.size(); i-- > 0;) {
    for (const auto& binding : scope.frames[i] | std::views::reverse) {
      if (binding.first == name) {
        return &binding.second;
      }
    }
  }
  if (scope.outer != nullptr) {
    for (const auto& binding : *scope.outer | std::views::reverse) {
      if (binding.first == name) {
        return &binding.second;
      }
    }
  }
  return nullptr;
}

// Unescapes with exactly the runtime literal rules so comp strings
// match lowered ones byte for byte.
std::string Lowerer::comp_unescape(std::string_view spelling) {
  std::string bytes;
  if (spelling.size() >= 2) {
    spelling.remove_prefix(1);
    spelling.remove_suffix(1);
  }
  for (usize i = 0; i < spelling.size(); ++i) {
    const char c = spelling[i];
    if (c != '\\' || i + 1 >= spelling.size()) {
      bytes.push_back(c);
      continue;
    }
    const char esc = spelling[++i];
    switch (esc) {
      case 'n': bytes.push_back('\n'); break;
      case 't': bytes.push_back('\t'); break;
      case 'r': bytes.push_back('\r'); break;
      case '\\': bytes.push_back('\\'); break;
      case '"': bytes.push_back('"'); break;
      case '0': bytes.push_back('\0'); break;
      default: bytes.push_back(esc); break;
    }
  }
  return bytes;
}

bool Lowerer::comp_eval_literal(u32 mod, ast::ExprIdx expr, CompVal& out) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::Literal& lit =
      ast.literals[node.payload.get<ast::ExprLiteral>().value];
  const ir::TypeIdx type = expr_type_in(mod, expr);
  if (tag_of(type) == ir::TypeTag::Error) {
    return comp_fail(node.span, "comp operand without type");
  }
  out.type = type;
  if (lit.kind == ast::LiteralKind::Bool) {
    out.value.tag = CompValue::Tag::Bool;
    out.value.bool_value = lit.spelling == "true";
    return true;
  }
  if (lit.kind == ast::LiteralKind::String) {
    out.value.tag = CompValue::Tag::Str;
    out.value.str_value = comp_unescape(lit.spelling);
    return true;
  }
  if (lit.kind == ast::LiteralKind::Integer) {
    out.value.tag = CompValue::Tag::Int;
    out.value.int_value = parse_numeric_value(lit.spelling);
    return true;
  }
  return comp_fail(node.span, "literal is not comp-evaluable");
}

bool Lowerer::comp_bind_pattern(u32 mod,
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
        return comp_fail(span, "pattern is not comp-evaluable");
      }
      const ir::TupleType& shape =
          builder.state()
              .tuple_types[builder.state().types[value.type].as_tuple()];
      const std::span<const ast::PatternIdx> elements =
          node.payload.tuple.elements;
      if (elements.size() != value.value.fields.size() ||
          elements.size() != shape.elements.size()) {
        return comp_fail(span, "tuple pattern arity");
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
        return comp_fail(span, "pattern is not comp-evaluable");
      }
      for (const ast::FieldPattern& field : node.payload.strukt.fields) {
        u32 index = 0;
        if (!struct_field_index(value.type, field.name.name, index) ||
            index >= value.value.fields.size()) {
          return comp_fail(field.name.span, "pattern field without value");
        }
        const ir::TypeIdx member_type = field_type_of(value.type, index, span);
        if (failed) {
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
  return comp_fail(node.span, "pattern is not comp-evaluable");
}

bool Lowerer::comp_match_pattern(u32 mod,
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
        return value.value.int_value == (parse_numeric_value(lit.spelling) &
                                         comp_mask(tag_of(value.type)));
      }
      if (lit.kind == ast::LiteralKind::String) {
        if (value.value.tag != CompValue::Tag::Str) {
          return false;
        }
        return value.value.str_value == comp_unescape(lit.spelling);
      }
      return comp_fail(node.span, "pattern is not comp-evaluable");
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
        return comp_fail(span, "pattern is not comp-evaluable");
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
          return comp_fail(span, "variant arity");
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
          return comp_fail(field.name.span, "pattern field without value");
        }
        const CompVal member{value.value.fields[index],
                             field_type_of(value.type, index, span)};
        if (failed) {
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
        if (failed) {
          scope.frames.back().resize(mark);
          return false;
        }
        scope.frames.back().resize(mark);
      }
      return false;
    }
  }
}

bool Lowerer::comp_eval_struct(u32 mod,
                               ast::ExprIdx expr,
                               CompScope& scope,
                               CompVal& out) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprStruct& strukt = node.payload.get<ast::ExprStruct>();
  out.type = expr_type_in(mod, expr);
  if (tag_of(out.type) != ir::TypeTag::Struct) {
    return comp_fail(node.span, "struct without value");
  }
  const auto* info = struct_info(out.type);
  if (info == nullptr) {
    return comp_fail(node.span, "struct without declaration");
  }
  std::vector<CompValue> fields(info->fields.size());
  std::vector<bool> seen(info->fields.size(), false);
  for (const ast::ExprFieldInit& field : strukt.init) {
    u32 index = 0;
    if (!struct_field_index(out.type, field.name.name, index)) {
      return comp_fail(field.name.span, "unknown field");
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
      return comp_fail(node.span, "struct base without value");
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

bool Lowerer::comp_eval_block(u32 mod,
                              ast::BlockIdx block,
                              CompScope& scope,
                              CompFlow& out) {
  const ast::Block& node = ast.blocks[block];
  scope.frames.emplace_back();
  for (ast::StmtIdx stmt : node.statements) {
    if (failed) {
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
  if (failed) {
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

bool Lowerer::comp_eval_stmt(u32 mod,
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
        return comp_fail(node.span, "compound assignment in comp evaluation");
      }
      if (ast.exprs[reassign.place].kind != ast::ExprKind::Path) {
        return comp_fail(node.span, "place without binding");
      }
      const ast::PathIdx path =
          ast.exprs[reassign.place].payload.get<ast::ExprPath>().idx;
      const std::span<const ast::Ident> segments = ast.paths[path].segments;
      if (segments.size() != 1) {
        return comp_fail(node.span, "place without binding");
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
        for (auto& binding :
             const_cast<std::vector<std::pair<std::string_view, CompVal>>&>(
                 *scope.outer)) {
          if (binding.first == segments[0].name) {
            binding.second = std::move(value);
            out.kind = CompFlow::Kind::Value;
            return true;
          }
        }
      }
      return comp_fail(node.span, "place without binding");
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
bool Lowerer::comp_eval_loop(
    u32 mod,
    ast::BlockIdx body,
    CompScope& scope,
    CompFlow& out,
    bool always,
    diag::Span span,
    ast::ExprIdx cond = ast::ExprIdx(base::kInvalidIdx)) {
  while (!failed) {
    if (!always) {
      CompVal test;
      if (!comp_eval_expr(mod, cond, scope, test)) {
        return false;
      }
      if (test.value.tag != CompValue::Tag::Bool) {
        return comp_fail(span, "condition without value");
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
bool Lowerer::comp_run_fn(u32 def_module,
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
    return comp_fail(span, "callee without body");
  }
  const ast::ItemFn& fn = ast.items[item].payload.get<ast::ItemFn>();
  if (args.size() != params.size() || args.size() != fn.params.size()) {
    return comp_fail(span, "call arity");
  }
  if (comp_call_depth_ >= kCompMaxCallDepth) {
    return comp_fail(span, "comp call depth exhausted");
  }
  ++comp_call_depth_;
  const u32 saved_inst = comp_inst_;
  comp_inst_ = inst;
  CompScope callee_scope;
  callee_scope.frames.emplace_back();
  bool ok = true;
  for (usize i = 0; i < args.size() && ok; ++i) {
    if (!fn.params[i].is_comp) {
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
  --comp_call_depth_;
  comp_inst_ = saved_inst;
  if (!ok) {
    return false;
  }
  out.type = ret;
  if (flow.kind == CompFlow::Kind::Return ||
      flow.kind == CompFlow::Kind::Value) {
    out.value = flow.value.value;
    return true;
  }
  return comp_fail(span, "control escapes the comp call");
}

bool Lowerer::comp_eval_assoc_call(
    u32 mod,
    ast::ExprIdx expr,
    CompScope& scope,
    CompVal& out,
    const analyzer::CheckedModule::CallTarget* target) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  if (target->index >= def.methods.size()) {
    return comp_fail(node.span, "callee without target");
  }
  const analyzer::CheckedModule::MethodInfo& info = def.methods[target->index];
  if (info.receiver != analyzer::CheckedModule::ReceiverKind::None) {
    return comp_fail(node.span, "method without receiver");
  }
  return comp_run_fn(target->module, info.item, info.params, info.ret,
                     callee_inst(target), mod, call.args, scope, node.span,
                     out);
}

// Native compile-time evaluation for string intrinsics. Other
// intrinsics touch runtime state and never evaluate.
bool Lowerer::comp_eval_intrinsic(u32 mod,
                                  const analyzer::CheckedModule::FnSig& sig,
                                  const std::span<const ast::ExprIdx>& args,
                                  CompScope& scope,
                                  diag::Span span,
                                  CompVal& out) {
  const ast::ItemIntrinsic& intrinsic =
      ast.items[sig.item].payload.get<ast::ItemIntrinsic>();
  const std::string_view name = intrinsic.name.name;
  if (name != "str_len" && name != "str_byte" && name != "str_slice") {
    return comp_fail(span, "intrinsic is not comp-evaluable");
  }
  CompVal receiver;
  if (!comp_eval_expr(mod, args[0], scope, receiver)) {
    return false;
  }
  if (receiver.value.tag != CompValue::Tag::Str) {
    return comp_fail(span, "string without value");
  }
  const std::string& bytes = receiver.value.str_value;
  const ir::TypeIdx usize_ty = builder.primitive(
      width == ir::PointerWidth::W64 ? ir::TypeTag::U64 : ir::TypeTag::U32);
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
    return comp_fail(span, "index without value");
  }
  const u64 index = first.value.int_value;
  if (name == "str_byte") {
    if (index >= bytes.size()) {
      return comp_fail(span, "index out of bounds");
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
    return comp_fail(span, "index without value");
  }
  const u64 end = second.value.int_value;
  if (index > end || end > bytes.size()) {
    return comp_fail(span, "slice out of bounds");
  }
  out.type = receiver.type;
  out.value.tag = CompValue::Tag::Str;
  out.value.str_value =
      bytes.substr(static_cast<usize>(index), static_cast<usize>(end - index));
  return true;
}

bool Lowerer::comp_eval_call(u32 mod,
                             ast::ExprIdx expr,
                             CompScope& scope,
                             CompVal& out) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
  if (ast.exprs[call.callee].kind != ast::ExprKind::Path) {
    return comp_fail(node.span, "callee without target");
  }
  const ast::PathIdx path =
      ast.exprs[call.callee].payload.get<ast::ExprPath>().idx;
  const std::span<const ast::Ident> segments = ast.paths[path].segments;
  if (segments.size() == 1) {
    const std::string_view name = segments[0].name;
    if (name == "print" || name == "println" || name == "panic") {
      return comp_fail(node.span, "intrinsic is not comp-evaluable");
    }
  }
  const analyzer::CheckedModule::CallTarget* target =
      call_target_in(mod, call.callee);
  if (target == nullptr) {
    if (const auto* use = variant_use_in(path, comp_inst_)) {
      const std::vector<ir::TypeIdx> payloads =
          variant_payload(use->enum_type, use->variant);
      if (call.args.size() != payloads.size()) {
        return comp_fail(node.span, "variant arity");
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
    return comp_fail(node.span, "callee without target");
  }
  if (target->is_method) {
    return comp_eval_assoc_call(mod, expr, scope, out, target);
  }
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  if (target->index >= def.functions.size()) {
    return comp_fail(node.span, "callee without target");
  }
  const analyzer::CheckedModule::FnSig& sig = def.functions[target->index];
  if (ast.items[sig.item].kind == ast::ItemKind::Intrinsic) {
    return comp_eval_intrinsic(mod, sig, call.args, scope, node.span, out);
  }
  return comp_run_fn(target->module, sig.item, sig.params, sig.ret,
                     callee_inst(target), mod, call.args, scope, node.span,
                     out);
}

bool Lowerer::comp_eval_method_call(u32 mod,
                                    ast::ExprIdx expr,
                                    CompScope& scope,
                                    CompVal& out) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprMethodCall& method = node.payload.get<ast::ExprMethodCall>();
  CompVal receiver;
  if (!comp_eval_expr(mod, method.receiver, scope, receiver)) {
    return false;
  }
  out.type = expr_type_in(mod, expr);
  const analyzer::CheckedModule::CallTarget* target = call_target_in(mod, expr);
  if (target == nullptr || !target->is_method) {
    return comp_fail(node.span, "method without target");
  }
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  if (target->index >= def.methods.size()) {
    return comp_fail(node.span, "method without target");
  }
  const analyzer::CheckedModule::MethodInfo& info = def.methods[target->index];
  if (info.receiver == analyzer::CheckedModule::ReceiverKind::None) {
    return comp_fail(node.span, "method without receiver");
  }
  if (!info.item.is_valid()) {
    return comp_fail(node.span, "method without body");
  }
  const ast::ItemFn& fn = ast.items[info.item].payload.get<ast::ItemFn>();
  if (method.args.size() + 1 != info.params.size() ||
      method.args.size() + 1 != fn.params.size()) {
    return comp_fail(node.span, "call arity");
  }
  if (comp_call_depth_ >= kCompMaxCallDepth) {
    return comp_fail(node.span, "comp call depth exhausted");
  }
  ++comp_call_depth_;
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
  --comp_call_depth_;
  if (!ok) {
    return false;
  }
  out.type = info.ret;
  if (flow.kind == CompFlow::Kind::Return ||
      flow.kind == CompFlow::Kind::Value) {
    out.value = flow.value.value;
    return true;
  }
  return comp_fail(node.span, "control escapes the comp call");
}

Val Lowerer::materialize_comp_value(const CompVal& value, diag::Span span) {
  const ir::TypeTag tag = tag_of(value.type);
  switch (value.value.tag) {
    case CompValue::Tag::Int:
    case CompValue::Tag::Bool: {
      const u64 bits = value.value.tag == CompValue::Tag::Bool
                           ? (value.value.bool_value ? 1 : 0)
                           : value.value.int_value;
      return Val{imm_from_u64(tag, value.type, bits), value.type, false, false};
    }
    case CompValue::Tag::Str: {
      const str::StringPoolId id = strings.intern(value.value.str_value);
      const ir::ImmutableIdx imm =
          builder.immutable({.type = value.type, .data = {.str_id_value = id}});
      return Val{to_operand(imm, value.type), value.type, false, false};
    }
    case CompValue::Tag::Void: return Val{size_one, value.type, false, false};
    case CompValue::Tag::Tuple: {
      const ir::TupleType& shape =
          builder.state()
              .tuple_types[builder.state().types[value.type].as_tuple()];
      if (shape.elements.size() != value.value.fields.size()) {
        internal(span, "comp tuple arity");
        return Val{size_one, error_type(), false, false};
      }
      const ir::RegisterIdx addr =
          emit(ir::Opcode::Alloca, value.type, {size_one});
      for (u32 i = 0; i < static_cast<u32>(shape.elements.size()); ++i) {
        const CompVal field{value.value.fields[i], shape.elements[i]};
        Val lowered = materialize_comp_value(field, span);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::RegisterIdx gep =
            emit(ir::Opcode::GetElementPtr, shape.elements[i],
                 {to_operand(addr, value.type), zero_i32, index_operand(i)});
        emit_void(ir::Opcode::Store,
                  {use_value(lowered), to_operand(gep, shape.elements[i])});
      }
      return Val{to_operand(addr, value.type), value.type, true, false};
    }
    case CompValue::Tag::Array: {
      const ir::ArrayType& shape =
          builder.state()
              .array_types[builder.state().types[value.type].as_array()];
      if (shape.count != value.value.fields.size()) {
        internal(span, "comp array arity");
        return Val{size_one, error_type(), false, false};
      }
      const ir::RegisterIdx addr =
          emit(ir::Opcode::Alloca, value.type, {size_one});
      for (u32 i = 0; i < static_cast<u32>(shape.count); ++i) {
        const CompVal field{value.value.fields[i], shape.element};
        Val lowered = materialize_comp_value(field, span);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::RegisterIdx gep =
            emit(ir::Opcode::GetElementPtr, shape.element,
                 {to_operand(addr, value.type), zero_i32, index_operand(i)});
        emit_void(ir::Opcode::Store,
                  {use_value(lowered), to_operand(gep, shape.element)});
      }
      return Val{to_operand(addr, value.type), value.type, true, false};
    }
    case CompValue::Tag::Struct: {
      const auto* info = struct_info(value.type);
      if (info == nullptr || info->fields.size() != value.value.fields.size()) {
        internal(span, "comp struct arity");
        return Val{size_one, error_type(), false, false};
      }
      const ir::RegisterIdx addr =
          emit(ir::Opcode::Alloca, value.type, {size_one});
      for (u32 i = 0; i < static_cast<u32>(info->fields.size()); ++i) {
        const ir::TypeIdx field_type = field_type_of(value.type, i, span);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const CompVal field{value.value.fields[i], field_type};
        Val lowered = materialize_comp_value(field, span);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::RegisterIdx gep =
            emit(ir::Opcode::GetElementPtr, field_type,
                 {to_operand(addr, value.type), zero_i32, index_operand(i)});
        emit_void(ir::Opcode::Store,
                  {use_value(lowered), to_operand(gep, field_type)});
      }
      return Val{to_operand(addr, value.type), value.type, true, false};
    }
    case CompValue::Tag::Enum: {
      const std::vector<ir::TypeIdx> payloads =
          variant_payload(value.type, value.value.variant);
      if (payloads.size() != value.value.fields.size()) {
        internal(span, "comp variant arity");
        return Val{size_one, error_type(), false, false};
      }
      const ir::TypeIdx slot = enum_slot_type();
      const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, slot, {size_one});
      const ir::RegisterIdx tag_reg =
          emit(ir::Opcode::GetElementPtr, builder.primitive(ir::TypeTag::I32),
               {to_operand(addr, slot), zero_i32, index_operand(0)});
      emit_void(ir::Opcode::Store,
                {disc_operand(value.value.variant), to_operand(tag_reg, slot)});
      if (!payloads.empty() && !is_unit_payload(payloads)) {
        const ir::TypeIdx payload_type = payload_tuple(payloads);
        const ir::RegisterIdx payload =
            emit(ir::Opcode::Alloca, payload_type, {size_one});
        for (usize i = 0; i < payloads.size(); ++i) {
          const CompVal field{value.value.fields[i], payloads[i]};
          Val lowered = materialize_comp_value(field, span);
          if (failed) {
            return Val{size_one, error_type(), false, false};
          }
          const ir::RegisterIdx field_reg =
              emit(ir::Opcode::GetElementPtr, payloads[i],
                   {to_operand(payload, payload_type), zero_i32,
                    index_operand(static_cast<u32>(i))});
          emit_void(ir::Opcode::Store,
                    {use_value(lowered), to_operand(field_reg, payloads[i])});
        }
        const ir::TypeIdx ptr = builder.primitive(ir::TypeTag::Ptr);
        const ir::RegisterIdx slot_field =
            emit(ir::Opcode::GetElementPtr, ptr,
                 {to_operand(addr, slot), zero_i32, index_operand(1)});
        emit_void(ir::Opcode::Store,
                  {to_operand(payload, ptr), to_operand(slot_field, ptr)});
      }
      return Val{to_operand(addr, slot), value.type, true, false};
    }
  }
}

// Top-level comp evaluation: fresh budget and call depth, seeded
// with the current function's comp bindings.
bool Lowerer::comp_evaluate(u32 mod, ast::ExprIdx expr, CompVal& out) {
  CompScope scope;
  scope.outer = &comp_scope_;
  scope.frames.emplace_back();
  comp_budget_ = kCompStepBudget;
  comp_call_depth_ = 0;
  return comp_eval_expr(mod, expr, scope, out);
}

bool Lowerer::comp_eval_expr(u32 mod,
                             ast::ExprIdx expr,
                             CompScope& scope,
                             CompVal& out) {
  if (failed || !comp_tick(ast.exprs[expr].span)) {
    return false;
  }
  const ast::ExprNode& node = ast.exprs[expr];
  switch (node.kind) {
    case ast::ExprKind::Literal: return comp_eval_literal(mod, expr, out);
    case ast::ExprKind::Path: {
      const ast::PathIdx path = node.payload.get<ast::ExprPath>().idx;
      const std::span<const ast::Ident> segments = ast.paths[path].segments;
      if (segments.size() == 1) {
        if (const CompVal* bound = comp_lookup(scope, segments[0].name)) {
          out = *bound;
          return true;
        }
        if (const auto* info = lookup_static(mod, segments[0].name)) {
          if (info->is_const && info->init.is_valid() &&
              ast.exprs[info->init].kind == ast::ExprKind::Literal) {
            return comp_eval_literal(mod, info->init, out);
          }
        }
      }
      if (const auto* use = variant_use_in(path, comp_inst_)) {
        const std::vector<ir::TypeIdx> payloads =
            variant_payload(use->enum_type, use->variant);
        if (!payloads.empty()) {
          return comp_fail(node.span, "variant without call");
        }
        out.type = use->enum_type;
        out.value.tag = CompValue::Tag::Enum;
        out.value.variant = use->variant;
        return true;
      }
      return comp_fail(node.span, "path is not comp-evaluable");
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
          return comp_fail(node.span, "unary operand without value");
        }
        out.value.tag = CompValue::Tag::Bool;
        out.value.bool_value = !inner.value.bool_value;
        return true;
      }
      if (inner.value.tag != CompValue::Tag::Int) {
        return comp_fail(node.span, "unary operand without value");
      }
      const ir::TypeTag tag = tag_of(inner.type);
      const u64 mask = comp_mask(tag);
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
        out.value.bool_value = comp_truth(inner);
        return true;
      }
      if (inner.value.tag == CompValue::Tag::Bool) {
        out.value.tag = CompValue::Tag::Int;
        out.value.int_value = inner.value.bool_value ? 1 : 0;
        return true;
      }
      if (inner.value.tag != CompValue::Tag::Int) {
        return comp_fail(node.span, "cast without value");
      }
      out.value.tag = CompValue::Tag::Int;
      u64 bits = inner.value.int_value & comp_mask(tag_of(inner.type));
      if (comp_is_signed(target) && comp_is_signed(tag_of(inner.type))) {
        const u32 bytes = comp_int_bytes(target);
        const u64 sign = bytes >= 8 ? static_cast<u64>(1) << 63
                                    : (static_cast<u64>(1) << (bytes * 8 - 1));
        bits &= comp_mask(target);
        if ((bits & sign) != 0) {
          bits |= ~comp_mask(target);
        }
      } else {
        bits &= comp_mask(target);
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
          return comp_fail(field.name.span, "unknown tuple field");
        }
        out.value = base.value.fields[index];
        return true;
      }
      if (base.value.tag == CompValue::Tag::Struct) {
        u32 index = 0;
        if (!struct_field_index(base.type, field.name.name, index) ||
            index >= base.value.fields.size()) {
          return comp_fail(field.name.span, "unknown field");
        }
        out.value = base.value.fields[index];
        return true;
      }
      return comp_fail(node.span, "field without value");
    }
    case ast::ExprKind::Question: {
      const ast::ExprQuestion& question = node.payload.get<ast::ExprQuestion>();
      CompVal inner;
      if (!comp_eval_expr(mod, question.inner, scope, inner)) {
        return false;
      }
      out.type = expr_type_in(mod, expr);
      if (inner.value.tag != CompValue::Tag::Enum) {
        return comp_fail(node.span, "'?' without value");
      }
      // Propagation escapes the comp evaluation, matching the runtime
      // early return that `?` lowers to.
      if (inner.value.variant != 0) {
        return comp_fail(node.span, "comp evaluation propagated a failure");
      }
      if (inner.value.fields.empty()) {
        return comp_fail(node.span, "'?' without a success payload");
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
        return comp_fail(node.span, "condition without value");
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
        return comp_fail(node.span, "control escapes the comp branch");
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
        if (failed) {
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
      return comp_fail(node.span, "match without value");
    }
    case ast::ExprKind::Block: {
      out.type = expr_type_in(mod, expr);
      CompFlow flow;
      if (!comp_eval_block(mod, node.payload.get<ast::ExprBlock>().block, scope,
                           flow)) {
        return false;
      }
      if (flow.kind != CompFlow::Kind::Value) {
        return comp_fail(node.span, "control escapes the comp block");
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
    case ast::ExprKind::Range: break;
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
        return comp_fail(node.span, "index without value");
      }
      if (position.value.int_value >= base.value.fields.size()) {
        return comp_fail(node.span, "index out of bounds");
      }
      out.type = expr_type_in(mod, expr);
      out.value =
          base.value.fields[static_cast<usize>(position.value.int_value)];
      return true;
    }
    case ast::ExprKind::Struct: return comp_eval_struct(mod, expr, scope, out);
  }
  return comp_fail(node.span, "expression is not comp-evaluable");
}

bool Lowerer::comp_truth(const CompVal& value) {
  if (value.value.tag == CompValue::Tag::Bool) {
    return value.value.bool_value;
  }
  return value.value.int_value != 0;
}

bool Lowerer::comp_eval_binary(u32 mod,
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
      return comp_fail(node.span, "logical operand without value");
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
      equal = (lhs.value.int_value & comp_mask(tag_of(lhs.type))) ==
              (rhs.value.int_value & comp_mask(tag_of(rhs.type)));
    } else if (lhs.value.tag == CompValue::Tag::Bool &&
               rhs.value.tag == CompValue::Tag::Bool) {
      equal = lhs.value.bool_value == rhs.value.bool_value;
    } else if (lhs.value.tag == CompValue::Tag::Str &&
               rhs.value.tag == CompValue::Tag::Str) {
      equal = lhs.value.str_value == rhs.value.str_value;
    } else {
      return comp_fail(node.span, "comparison without value");
    }
    out.value.tag = CompValue::Tag::Bool;
    out.value.bool_value = bin.op == ast::BinaryOp::Eq ? equal : !equal;
    return true;
  }
  if (lhs.value.tag != CompValue::Tag::Int ||
      rhs.value.tag != CompValue::Tag::Int) {
    return comp_fail(node.span, "operand without value");
  }
  const ir::TypeTag tag = tag_of(lhs.type);
  const u64 mask = comp_mask(tag);
  const u64 left = lhs.value.int_value & mask;
  const u64 right = rhs.value.int_value & mask;
  if (bin.op == ast::BinaryOp::Gt || bin.op == ast::BinaryOp::Lt ||
      bin.op == ast::BinaryOp::GtEq || bin.op == ast::BinaryOp::LtEq) {
    bool ordered = false;
    if (comp_is_signed(tag)) {
      const i64 sl = comp_sign_extend(left, tag);
      const i64 sr = comp_sign_extend(right, tag);
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
        return comp_fail(node.span, "division by zero in comp evaluation");
      }
      if (comp_is_signed(tag)) {
        const i64 sl = comp_sign_extend(left, tag);
        const i64 sr = comp_sign_extend(right, tag);
        static constexpr i64 kMin = static_cast<i64>(static_cast<u64>(1) << 63);
        if (sl == kMin && sr == -1 && comp_int_bytes(tag) == 8) {
          out.value.int_value = static_cast<u64>(kMin);
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
        return comp_fail(node.span, "shift out of range in comp evaluation");
      }
      if (bin.op == ast::BinaryOp::Shl) {
        out.value.int_value = (left << right) & mask;
        return true;
      }
      if (comp_is_signed(tag)) {
        out.value.int_value =
            static_cast<u64>(comp_sign_extend(left, tag) >> right) & mask;
        return true;
      }
      out.value.int_value = (left >> right) & mask;
      return true;
    }
    default: break;
  }
  return comp_fail(node.span, "operator is not comp-evaluable");
}

i64 Lowerer::comp_sign_extend(u64 bits, ir::TypeTag tag) {
  const u32 bytes = comp_int_bytes(tag);
  if (bytes >= 8) {
    return static_cast<i64>(bits);
  }
  const u64 mask = comp_mask(tag);
  const u64 sign = static_cast<u64>(1) << (bytes * 8 - 1);
  bits &= mask;
  if ((bits & sign) != 0) {
    bits |= ~mask;
  }
  return static_cast<i64>(bits);
}

// Formats into a caller buffer by compile-time expansion: literal
// pieces copy directly, arguments convert per type. Truncation is
// silent; total reports the untruncated size.
// Emits one bounded piece copy per format piece into the
// destination buffer, tracking written/total through the state.
bool Lowerer::emit_fmt_pieces(diag::Span span,
                              const std::vector<analyzer::FmtPiece>& pieces,
                              ir::OperandIdx tup_op,
                              const std::vector<ir::TypeIdx>& elem_types,
                              ir::OperandIdx dst_base,
                              u64 capacity_value,
                              FmtState& state) {
  const ir::TypeIdx usize_ty = usize_type();
  const ir::TypeIdx u8_ty = builder.primitive(ir::TypeTag::U8);
  const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
  auto usize_imm = [&](u64 value) {
    return to_operand(
        builder.immutable({.type = usize_ty, .data = {.u64_value = value}}),
        usize_ty);
  };
  state.capacity = usize_imm(capacity_value);
  state.off_addr = emit(ir::Opcode::Alloca, usize_ty, {size_one});
  emit_void(ir::Opcode::Store,
            {usize_imm(0), to_operand(state.off_addr, usize_ty)});
  state.tot_addr = emit(ir::Opcode::Alloca, usize_ty, {size_one});
  emit_void(ir::Opcode::Store,
            {usize_imm(0), to_operand(state.tot_addr, usize_ty)});
  auto advance = [&](ir::OperandIdx delta) {
    const ir::RegisterIdx off = emit(ir::Opcode::Load, usize_ty,
                                     {to_operand(state.off_addr, usize_ty)});
    const ir::RegisterIdx grown =
        emit(ir::Opcode::IntAdd, usize_ty, {to_operand(off, usize_ty), delta});
    emit_void(ir::Opcode::Store, {to_operand(grown, usize_ty),
                                  to_operand(state.off_addr, usize_ty)});
  };
  auto accumulate = [&](ir::OperandIdx full) {
    const ir::RegisterIdx total = emit(ir::Opcode::Load, usize_ty,
                                       {to_operand(state.tot_addr, usize_ty)});
    const ir::RegisterIdx grown =
        emit(ir::Opcode::IntAdd, usize_ty, {to_operand(total, usize_ty), full});
    emit_void(ir::Opcode::Store, {to_operand(grown, usize_ty),
                                  to_operand(state.tot_addr, usize_ty)});
  };
  // Copies [src, len), bounded by the buffer: only chunk reaches
  // the buffer, while full always accrues to the total.
  auto bounded_copy = [&](ir::OperandIdx src, ir::OperandIdx len,
                          ir::OperandIdx full) {
    const ir::RegisterIdx off = emit(ir::Opcode::Load, usize_ty,
                                     {to_operand(state.off_addr, usize_ty)});
    const ir::RegisterIdx remaining =
        emit(ir::Opcode::IntSub, usize_ty,
             {state.capacity, to_operand(off, usize_ty)});
    const ir::RegisterIdx fits =
        emit(ir::Opcode::Lt, boolean, {len, to_operand(remaining, usize_ty)});
    const ir::RegisterIdx chunk =
        emit(ir::Opcode::Select, usize_ty,
             {to_operand(fits, boolean), len, to_operand(remaining, usize_ty)});
    const ir::OperandIdx dst =
        advance_ptr(dst_base, to_operand(off, usize_ty), span);
    if (failed) {
      return false;
    }
    emit_void(ir::Opcode::Memcopy, {dst, src, to_operand(chunk, usize_ty)});
    advance(to_operand(chunk, usize_ty));
    accumulate(full);
    return !failed;
  };
  auto const_str = [&](const std::string& bytes, ir::OperandIdx& ptr_out,
                       ir::OperandIdx& len_out) {
    const str::StringPoolId id = strings.intern(bytes);
    const ir::ImmutableIdx imm =
        builder.immutable({.type = builder.primitive(ir::TypeTag::Str),
                           .data = {.str_id_value = id}});
    const ir::OperandIdx str_op =
        to_operand(imm, builder.primitive(ir::TypeTag::Str));
    const ir::RegisterIdx ptr =
        emit(ir::Opcode::ExtractValue, builder.primitive(ir::TypeTag::Ptr),
             {str_op, index_operand(0)});
    if (failed) {
      return false;
    }
    const ir::RegisterIdx len =
        emit(ir::Opcode::ExtractValue, usize_ty, {str_op, index_operand(1)});
    if (failed) {
      return false;
    }
    ptr_out = to_operand(ptr, builder.primitive(ir::TypeTag::Ptr));
    len_out = to_operand(len, usize_ty);
    return true;
  };
  for (const analyzer::FmtPiece& piece : pieces) {
    if (failed) {
      return false;
    }
    if (!piece.is_arg) {
      ir::OperandIdx ptr = ir::OperandIdx::invalid();
      ir::OperandIdx len = ir::OperandIdx::invalid();
      if (!const_str(piece.literal, ptr, len)) {
        return false;
      }
      if (!bounded_copy(ptr, len, len)) {
        return false;
      }
      continue;
    }
    const ir::TypeIdx elem_ty = elem_types[piece.arg];
    const ir::RegisterIdx elem_addr =
        emit(ir::Opcode::GetElementPtr, elem_ty,
             {tup_op, zero_i32, index_operand(piece.arg)});
    const ir::TypeTag tag = tag_of(elem_ty);
    if (tag == ir::TypeTag::Str) {
      const ir::RegisterIdx loaded =
          emit(ir::Opcode::Load, elem_ty, {to_operand(elem_addr, elem_ty)});
      const ir::RegisterIdx ptr =
          emit(ir::Opcode::ExtractValue, builder.primitive(ir::TypeTag::Ptr),
               {to_operand(loaded, elem_ty), index_operand(0)});
      const ir::RegisterIdx len =
          emit(ir::Opcode::ExtractValue, usize_ty,
               {to_operand(loaded, elem_ty), index_operand(1)});
      if (!bounded_copy(to_operand(ptr, builder.primitive(ir::TypeTag::Ptr)),
                        to_operand(len, usize_ty), to_operand(len, usize_ty))) {
        return false;
      }
      continue;
    }
    if (tag == ir::TypeTag::I1) {
      const ir::RegisterIdx loaded =
          emit(ir::Opcode::Load, elem_ty, {to_operand(elem_addr, elem_ty)});
      ir::OperandIdx true_ptr = ir::OperandIdx::invalid();
      ir::OperandIdx true_len = ir::OperandIdx::invalid();
      ir::OperandIdx false_ptr = ir::OperandIdx::invalid();
      ir::OperandIdx false_len = ir::OperandIdx::invalid();
      if (!const_str("true", true_ptr, true_len) ||
          !const_str("false", false_ptr, false_len)) {
        return false;
      }
      const ir::RegisterIdx ptr =
          emit(ir::Opcode::Select, builder.primitive(ir::TypeTag::Ptr),
               {to_operand(loaded, elem_ty), true_ptr, false_ptr});
      const ir::RegisterIdx len =
          emit(ir::Opcode::Select, usize_ty,
               {to_operand(loaded, elem_ty), true_len, false_len});
      if (!bounded_copy(to_operand(ptr, builder.primitive(ir::TypeTag::Ptr)),
                        to_operand(len, usize_ty), to_operand(len, usize_ty))) {
        return false;
      }
      continue;
    }
    // Integers format as decimal through a scratch buffer.
    const ir::RegisterIdx loaded =
        emit(ir::Opcode::Load, elem_ty, {to_operand(elem_addr, elem_ty)});
    const ir::TypeIdx u64_ty = builder.primitive(ir::TypeTag::U64);
    const ir::RegisterIdx wide =
        emit(ir::Opcode::TypeCast, u64_ty, {to_operand(loaded, elem_ty)});
    const ir::TypeIdx digits_ty =
        builder.array_type(u8_ty, static_cast<u64>(20));
    const ir::RegisterIdx tmp = emit(ir::Opcode::Alloca, digits_ty, {size_one});
    const ir::RegisterIdx n_addr = emit(ir::Opcode::Alloca, u64_ty, {size_one});
    emit_void(ir::Opcode::Store,
              {to_operand(wide, u64_ty), to_operand(n_addr, u64_ty)});
    const ir::RegisterIdx count_addr =
        emit(ir::Opcode::Alloca, usize_ty, {size_one});
    emit_void(ir::Opcode::Store,
              {usize_imm(0), to_operand(count_addr, usize_ty)});
    const ir::BlockIdx head = reserve_block();
    const ir::BlockIdx body = reserve_block();
    const ir::BlockIdx exit = reserve_block();
    emit_br(head);
    switch_to(head);
    emit_br(body);
    switch_to(body);
    const ir::RegisterIdx pending =
        emit(ir::Opcode::Load, u64_ty, {to_operand(n_addr, u64_ty)});
    const ir::RegisterIdx digit = emit(
        ir::Opcode::UintRem, u64_ty,
        {to_operand(pending, u64_ty),
         to_operand(
             builder.immutable({.type = u64_ty, .data = {.u64_value = 10}}),
             u64_ty)});
    const ir::RegisterIdx rest = emit(
        ir::Opcode::UintDiv, u64_ty,
        {to_operand(pending, u64_ty),
         to_operand(
             builder.immutable({.type = u64_ty, .data = {.u64_value = 10}}),
             u64_ty)});
    emit_void(ir::Opcode::Store,
              {to_operand(rest, u64_ty), to_operand(n_addr, u64_ty)});
    const ir::RegisterIdx narrow =
        emit(ir::Opcode::TypeCast, u8_ty, {to_operand(digit, u64_ty)});
    const ir::RegisterIdx glyph =
        emit(ir::Opcode::IntAdd, u8_ty,
             {to_operand(narrow, u8_ty),
              to_operand(
                  builder.immutable({.type = u8_ty, .data = {.u8_value = 48}}),
                  u8_ty)});
    const ir::RegisterIdx written =
        emit(ir::Opcode::Load, usize_ty, {to_operand(count_addr, usize_ty)});
    const ir::RegisterIdx slot =
        emit(ir::Opcode::IntSub, usize_ty,
             {usize_imm(19), to_operand(written, usize_ty)});
    const ir::RegisterIdx cell = emit(
        ir::Opcode::GetElementPtr, u8_ty,
        {to_operand(tmp, digits_ty), zero_i32, to_operand(slot, usize_ty)});
    emit_void(ir::Opcode::Store,
              {to_operand(glyph, u8_ty), to_operand(cell, u8_ty)});
    const ir::RegisterIdx grown =
        emit(ir::Opcode::IntAdd, usize_ty,
             {to_operand(written, usize_ty), usize_imm(1)});
    emit_void(ir::Opcode::Store,
              {to_operand(grown, usize_ty), to_operand(count_addr, usize_ty)});
    const ir::RegisterIdx left =
        emit(ir::Opcode::Load, u64_ty, {to_operand(n_addr, u64_ty)});
    const ir::RegisterIdx more =
        emit(ir::Opcode::Gt, boolean,
             {to_operand(left, u64_ty),
              to_operand(
                  builder.immutable({.type = u64_ty, .data = {.u64_value = 0}}),
                  u64_ty)});
    emit_cond_br(to_operand(more, boolean), body, exit);
    switch_to(exit);
    if (failed) {
      return false;
    }
    const ir::RegisterIdx digits =
        emit(ir::Opcode::Load, usize_ty, {to_operand(count_addr, usize_ty)});
    const ir::RegisterIdx start =
        emit(ir::Opcode::IntSub, usize_ty,
             {usize_imm(20), to_operand(digits, usize_ty)});
    const ir::RegisterIdx first = emit(
        ir::Opcode::GetElementPtr, u8_ty,
        {to_operand(tmp, digits_ty), zero_i32, to_operand(start, usize_ty)});
    if (!bounded_copy(to_operand(first, builder.primitive(ir::TypeTag::Ptr)),
                      to_operand(digits, usize_ty),
                      to_operand(digits, usize_ty))) {
      return false;
    }
  }
  return !failed;
}

Val Lowerer::lower_fmt_write(ast::ExprIdx expr,
                             const analyzer::CheckedModule::FnSig& sig) {
  const ast::ExprNode& node = ast.exprs[expr];
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  if (args.size() != 3) {
    internal(node.span, "call arity");
    return Val{size_one, error_type(), false, false};
  }
  CompVal fmt_val;
  if (!comp_evaluate(module, args[0], fmt_val) ||
      fmt_val.value.tag != CompValue::Tag::Str) {
    if (!failed) {
      internal(node.span, "format string without value");
    }
    return Val{size_one, error_type(), false, false};
  }
  Val buf = lower_expr(args[1], nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  if (tag_of(buf.type) != ir::TypeTag::MutRef) {
    internal(node.span, "buffer without address");
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx buf_pointee =
      builder.ref_types()[builder.types()[buf.type].as_ref()].pointee;
  if (tag_of(buf_pointee) != ir::TypeTag::Array) {
    internal(node.span, "buffer without address");
    return Val{size_one, error_type(), false, false};
  }
  const u64 capacity =
      builder.array_types()[builder.types()[buf_pointee].as_array()].count;
  Val tup = lower_expr(args[2], nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx tup_type = expr_type(args[2]);
  if (tag_of(tup_type) != ir::TypeTag::Tuple &&
      tag_of(tup_type) != ir::TypeTag::Void) {
    internal(node.span, "arguments without tuple");
    return Val{size_one, error_type(), false, false};
  }
  if (!tup.address && tag_of(tup_type) == ir::TypeTag::Tuple) {
    tup = address_of(tup);
  }
  std::vector<ir::TypeIdx> elem_types;
  if (tag_of(tup_type) == ir::TypeTag::Tuple) {
    const ir::TupleType& shape =
        builder.state().tuple_types[builder.state().types[tup_type].as_tuple()];
    elem_types.reserve(shape.elements.size());
    for (ir::TypeIdx element : shape.elements) {
      elem_types.push_back(element);
    }
  }
  const analyzer::FmtParse parsed =
      analyzer::parse_format_string(fmt_val.value.str_value);
  if (parsed.error != analyzer::FmtError::None) {
    unsupported(node.span, "invalid format string");
    return Val{size_one, error_type(), false, false};
  }
  if (parsed.placeholders != elem_types.size()) {
    unsupported(node.span, "placeholder count does not match arguments");
    return Val{size_one, error_type(), false, false};
  }
  for (ir::TypeIdx element : elem_types) {
    if (!analyzer::is_formattable_tag(tag_of(element))) {
      unsupported(node.span, "argument is not formattable");
      return Val{size_one, error_type(), false, false};
    }
  }
  const ir::TypeIdx usize_ty = usize_type();
  FmtState state;
  if (!emit_fmt_pieces(node.span, parsed.pieces, tup.op, elem_types, buf.op,
                       capacity, state)) {
    return Val{size_one, error_type(), false, false};
  }
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx written =
      emit(ir::Opcode::Load, usize_ty, {to_operand(state.off_addr, usize_ty)});
  const ir::RegisterIdx total =
      emit(ir::Opcode::Load, usize_ty, {to_operand(state.tot_addr, usize_ty)});
  const ir::RegisterIdx slot = emit(ir::Opcode::Alloca, sig.ret, {size_one});
  const ir::TypeIdx outcome_written = field_type_of(sig.ret, 0, node.span);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx outcome_total = field_type_of(sig.ret, 1, node.span);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx field0 =
      emit(ir::Opcode::GetElementPtr, outcome_written,
           {to_operand(slot, sig.ret), zero_i32, index_operand(0)});
  emit_void(ir::Opcode::Store, {to_operand(written, outcome_written),
                                to_operand(field0, outcome_written)});
  const ir::RegisterIdx field1 =
      emit(ir::Opcode::GetElementPtr, outcome_total,
           {to_operand(slot, sig.ret), zero_i32, index_operand(1)});
  emit_void(ir::Opcode::Store, {to_operand(total, outcome_total),
                                to_operand(field1, outcome_total)});
  return Val{to_operand(slot, sig.ret), sig.ret, true, false};
}

Val Lowerer::lower_fmt_format(ast::ExprIdx expr,
                              const analyzer::CheckedModule::FnSig& sig) {
  const ast::ExprNode& node = ast.exprs[expr];
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  if (args.size() != 2) {
    internal(node.span, "call arity");
    return Val{size_one, error_type(), false, false};
  }
  CompVal fmt_val;
  if (!comp_evaluate(module, args[0], fmt_val) ||
      fmt_val.value.tag != CompValue::Tag::Str) {
    if (!failed) {
      internal(node.span, "format string without value");
    }
    return Val{size_one, error_type(), false, false};
  }
  Val tup = lower_expr(args[1], nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx tup_type = expr_type(args[1]);
  ir::OperandIdx tup_op = size_one;
  std::vector<ir::TypeIdx> elem_types;
  if (tag_of(tup_type) == ir::TypeTag::Tuple) {
    if (!tup.address) {
      tup = address_of(tup);
    }
    tup_op = tup.op;
    const ir::TupleType& shape =
        builder.state().tuple_types[builder.state().types[tup_type].as_tuple()];
    for (ir::TypeIdx element : shape.elements) {
      elem_types.push_back(element);
    }
  } else if (tag_of(tup_type) != ir::TypeTag::Void) {
    internal(node.span, "arguments without tuple");
    return Val{size_one, error_type(), false, false};
  }
  const analyzer::FmtParse parsed =
      analyzer::parse_format_string(fmt_val.value.str_value);
  if (parsed.error != analyzer::FmtError::None) {
    unsupported(node.span, "invalid format string");
    return Val{size_one, error_type(), false, false};
  }
  if (parsed.placeholders != elem_types.size()) {
    unsupported(node.span, "placeholder count does not match arguments");
    return Val{size_one, error_type(), false, false};
  }
  for (ir::TypeIdx element : elem_types) {
    if (!analyzer::is_formattable_tag(tag_of(element))) {
      unsupported(node.span, "argument is not formattable");
      return Val{size_one, error_type(), false, false};
    }
  }
  const ir::TypeIdx usize_ty = usize_type();
  const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
  u32 buf_field = 0;
  u32 len_field = 1;
  if (!struct_field_index(sig.ret, "buf", buf_field) ||
      !struct_field_index(sig.ret, "len", len_field)) {
    internal(node.span, "string without fields");
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx buf_field_ty = field_type_of(sig.ret, buf_field, node.span);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::ArrayType& buf_shape =
      builder.state()
          .array_types[builder.state().types[buf_field_ty].as_array()];
  const ir::RegisterIdx slot = emit(ir::Opcode::Alloca, sig.ret, {size_one});
  const ir::RegisterIdx buf_addr =
      emit(ir::Opcode::GetElementPtr, buf_field_ty,
           {to_operand(slot, sig.ret), zero_i32, index_operand(buf_field)});
  FmtState state;
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  if (!emit_fmt_pieces(node.span, parsed.pieces, tup_op, elem_types,
                       to_operand(buf_addr, ptr_ty), buf_shape.count, state)) {
    return Val{size_one, error_type(), false, false};
  }
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx written =
      emit(ir::Opcode::Load, usize_ty, {to_operand(state.off_addr, usize_ty)});
  const ir::RegisterIdx total =
      emit(ir::Opcode::Load, usize_ty, {to_operand(state.tot_addr, usize_ty)});
  const ir::RegisterIdx truncated =
      emit(ir::Opcode::Ne, boolean,
           {to_operand(written, usize_ty), to_operand(total, usize_ty)});
  const ir::BlockIdx ok_block = reserve_block();
  const ir::BlockIdx bad_block = reserve_block();
  emit_cond_br(to_operand(truncated, boolean), bad_block, ok_block);
  switch_to(bad_block);
  emit_panic(str_operand("format output truncated"));
  switch_to(ok_block);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx len_ty = field_type_of(sig.ret, len_field, node.span);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx len_addr =
      emit(ir::Opcode::GetElementPtr, len_ty,
           {to_operand(slot, sig.ret), zero_i32, index_operand(len_field)});
  emit_void(ir::Opcode::Store,
            {to_operand(written, len_ty), to_operand(len_addr, len_ty)});
  return Val{to_operand(slot, sig.ret), sig.ret, true, false};
}
}  // namespace lower
