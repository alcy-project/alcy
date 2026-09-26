// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/fmt.h"
#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/seq_builder.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "lower/lower.h"
#include "source/source.h"

namespace lower {

// Diagnostic codes 4300-4319 are reserved for lowering.
constexpr u32 kLowerUnsupported = 4300;
constexpr u32 kLowerInternal = 4301;
constexpr u32 kLowerUnreachable = 4302;
constexpr u32 kLowerDropUnplaced = 4303;
constexpr u32 kLowerDiscardedDestructor = 4304;

// A lowered value: either an SSA operand or the address of one.
// Places stay in address form so moves and borrows observe origins.
struct Val {
  ir::OperandIdx op;
  ir::TypeIdx type;
  bool address = false;
  bool place = false;
};

struct Local {
  std::string_view name;
  ir::RegisterIdx addr;
  ir::TypeIdx type;
  // Ending this value runs code, so a scope exit has to place a call.
  bool needs_drop = false;
  // The value has already left, either through a move out of it or
  // through a call that consumed it. Scope exit must not end it again.
  bool moved = false;
};

class Lowerer {
 public:
  analyzer::CheckedPackage pkg;
  ir::StorageBuilder builder;
  ir::PointerWidth width;
  ast::AstArena& ast;
  str::StringInterner& strings;
  diag::DiagBag& bag;
  bool failed = false;

  // Side tables for ownership analysis: every emitted instruction
  // records its source span, and every address alloca records the
  // bound name (for diagnostics; analysis needs only identity).
  diag::Span cur_span_{};
  std::vector<diag::Span> instr_spans_;
  std::vector<LoweredPackage::AddrInfo> addr_names_;

  // Compile-time values for comp evaluation. Integers ride as u64
  // with their type attached (semantics follow the emitting opcodes:
  // wrapping arithmetic, two's-complement negation); aggregates carry
  // positional fields.
  struct CompValue {
    enum class Tag : u8 {
      Void,
      Int,
      Bool,
      Str,
      Tuple,
      Array,
      Struct,
      Enum,
    };
    Tag tag = Tag::Void;
    u64 int_value = 0;
    bool bool_value = false;
    std::string str_value;
    std::vector<CompValue> fields;
    u32 variant = 0;
  };

  struct CompVal {
    CompValue value;
    ir::TypeIdx type = ir::TypeIdx(base::kInvalidIdx);
  };

  // Lexical comp bindings: persistent per-function bindings plus
  // evaluation-local frames.
  struct CompScope {
    const std::vector<std::pair<std::string_view, CompVal>>* outer = nullptr;
    std::vector<std::vector<std::pair<std::string_view, CompVal>>> frames;
  };

  struct FnEntry {
    ast::ItemIdx item = ast::ItemIdx::invalid();
    // Specialization key over comp argument values; empty for
    // functions without comp parameters.
    std::string comp_key;
    ir::FunctionIdx idx = ir::FunctionIdx(base::kInvalidIdx);
    // Lowering work item, filled at reservation time.
    u32 mod = 0;
    std::string name;
    std::vector<ir::TypeIdx> params;
    ir::TypeIdx ret = ir::TypeIdx(base::kInvalidIdx);
    // Generic instantiation lowered under (kNoInst for plain code).
    u32 inst = analyzer::kNoInst;
    // What the symbol names. A method and an associated function share
    // an item, so the kind comes from the signature, not the item.
    ir::SymbolKind kind = ir::SymbolKind::Free;
    // Type arguments of the instantiation, for the symbol.
    std::vector<ir::TypeIdx> generics;
    // Comp argument values in formal-parameter order.
    std::vector<CompVal> comp_args;
  };
  std::vector<FnEntry> fns;
  // Reservation order matches lowering order (FIFO worklist), so
  // reserved indexes line up with storage positions.
  std::vector<usize> worklist_;
  // Per-function comp bindings (comp parameters); cleared per body.
  std::vector<std::pair<std::string_view, CompVal>> comp_scope_;
  // Lowered prelude functions, excluded from reported counts.
  usize prelude_functions_ = 0;
  // Step budget per top-level comp evaluation; recursion depth guard.
  usize comp_budget_ = 0;
  u32 comp_call_depth_ = 0;
  // Instantiation under lowering (runtime) and under comp
  // evaluation; side-table lookups match these contexts.
  u32 cur_inst_ = analyzer::kNoInst;
  u32 comp_inst_ = analyzer::kNoInst;

  struct ExtEntry {
    std::string_view name;
    ir::ExternalFunctionIdx idx;
  };
  std::vector<ExtEntry> exts;

  // Per-function state.
  u32 module = 0;
  std::vector<Local> locals;
  // One entry per open block, holding the `locals` size on entry. A
  // value declared inside a block ends with it, so a scope exit ends
  // everything from its mark onward.
  std::vector<u32> scope_marks;
  // Instruction streams by reserved block: reservation order matches
  // creation order, so reserved indexes line up with storage positions.
  std::vector<ir::InstrSeq> streams_;
  std::vector<ir::InstructionIdx> stream_last_;
  std::vector<ir::BlockIdx> fn_blocks_;
  ir::BlockIdx cur_{base::kInvalidIdx};
  u32 block_next_ = 0;
  u32 fn_block_base_ = 0;
  bool binding_param_ = false;
  std::vector<ir::BlockIdx> break_targets_;
  std::vector<ir::BlockIdx> continue_targets_;
  static bool is_block_terminator(ir::Opcode op);
  usize at(ir::BlockIdx block) const;
  ir::BlockIdx reserve_block();
  void switch_to(ir::BlockIdx block);
  bool terminated(ir::BlockIdx block);
  bool terminated_cur();

  ir::OperandIdx size_one = ir::OperandIdx(0);
  ir::OperandIdx zero_i32 = ir::OperandIdx(0);
  Lowerer(analyzer::CheckedPackage package,
          ir::PointerWidth width,
          ast::AstArena& ast,
          str::StringInterner& strings,
          diag::DiagBag& bag);
  void unsupported(diag::Span span, std::string_view what);
  void internal(diag::Span span, std::string_view what);
  ir::TypeIdx error_type();
  bool is_copy(ir::TypeIdx type) const;
  ir::TypeTag tag_of(ir::TypeIdx idx) const;
  bool is_ref_tag(ir::TypeTag tag) const;
  bool same_shape(ir::TypeIdx a, ir::TypeIdx b);
  bool same_shape_inner(ir::TypeIdx a, ir::TypeIdx b, std::vector<u64>& seen);
  ir::RegisterIdx claim_reg();
  ir::OperandIdx to_operand(ir::RegisterIdx reg, ir::TypeIdx type);
  ir::OperandIdx to_operand(ir::ImmutableIdx imm, ir::TypeIdx type);
  ir::RegisterIdx emit(ir::Opcode op,
                       ir::TypeIdx type,
                       const std::vector<ir::OperandIdx>& ops);
  void emit_void(ir::Opcode op, const std::vector<ir::OperandIdx>& ops);
  // Emits a type query, which measures a type instead of consuming
  // operands: the measured type rides on the instruction.
  ir::RegisterIdx emit_type_query(ir::Opcode op, ir::TypeIdx measure);
  // Type arguments the checker bound to a generic function or
  // intrinsic, or empty for a non-generic item.
  const std::vector<ir::TypeIdx>& fn_instance_args(u32 module,
                                                   u32 sig_index) const;

  struct SpanGuard {
    Lowerer* lowerer;
    diag::Span previous;
    SpanGuard(Lowerer* lowerer, diag::Span previous)
        : lowerer(lowerer), previous(previous) {}
    ~SpanGuard() { lowerer->cur_span_ = previous; }
  };
  Val materialize(Val v);
  Val address_of(Val v);
  void mark_move(Val v);
  ir::OperandIdx use_value(Val v);
  const Local* lookup_local(std::string_view name) const;
  const analyzer::CheckedModule::StaticInfo* lookup_static(
      u32 mod,
      std::string_view name) const;
  ir::TypeIdx expr_type(ast::ExprIdx expr);
  const analyzer::CheckedModule::CallTarget* call_target(
      ast::ExprIdx callee) const;
  std::vector<u32> comp_positions(ast::ItemIdx item) const;
  static void comp_key_into(std::string& key, const CompValue& value);
  // Reserves (or finds) the IR function for one instantiation. `kind`
  // says what the symbol names, because a method and an associated
  // function share an item.
  ir::FunctionIdx fn_index(u32 mod,
                           ast::ItemIdx item,
                           std::string_view name,
                           const std::vector<ir::TypeIdx>& params,
                           ir::TypeIdx ret,
                           u32 inst,
                           std::vector<CompVal> comp_args,
                           ir::SymbolKind kind = ir::SymbolKind::Free);
  u32 callee_inst(const analyzer::CheckedModule::CallTarget* target) const;
  // Type arguments of a nominal type, empty for a plain declaration.
  std::vector<ir::TypeIdx> nominal_arguments(ir::TypeIdx type) const;
  // Type arguments a generic free function or intrinsic bound.
  std::vector<ir::TypeIdx> fn_args_for(ast::ItemIdx item) const;
  u32 generic_inst_index(ir::TypeIdx type) const;
  const analyzer::CheckedModule::StructInfo* struct_info(ir::TypeIdx type);
  // Follows a field storage copy back to the type it was copied from.
  ir::TypeIdx type_origin(ir::TypeIdx type) const;
  u64 parse_numeric_value(std::string_view spelling);
  ir::TypeTag literal_tag(ast::LiteralIdx value, const ir::TypeIdx* expected);
  Val lower_literal(ast::LiteralIdx lit_idx, const ir::TypeIdx* expected);
  ir::OperandIdx imm_from_u64(ir::TypeTag tag, ir::TypeIdx type, u64 value);
  Val place_addr(ast::ExprIdx expr);
  Val checked_index_addr(Val base, Val position, diag::Span span);
  ir::OperandIdx index_operand(u32 index);
  bool struct_field_index(ir::TypeIdx type,
                          std::string_view name,
                          u32& index_out);
  ir::TypeIdx field_type_of(ir::TypeIdx base, u32 index, diag::Span span);
  void bind_pattern(ast::PatternIdx pattern, Val init);
  Val lower_path(ast::ExprIdx expr, const ir::TypeIdx* expected);
  ir::ExternalFunctionIdx declare_external(
      std::string_view name,
      ir::TypeIdx ret,
      const std::vector<ir::TypeIdx>& params);
  const analyzer::CheckedModule::VariantUse* variant_use(
      ast::PathIdx path) const;
  const analyzer::CheckedModule::VariantUse* variant_use_in(ast::PathIdx path,
                                                            u32 inst) const;
  const analyzer::CheckedModule::EnumInfo* enum_info(ir::TypeIdx type) const;
  bool variant_index(ir::TypeIdx enum_type,
                     std::string_view name,
                     u32& index_out);
  std::vector<ir::TypeIdx> variant_payload(ir::TypeIdx enum_type, u32 variant);
  ir::TypeIdx enum_slot_type();

  ir::TypeIdx enum_slot_type_ = ir::TypeIdx(base::kInvalidIdx);
  Val lower_variant_construct(ast::ExprIdx expr,
                              const analyzer::CheckedModule::VariantUse* use);
  ir::OperandIdx disc_operand(u32 discriminant);
  Val lower_call(ast::ExprIdx expr, const ir::TypeIdx* expected);
  Val lower_associated_call(ast::ExprIdx expr);
  Val lower_intrinsic_call(ast::ExprIdx expr,
                           const analyzer::CheckedModule::FnSig& sig,
                           const std::vector<ir::TypeIdx>& type_args);
  ir::TypeIdx usize_type();
  bool str_parts(Val str, ir::OperandIdx& bytes_out, ir::OperandIdx& len_out);
  ir::OperandIdx advance_ptr(ir::OperandIdx ptr,
                             ir::OperandIdx offset,
                             diag::Span span);
  Val lower_str_intrinsic(ast::ExprIdx expr, std::string_view name);
  Val lower_intrinsic(ast::ExprIdx expr, std::string_view name);
  ir::OperandIdx arg_for(Val arg, ir::TypeIdx param);
  Val enum_addr(Val value);
  Val load_disc(Val slot_addr);
  bool is_unit_payload(const std::vector<ir::TypeIdx>& payloads);
  Val void_value();
  ir::TypeIdx payload_tuple(const std::vector<ir::TypeIdx>& fields);
  Val load_payload_field(Val slot_addr, ir::TypeIdx payload_type, u32 field);
  void emit_br(ir::BlockIdx target);
  void emit_cond_br(ir::OperandIdx cond,
                    ir::BlockIdx then_block,
                    ir::BlockIdx else_block);
  ir::OperandIdx bool_operand(bool value);
  void emit_panic(ir::OperandIdx message);
  ir::OperandIdx str_operand(std::string_view message);
  Val lower_method_call(ast::ExprIdx expr, const ir::TypeIdx* expected);
  Val field_addr(Val base, std::string_view name, diag::Span span);
  Val lower_struct(ast::ExprIdx expr);
  Val lower_tuple(ast::ExprIdx expr);
  Val lower_array(ast::ExprIdx expr);
  ir::Opcode int_binop(ast::BinaryOp op, ir::TypeTag tag);
  Val lower_binary(ast::ExprIdx expr);
  void store_result(Val slot, Val value);
  Val result_slot(ir::TypeIdx type, diag::Span span);
  void lower_arm_test(ast::PatternIdx pattern,
                      Val scrut_addr,
                      ir::TypeIdx scrut_type,
                      ir::BlockIdx body_block,
                      ir::BlockIdx fail_block);
  void expand_or_arms(
      const ast::ExprMatchArm& arm,
      std::vector<std::pair<ast::PatternIdx, ast::ExprIdx>>& out);
  Val lower_match(ast::ExprIdx expr, const ir::TypeIdx* expected);
  Val lower_if(ast::ExprIdx expr, const ir::TypeIdx* expected);
  Val lower_loop(ast::ExprIdx expr);
  Val lower_while(ast::ExprIdx expr);
  Val lower_question(ast::ExprIdx expr);

  // Compile-time evaluation

  static constexpr usize kCompStepBudget = 1u << 20;
  static constexpr u32 kCompMaxCallDepth = 64;
  static bool comp_is_signed(ir::TypeTag tag);
  static u32 comp_int_bytes(ir::TypeTag tag);
  static u64 comp_mask(ir::TypeTag tag);
  bool comp_fail(diag::Span span, std::string_view what);
  bool comp_tick(diag::Span span);
  ir::TypeIdx expr_type_in(u32 mod, ast::ExprIdx expr);
  const analyzer::CheckedModule::CallTarget* call_target_in(
      u32 mod,
      ast::ExprIdx callee) const;
  const CompVal* comp_lookup(const CompScope& scope, std::string_view name);
  static std::string comp_unescape(std::string_view spelling);
  bool comp_eval_literal(u32 mod, ast::ExprIdx expr, CompVal& out);

  struct CompFlow {
    enum class Kind : u8 { Value, Break, Continue, Return };
    Kind kind = Kind::Value;
    CompVal value;
  };
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
                      ast::ExprIdx cond);
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
                            const analyzer::CheckedModule::CallTarget* target);
  bool comp_eval_intrinsic(u32 mod,
                           const analyzer::CheckedModule::FnSig& sig,
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
  Val materialize_comp_value(const CompVal& value, diag::Span span);
  bool comp_evaluate(u32 mod, ast::ExprIdx expr, CompVal& out);
  bool comp_eval_expr(u32 mod,
                      ast::ExprIdx expr,
                      CompScope& scope,
                      CompVal& out);
  static bool comp_truth(const CompVal& value);
  bool comp_eval_binary(u32 mod,
                        ast::ExprIdx expr,
                        CompScope& scope,
                        CompVal& out);
  static i64 comp_sign_extend(u64 bits, ir::TypeTag tag);
  struct FmtState {
    ir::RegisterIdx off_addr = ir::RegisterIdx::invalid();
    ir::RegisterIdx tot_addr = ir::RegisterIdx::invalid();
    ir::OperandIdx capacity = ir::OperandIdx::invalid();
  };
  bool emit_fmt_pieces(diag::Span span,
                       const std::vector<analyzer::FmtPiece>& pieces,
                       ir::OperandIdx tup_op,
                       const std::vector<ir::TypeIdx>& elem_types,
                       ir::OperandIdx dst_base,
                       u64 capacity_value,
                       FmtState& state);
  Val lower_fmt_write(ast::ExprIdx expr,
                      const analyzer::CheckedModule::FnSig& sig);
  Val lower_fmt_format(ast::ExprIdx expr,
                       const analyzer::CheckedModule::FnSig& sig);
  Val lower_expr(ast::ExprIdx expr, const ir::TypeIdx* expected);
  Val lower_literal_zero(ir::TypeIdx type, diag::Span span);
  void lower_stmt(ast::StmtIdx stmt);
  Val lower_block(ast::BlockIdx block, const ir::TypeIdx* expected);
  // Ends every value from `mark` onward, innermost first. A destructor
  // consumes its value, so this runs the move the borrow checker sees
  // as ending the local.
  void emit_drops(u32 mark, diag::Span span);
  // Ends one place: calls the type's destructor if it has one, and
  // otherwise ends each destructible field it holds. Returns whether
  // every destructor in that place was placed.
  bool emit_drop_at(ir::OperandIdx place, ir::TypeIdx type, diag::Span span);
  bool runs_destructor(ir::TypeIdx type) const;
  bool is_destructor(ast::ItemIdx item) const;
  void lower_fn(const FnEntry& entry);

  // Registers backing the current entry-block parameter list, consumed
  // by lower_fn while binding patterns.
  std::vector<ir::RegisterIdx> pending_params_;
  void run();

  ir::BlockParamIdxRange pending_block_params_;
  ir::Storage finish() &&;
};

}  // namespace lower
