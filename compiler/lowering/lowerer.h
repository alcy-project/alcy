// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/fmt.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "base/nesting.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/debug/profiler/profiler.h"
#include "fpag/str/string_interner.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/opcode.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/verifier.h"
#include "lowering/lowering.h"

namespace lowering {

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
  // Set once the shared table has no room for another name; the loop
  // that saw it stops and `lower_package` refuses the package.
  bool name_table_exhausted_ = false;
  // Where the trace events go, or nothing. Set where the lowerer is
  // constructed - `lower_package` - and read by the function loop here.
  debug::Profiler* profiler = nullptr;

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
    ir::TypeIdx type = ir::TypeIdx(base::INVALID_IDX);
  };

  // Lexical comp bindings: persistent per-function bindings plus
  // evaluation-local frames.
  struct CompScope {
    std::vector<std::pair<std::string_view, CompVal>>* outer = nullptr;
    std::vector<std::vector<std::pair<std::string_view, CompVal>>> frames;
  };

  struct FnEntry {
    ast::ItemIdx item = ast::ItemIdx::invalid();
    // Specialization key over comp argument values; empty for
    // functions without comp parameters.
    std::string comp_key;
    ir::FunctionIdx idx = ir::FunctionIdx(base::INVALID_IDX);
    u32 mod = 0;
    std::string name;
    std::vector<ir::TypeIdx> params;
    ir::TypeIdx ret = ir::TypeIdx(base::INVALID_IDX);
    // Generic instantiation lowered under (NO_INST for plain code).
    u32 inst = analyzer::NO_INST;
    // What the symbol names. A method and an associated function share
    // an item, so the kind comes from the signature, not the item.
    ir::SymbolKind kind = ir::SymbolKind::Free;
    // Type arguments of the instantiation, for the symbol.
    std::vector<ir::TypeIdx> generics;
    // Comp argument values in formal-parameter order.
    std::vector<CompVal> comp_args;
    // A closure body to compile instead of an item: the literal or
    // coercion expression that reserved this entry. Invalid for
    // declared functions.
    ast::ExprIdx closure = ast::ExprIdx::invalid();
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
  u32 cur_inst_ = analyzer::NO_INST;
  u32 comp_inst_ = analyzer::NO_INST;

  struct ExtEntry {
    std::string_view name;
    ir::ExternalFunctionIdx idx;
  };
  std::vector<ExtEntry> exts;

  // Per-function state.
  u32 module = 0;
  std::vector<Local> locals;
  // Bounds the recursive tree walk; see base::MAX_NESTING. The analyzer
  // rejects a tree this deep before lowering sees it, so tripping this
  // means the budget is the only thing between a hostile input and the
  // stack.
  base::NestingGuard nesting_{base::MAX_NESTING};
  bool reported_too_deep_ = false;
  // True once the budget is spent, so a lowered value can be reported
  // without a span at hand.
  bool nesting_exhausted() const { return nesting_.exhausted(); }
  bool report_nesting(diag::Span span);
  // Instruction streams by reserved block: reservation order matches
  // creation order, so reserved indexes line up with storage positions.
  std::vector<ir::InstrSeq> streams_;
  std::vector<ir::InstructionIdx> stream_last_;
  std::vector<ir::BlockIdx> fn_blocks_;
  ir::BlockIdx cur_{base::INVALID_IDX};
  u32 block_next_ = 0;
  u32 fn_block_base_ = 0;
  bool binding_param_ = false;
  // A loop's branch target and where its body's locals begin. A branch
  // out of the body skips the body's own drop point, so break and
  // continue end those locals from this mark. The innermost block's mark
  // is not enough: the branch usually sits in a nested block.
  struct LoopTarget {
    ir::BlockIdx block;
    u32 drops = 0;
  };
  std::vector<LoopTarget> break_targets_;
  std::vector<LoopTarget> continue_targets_;
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
          diag::DiagBag& bag,
          debug::Profiler* profiler = nullptr);
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
  const analyzer::CheckedModule::IndirectCall* indirect_call(
      ast::ExprIdx callee) const;
  const analyzer::CheckedModule::ClosureFn* closure_fn(ast::ExprIdx expr) const;
  const analyzer::CheckedModule::ClosureLit* closure_lit(
      ast::ExprIdx expr) const;
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
  std::vector<ir::TypeIdx> fn_args_for(ast::ItemIdx item, u32 inst) const;
  u32 generic_inst_index(ir::TypeIdx type) const;
  const analyzer::CheckedModule::StructInfo* struct_info(ir::TypeIdx type);
  // Follows a field storage copy back to the type it was copied from.
  ir::TypeIdx type_origin(ir::TypeIdx type) const;
  u64 parse_numeric_value(std::string_view spelling);
  ir::TypeTag literal_tag(ast::LiteralIdx value, const ir::TypeIdx* expected);
  // Interns a name, or reports the shared table as spent once and marks
  // the run failed. A caller that gets an invalid id stops.
  str::StringPoolId intern_name(std::string_view name);
  Val lower_literal(ast::LiteralIdx lit_idx, const ir::TypeIdx* expected);
  ir::OperandIdx imm_from_u64(ir::TypeTag tag, ir::TypeIdx type, u64 value);
  Val place_addr(ast::ExprIdx expr);
  bool is_rooted_place(ast::ExprIdx expr) const;
  Val checked_index_addr(Val base, Val position, diag::Span span);
  // Whether a checked type is a `Range<T>` value. The interval types
  // are reserved to core, so the shape is the declaration's.
  bool is_range_type(ir::TypeIdx type);
  // The half-open `[start, width)` a range value names over a run of
  // `len` elements. Both endpoints and their order are checked at
  // runtime; `start` and `width` are `usize` operands.
  struct RunBounds {
    ir::OperandIdx start = ir::OperandIdx::invalid();
    ir::OperandIdx width = ir::OperandIdx::invalid();
  };
  bool run_bounds(Val range,
                  ir::OperandIdx len,
                  RunBounds& out,
                  diag::Span span);
  // Writes one endpoint of a range expression into `range_slot`:
  // `Included` for a start and for an end after `..=`, `Excluded` for
  // the other ends, and `Unbounded` for an absent side.
  bool store_bound(ast::ExprIdx expr,
                   Val range_slot,
                   std::string_view field,
                   ast::ExprIdx endpoint,
                   bool included);
  Val lower_range(ast::ExprIdx expr);
  // A run of `base` as a view: a fixed array is borrowed for it, a
  // slice re-slices to its own kind, and a `str` to `str`. `exclusive`
  // selects `&mut [T]` when the receiver is an array.
  Val lower_subslice(Val base, Val range, bool exclusive, diag::Span span);
  // A `usize` immediate, sized for the target.
  ir::OperandIdx const_usize(u64 value);
  ir::OperandIdx index_operand(u32 index);
  bool struct_field_index(ir::TypeIdx type,
                          std::string_view name,
                          u32& index_out);
  ir::TypeIdx field_type_of(ir::TypeIdx base, u32 index, diag::Span span);
  void bind_pattern(ast::PatternIdx pattern, Val init);
  Val lower_path(ast::ExprIdx expr, const ir::TypeIdx* expected);
  // A closure literal or coercion as a value: the code with its
  // environment, packed as the function type lays out.
  Val lower_closure(ast::ExprIdx expr);
  // Reserves (or finds) the function a closure body compiles to.
  // The environment arrives first, then the closure's parameters;
  // plain functions coerce through a wrapper that drops it. `env`
  // is the environment tuple's reference type, invalid when the
  // closure captures nothing.
  ir::FunctionIdx closure_fn_index(u32 mod,
                                   ast::ExprIdx key,
                                   const std::vector<ir::TypeIdx>& params,
                                   ir::TypeIdx ret,
                                   u32 inst,
                                   ir::TypeIdx env);
  // The environment type of a capture list: a structural tuple with
  // one field per capture, a reference or the copied value itself.
  // The creation site and the synthetic function build it the same
  // way, so structural interning gives them one type.
  ir::TypeIdx closure_env_type(
      const std::vector<analyzer::CheckedModule::ClosureLit::Capture>&
          captures);
  // A call through a function value: the callee operand carries the
  // value, and the signature comes from its function type.
  Val lower_indirect_call(ast::ExprIdx expr);
  void lower_closure_fn(const FnEntry& entry);
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
  ir::TypeIdx enum_slot_type(ir::TypeIdx enum_type);
  ir::TypeIdx payload_carrier(u64 align);

  // Enum slot types by enum type index: the payload half is inline,
  // so the slot shape varies per enum.
  std::unordered_map<u32, ir::TypeIdx> enum_slot_types_;
  Val lower_variant_construct(ast::ExprIdx expr,
                              const analyzer::CheckedModule::VariantUse* use);
  ir::OperandIdx disc_operand(u32 discriminant);
  Val lower_call(ast::ExprIdx expr);
  Val lower_associated_call(ast::ExprIdx expr);
  Val lower_intrinsic_call(ast::ExprIdx expr,
                           const analyzer::CheckedModule::FnSig& sig,
                           const std::vector<ir::TypeIdx>& type_args);
  // An `extern "C"` call: the declaration names the symbol, arguments
  // pass by value, and the gate was checked at the call site.
  Val lower_extern_call(ast::ExprIdx expr,
                        const analyzer::CheckedModule::FnSig& sig);
  ir::TypeIdx usize_type();
  bool str_parts(Val str, ir::OperandIdx& bytes_out, ir::OperandIdx& len_out);
  bool slice_parts(Val slice,
                   ir::OperandIdx& bytes_out,
                   ir::OperandIdx& len_out);
  Val build_slice_value(ir::OperandIdx base,
                        ir::OperandIdx len,
                        ir::TypeIdx ref_ty,
                        diag::Span span);
  bool is_slice_ref(ir::TypeIdx param) const;
  ir::TypeIdx slice_pointee(ir::TypeIdx type) const;
  ir::OperandIdx advance_ptr(ir::OperandIdx ptr,
                             ir::OperandIdx offset,
                             diag::Span span);
  // Allocates `count` elements of `elem` on the heap and labels the raw
  // pointer with `ref_ty`, the `&mut` the caller owns. This is what
  // `alloc<T>` lowers to, factored out for callers that need a heap
  // buffer without going through the intrinsic.
  ir::RegisterIdx emit_heap_alloc(ir::TypeIdx elem,
                                  ir::TypeIdx ref_ty,
                                  ir::OperandIdx count);
  Val lower_str_intrinsic(ast::ExprIdx expr, std::string_view name);
  Val lower_intrinsic(ast::ExprIdx expr, std::string_view name);
  ir::OperandIdx arg_for(Val arg, ir::TypeIdx param);
  Val load_disc(Val slot_addr);
  bool is_unit_payload(const std::vector<ir::TypeIdx>& payloads);
  Val void_value();
  ir::TypeIdx payload_tuple(const std::vector<ir::TypeIdx>& fields);
  Val load_payload_field(Val slot_addr, ir::TypeIdxRange fields, u32 field);
  ir::RegisterIdx payload_field_addr(Val slot_addr,
                                     ir::TypeIdxRange fields,
                                     u32 field);
  ir::TypeIdxRange variant_fields(ir::TypeIdx enum_type, u32 variant);
  void emit_br(ir::BlockIdx target);
  void emit_cond_br(ir::OperandIdx cond,
                    ir::BlockIdx then_block,
                    ir::BlockIdx else_block);
  ir::OperandIdx bool_operand(bool value);
  void emit_panic(ir::OperandIdx message);
  ir::OperandIdx str_operand(std::string_view message);
  Val lower_method_call(ast::ExprIdx expr);
  // `a[i]` where the checker resolved an `Index`/`IndexMut` impl: the
  // recorded call becomes a method call whose result reference is the
  // place of the element. `as_place` keeps that place; otherwise the
  // element is loaded.
  Val lower_spec_index(ast::ExprIdx expr,
                       const analyzer::CheckedModule::CallTarget* target,
                       bool as_place);
  // `a == b` where the checker resolved a `PartialEq` impl: the
  // recorded call takes both operands by shared reference, and `!=`
  // negates its result.
  Val lower_spec_equality(ast::ExprIdx expr,
                          const analyzer::CheckedModule::CallTarget* target);
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
  // Alternatives to test for a condition pattern; a plain pattern is
  // its own only alternative.
  std::vector<ast::PatternIdx> condition_alternatives(ast::PatternIdx pattern);
  // Tests a condition pattern, taking the body on any match and the
  // failure path only when every alternative misses.
  void lower_condition_test(ast::PatternIdx pattern,
                            Val scrut_addr,
                            ir::TypeIdx scrut_type,
                            ir::BlockIdx body_block,
                            ir::BlockIdx fail_block);
  Val lower_match(ast::ExprIdx expr, const ir::TypeIdx* expected);
  Val lower_if(ast::ExprIdx expr, const ir::TypeIdx* expected);
  Val lower_loop(ast::ExprIdx expr);
  Val lower_while(ast::ExprIdx expr);
  Val lower_question(ast::ExprIdx expr);

  // Compile-time evaluation

  static constexpr usize COMP_STEP_BUDGET = 1u << 20;
  static constexpr u32 COMP_MAX_CALL_DEPTH = 64;
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
  // One const item's initializer, evaluated at a use site: item scope,
  // so no comp binding of the caller is visible, with the evaluation
  // budget reset for the run.
  bool comp_evaluate_item(u32 mod, ast::ExprIdx init, CompVal& out);
  // The same, from inside an evaluation already running: the budget is
  // shared and the recursion depth counts against the call limit.
  bool comp_eval_const_item(u32 mod, ast::ExprIdx init, CompVal& out);
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
                       ir::OperandIdx capacity,
                       FmtState& state,
                       bool measure_only);
  Val lower_fmt_write(ast::ExprIdx expr,
                      const analyzer::CheckedModule::FnSig& sig);
  Val lower_fmt_format(ast::ExprIdx expr,
                       const analyzer::CheckedModule::FnSig& sig);
  Val lower_expr(ast::ExprIdx expr, const ir::TypeIdx* expected);
  Val lower_literal_zero(ir::TypeIdx type, diag::Span span);
  void lower_stmt(ast::StmtIdx stmt);
  Val lower_block(ast::BlockIdx block, const ir::TypeIdx* expected);
  // One arm of a branch (if/else, pattern test, loop body). The block is
  // one path through the function, so a value it moved is put back on
  // the way out: the path that did not take this branch still owns it,
  // and the borrow checker's join reports what any path moved. A
  // sequenced block keeps its moves; nothing else runs that path.
  Val lower_branch(ast::BlockIdx block, const ir::TypeIdx* expected);
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
  // Which items are a destructor, gathered once from the checked package.
  // Lowering asks this of every function it lowers, and answering by walking
  // every method in the package charged each function for the whole package.
  std::vector<bool> drop_items_;
  void mark_drops();
  // Where each type copy came from, and where each struct's shape is
  // recorded, both answered by table.
  std::unordered_map<u32, u32> type_origins_;
  std::unordered_map<u32, const analyzer::CheckedModule::StructInfo*> structs_;
  // An enumeration's shape and where a path's variant was recorded, by what a
  // caller knows when it asks. Each was a walk over every module in the
  // package, so one question cost the package.
  std::unordered_map<u32, const analyzer::CheckedModule::EnumInfo*> enums_;
  std::unordered_map<u64, const analyzer::CheckedModule::VariantUse*> variants_;
  std::unordered_map<u32, u32> generic_insts_;
  // Where a specialization key already has an entry. Lowering asks this of
  // every call site, and walking the entries so far to compare keys charged
  // one call site for every function lowered before it.
  std::unordered_map<std::string, u32> fn_by_key_;
  void index_origins();
  void index_structs();
  void lower_fn(const FnEntry& entry);

  // Registers backing the current entry-block parameter list, consumed
  // by lower_fn while binding patterns.
  std::vector<ir::RegisterIdx> pending_params_;
  void run();

  ir::BlockParamIdxRange pending_block_params_;
  base::Result<ir::VerifiedStorage, ir::VerificationError> finish() &&;
};

}  // namespace lowering
