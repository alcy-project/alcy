// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/types.h"
#include "ast/ast.h"
#include "comp/comp_value.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace analyzer {

// One call and one loop back edge spend one unit; nothing raises the
// quota yet, so evaluation is bounded deterministically by control flow.
constexpr u64 COMP_BRANCH_QUOTA = 1'000'000;
// Recursion beyond this depth is refused before it can exhaust the
// machine stack, which the quota alone would not prevent.
constexpr u32 COMP_MAX_CALL_DEPTH = 64;

// The AST engine of compile-time evaluation (ADR-0054): it interprets
// the checked AST, and both readers - the checker for the const values
// types need, the lowering for comp blocks and specialization - ask it
// for values. The value types live in the comp component; the engine
// lives here because both readers can reach the analyzer, and the
// checked tables it reads are exactly what a separate component would
// have to be handed through an interface.
//
// Evaluation is deterministic and bounded; a failure is remembered
// rather than reported, because the two readers report it in their own
// stages.
class CompEvaluator {
 public:
  CompEvaluator(const CheckedPackage& pkg,
                const ast::AstArena& ast,
                ir::StorageBuilder& builder,
                ir::PointerWidth width);

  // One expression at `module`, checked under `inst`. `outer` holds
  // the persistent comp bindings (comp parameters) and may be null;
  // the evaluator reads through it and, as evaluation always has,
  // writes a reassigned binding there.
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

  bool failed() const { return failed_; }
  bool failure_is_internal() const { return fail_internal_; }
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
  using CompValue = comp::CompValue;
  using CompVal = comp::CompVal;
  using CompScope = comp::CompScope;
  using CompFlow = comp::CompFlow;

  // The first failure is kept; later ones are consequences of it.
  bool fail(diag::Span span, std::string_view what);
  bool fail_internal(diag::Span span, std::string_view what);
  // Spends one quota unit; false once the quota is exhausted.
  bool spend(diag::Span span);

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
  bool variant_index(ir::TypeIdx enum_type,
                     std::string_view name,
                     u32& index_out);

  ir::TypeIdx expr_type_in(u32 mod, ast::ExprIdx expr);
  const CheckedModule::CallTarget* call_target_in(u32 mod,
                                                  ast::ExprIdx callee) const;
  const CheckedModule::StaticInfo* lookup_static_in(
      u32 mod,
      std::string_view name) const;
  const CheckedModule::VariantUse* variant_use_in(ast::PathIdx path,
                                                  u32 inst) const;
  std::vector<ir::TypeIdx> variant_payload(ir::TypeIdx enum_type,
                                           u32 variant) const;
  u32 generic_inst_index(ir::TypeIdx type) const;
  u32 callee_inst(const CheckedModule::CallTarget* target) const;

  bool comp_eval_literal(u32 mod, ast::ExprIdx expr, CompVal& out);
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
  bool comp_eval_struct(u32 mod,
                        ast::ExprIdx expr,
                        CompScope& scope,
                        CompVal& out);
  bool comp_eval_block(u32 mod,
                       ast::BlockIdx block,
                       CompScope& scope,
                       CompFlow& out);
  bool comp_eval_stmt(u32 mod,
                      ast::StmtIdx stmt,
                      CompScope& scope,
                      CompFlow& out);
  bool comp_eval_loop(u32 mod,
                      ast::BlockIdx body,
                      CompScope& scope,
                      CompFlow& out,
                      bool always,
                      diag::Span span,
                      ast::ExprIdx cond = ast::ExprIdx(base::INVALID_IDX));
  bool comp_run_fn(u32 def_module,
                   ast::ItemIdx item,
                   const std::vector<ir::TypeIdx>& params,
                   ir::TypeIdx ret,
                   u32 inst,
                   u32 caller_module,
                   const std::span<const ast::ExprIdx>& args,
                   CompScope& caller_scope,
                   diag::Span span,
                   CompVal& out);
  bool comp_eval_assoc_call(u32 mod,
                            ast::ExprIdx expr,
                            CompScope& scope,
                            CompVal& out,
                            const CheckedModule::CallTarget* target);
  bool comp_eval_intrinsic(u32 mod,
                           const CheckedModule::FnSig& sig,
                           const std::span<const ast::ExprIdx>& args,
                           CompScope& scope,
                           diag::Span span,
                           CompVal& out);
  bool comp_eval_call(u32 mod,
                      ast::ExprIdx expr,
                      CompScope& scope,
                      CompVal& out);
  bool comp_eval_method_call(u32 mod,
                             ast::ExprIdx expr,
                             CompScope& scope,
                             CompVal& out);
  bool comp_eval_const_item(u32 mod, ast::ExprIdx init, CompVal& out);
  bool comp_eval_expr(u32 mod,
                      ast::ExprIdx expr,
                      CompScope& scope,
                      CompVal& out);
  bool comp_eval_binary(u32 mod,
                        ast::ExprIdx expr,
                        CompScope& scope,
                        CompVal& out);

  const CheckedPackage& pkg;
  const ast::AstArena& ast;
  ir::StorageBuilder& builder;
  ir::PointerWidth width;
  // The instantiation context the checked side tables are read under;
  // a callee's evaluation swaps it for the callee's own.
  u32 inst_ = NO_INST;
  u64 quota_ = 0;
  u32 call_depth_ = 0;
  bool failed_ = false;
  bool fail_internal_ = false;
  diag::Span fail_span_{};
  std::string_view fail_what_{};
  // The indexes the evaluator used to walk for: a type's instantiation
  // number, a variant use by path and instantiation, and the same
  // shapes the lowering indexes for its own lookups.
  std::unordered_map<u32, u32> generic_insts_;
  std::unordered_map<u64, const CheckedModule::VariantUse*> variants_;
  std::unordered_map<u32, u32> type_origins_;
  std::unordered_map<u32, const CheckedModule::StructInfo*> structs_;
  std::unordered_map<u32, const CheckedModule::EnumInfo*> enums_;
};

}  // namespace analyzer
