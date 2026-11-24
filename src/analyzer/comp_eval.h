// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "comp/comp_value.h"
#include "comp/ir_interpreter.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace analyzer {

struct EmitContext;

// The checked module and instantiation an expression is read under.
struct EvalSite {
  u32 module = 0;
  u32 inst = 0;
};

// The checked shape of a `comp fn` call's callee, for emitting a
// synthetic IR function the interpreter can call.
struct CompFnInfo {
  ast::BlockIdx body = ast::BlockIdx::invalid();
  std::vector<ast::PatternIdx> params;
  std::vector<ir::TypeIdx> param_types;
  ir::TypeIdx ret = ir::TypeIdx(base::INVALID_IDX);
  // A variadic comp fn binds its trailing arguments as a shape; the
  // fixed params bind normally.
  bool variadic = false;
  // The declared element type of a typed variadic, invalid when untyped.
  ir::TypeIdx element = ir::TypeIdx(base::INVALID_IDX);
  // A method call's receiver occupies the first parameter (`self`).
  bool is_method = false;
  // The module and instantiation the callee body was checked under.
  u32 module = 0;
  u32 inst = 0;
};

// One expression at a time spends one unit; nothing raises the quota
// yet, so evaluation is bounded deterministically by control flow.
inline constexpr u64 COMP_BRANCH_QUOTA = comp::COMP_BRANCH_QUOTA;
// Recursion beyond this depth is refused before it can exhaust the
// machine stack, which the quota alone would not prevent.
inline constexpr u32 COMP_MAX_CALL_DEPTH = comp::COMP_MAX_CALL_DEPTH;

// The checked data the evaluator reads. The lowering passes the
// finished package's fields; the checker passes its in-progress
// tables, which is why nothing here owns anything.
struct CompInputs {
  const ModuleTree& tree;
  std::span<const CheckedModule> modules;
  std::span<const ir::TypeIdx> generic_insts;
  // (copy, origin) pairs, newest last.
  std::span<const std::pair<ir::TypeIdx, ir::TypeIdx>> type_origins;
  // The type arguments each generic function or intrinsic bound.
  std::span<const FnInstance> fn_insts;
};

// Compile-time evaluation on the IR interpreter. The checker asks it
// for the values types need and the lowering asks it for comp values;
// both read the same checked tables and run the same synthetic IR.
// A failure is remembered rather than reported, so each reader can
// turn it into the diagnostic its own stage needs.
class CompEval {
 public:
  CompEval(CompInputs inputs,
           const ast::AstArena& ast,
           ir::StorageBuilder& builder,
           ir::PointerWidth width);

  // One expression at `module`, checked under `inst`. `outer` holds
  // the persistent comp bindings (comp parameters) and may be null.
  bool evaluate(u32 module,
                ast::ExprIdx expr,
                u32 inst,
                std::vector<std::pair<std::string_view, comp::CompVal>>* outer,
                comp::CompVal& out);
  // One const item initializer, in item scope with the quota reset;
  // no caller bindings are visible.
  bool evaluate_item(u32 module,
                     ast::ExprIdx init,
                     u32 inst,
                     comp::CompVal& out);

  // Checked-table resolution used by the synthetic IR emitter. Every
  // method reads `site` explicitly, so the emitter does not depend on
  // captured callbacks or evaluator state it cannot see.
  ir::TypeIdx type_of(EvalSite site, ast::ExprIdx expr);
  ir::TypeIdx layout_type_of(EvalSite site, ast::ExprIdx expr);
  bool path_value(EvalSite site,
                  comp::CompScope& scope,
                  ast::PathIdx path,
                  comp::CompVal& out);
  bool comp_fn(EvalSite site, ast::ExprIdx call, CompFnInfo& info);
  bool field_index(ir::TypeIdx type, std::string_view name, u32& index_out);
  bool variant_index(ir::TypeIdx type, std::string_view name, u32& index_out);

  EmitContext emit_context_for(EvalSite site, comp::CompScope& scope);

  enum class Failure : u8 {
    None,
    Program,
    Internal,
    Overflow,
    DivZero,
    OutOfBounds,
    Budget,
    ExpansionCap,
  };

  bool failed() const { return failure_ != Failure::None; }
  Failure failure_kind() const { return failure_; }
  diag::Span failure_span() const { return fail_span_; }
  std::string_view failure() const { return fail_what_; }
  void clear_failure();

  // Binds one evaluated value to `pattern`, for the lowering's own
  // publishing of comp arguments and declarations.
  bool bind_pattern(u32 module,
                    ast::PatternIdx pattern,
                    const comp::CompVal& value,
                    comp::CompScope& scope,
                    diag::Span span) {
    return comp_bind_pattern(module, pattern, value, scope, span);
  }

 private:
  using CompVal = comp::CompVal;
  using CompScope = comp::CompScope;

  bool fail_as(Failure kind, diag::Span span, std::string_view what);
  bool fail(diag::Span span, std::string_view what);
  bool fail_internal(diag::Span span, std::string_view what);
  bool fail_overflow(diag::Span span, std::string_view what);
  bool fail_div_zero(diag::Span span, std::string_view what);
  bool fail_oob(diag::Span span, std::string_view what);
  bool fail_budget(diag::Span span, std::string_view what);
  bool fail_expansion_cap(diag::Span span);

  ir::TypeIdx error_type() { return builder.error_type(); }
  ir::TypeTag tag_of(ir::TypeIdx idx) const {
    return builder.state().types[idx].tag;
  }
  ir::TypeIdx usize_type();
  ir::TypeIdx type_origin(ir::TypeIdx type) const;
  const CheckedModule::StructInfo* struct_info(ir::TypeIdx type) const;
  bool struct_field_index(ir::TypeIdx type,
                          std::string_view name,
                          u32& index_out);
  ir::TypeIdx field_type_of(ir::TypeIdx base, u32 index, diag::Span span);
  std::vector<ir::TypeIdx> variant_payload(ir::TypeIdx enum_type,
                                           u32 variant) const;

  ir::TypeIdx expr_type_in(u32 mod, ast::ExprIdx expr);
  ir::TypeIdx expr_type_in_fallback(u32 mod, ast::ExprIdx expr);
  const CheckedModule::CallTarget* call_target_in(u32 mod,
                                                  ast::ExprIdx callee) const;
  const CheckedModule::ValueSplice* value_splice_in(u32 mod,
                                                    ast::ExprIdx expr,
                                                    u32 inst) const;
  const CheckedModule::StaticInfo* lookup_static_in(
      u32 mod,
      std::string_view name) const;
  const CheckedModule::VariantUse* variant_use_in(ast::PathIdx path,
                                                  u32 inst) const;
  u32 generic_inst_index(ir::TypeIdx type) const;
  u32 callee_inst(const CheckedModule::CallTarget* target) const;

  ir::TypeIdx expr_type_at(u32 mod, u32 inst, ast::ExprIdx expr);
  const CheckedModule::CallTarget* call_target_at(u32 mod,
                                                  u32 inst,
                                                  ast::ExprIdx callee) const;

  bool try_ir_eval(u32 mod, ast::ExprIdx expr, CompScope& scope, CompVal& out);

  bool comp_bind_pattern(u32 mod,
                         ast::PatternIdx pattern,
                         const CompVal& value,
                         CompScope& scope,
                         diag::Span span);
  bool comp_match_pattern(u32 mod,
                          ast::PatternIdx pattern,
                          const CompVal& value,
                          CompScope& scope,
                          diag::Span span);

  CompInputs inputs_;
  const ast::AstArena& ast;
  ir::StorageBuilder& builder;
  ir::PointerWidth width;
  u32 inst_ = NO_INST;
  u64 quota_ = 0;
  u32 call_depth_ = 0;
  Failure failure_ = Failure::None;
  diag::Span fail_span_{};
  std::string fail_what_;
  std::unordered_map<u32, u32> generic_insts_;
  // Path index to the expression that carries a generic value splice.
  std::unordered_map<u32, u32> path_splices_;
  std::unordered_map<u64, const CheckedModule::VariantUse*> variants_;
  std::unordered_map<u32, u32> type_origins_;
  std::unordered_map<u32, const CheckedModule::StructInfo*> structs_;
  std::unordered_map<u32, const CheckedModule::EnumInfo*> enums_;
};

}  // namespace analyzer
