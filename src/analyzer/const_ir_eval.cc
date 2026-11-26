// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "analyzer/const_ir_eval.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "analyzer/comp_eval.h"
#include "analyzer/fmt.h"
#include "ast/ast.h"
#include "comp/comp_value.h"
#include "comp/ir_interpreter.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/limits.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/opcode.h"
#include "ir/seq_builder.h"
#include "ir/storage_builder.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "ir/type_util.h"
#include "text/unescape.h"

namespace analyzer {

EmitContext::EmitContext(CompEval& evaluator_in,
                         comp::CompScope& scope_in,
                         EvalSite site_in)
    : evaluator(evaluator_in), scope(scope_in), site(site_in) {}

EmitContext EmitContext::with_site(EvalSite next) const {
  return EmitContext(evaluator, scope, next);
}

ir::TypeIdx EmitContext::ExprType::operator()(ast::ExprIdx expr) const {
  return ctx.evaluator.type_of(ctx.site, expr);
}

ir::TypeIdx EmitContext::LayoutType::operator()(ast::ExprIdx expr) const {
  return ctx.evaluator.layout_type_of(ctx.site, expr);
}

bool EmitContext::PathValue::operator()(ast::PathIdx path,
                                        comp::CompVal& out) const {
  return ctx.evaluator.path_value(ctx.site, ctx.scope, path, out);
}

bool EmitContext::CompFn::operator()(ast::ExprIdx call,
                                     CompFnInfo& info) const {
  return ctx.evaluator.comp_fn(ctx.site, call, info);
}

bool EmitContext::FieldIndex::operator()(ir::TypeIdx type,
                                         std::string_view name,
                                         u32& index) const {
  return ctx.evaluator.field_index(type, name, index);
}

bool EmitContext::VariantIndex::operator()(ir::TypeIdx type,
                                           std::string_view name,
                                           u32& index) const {
  return ctx.evaluator.variant_index(type, name, index);
}
namespace {

using ExprTypeFn = EmitContext::ExprType;
using ParamEnv = std::vector<std::pair<std::string_view, ir::OperandIdx>>;

constexpr usize COMP_MAX_UNROLL = 4096;
struct LocalBinding {
  ir::OperandIdx addr = ir::OperandIdx::invalid();
  ir::TypeIdx type = ir::TypeIdx::invalid();
  std::optional<comp::CompVal> value;
};
using LocalEnv = std::vector<std::pair<std::string_view, LocalBinding>>;

// A synthetic function under construction. Each block owns its
// instruction sequence; control flow switches the active block and
// joins values through block parameters. References stay stable across
// new blocks because the sequences are heap-owned.
class ConstEmitter {
 public:
  explicit ConstEmitter(ir::StorageBuilder& builder, u64& quota)
      : builder_(builder),
        quota_(quota),
        start_(builder.state().blocks.size()) {}

  ir::StorageBuilder& builder() { return builder_; }

  bool spend_quota() {
    if (quota_ == 0) {
      set_emit_error(EmitError::Budget);
      return false;
    }
    --quota_;
    return true;
  }
  ir::BlockIdx start() const { return ir::BlockIdx(start_); }

  ir::InstrSeq& cur() { return *blocks_[current_]; }

  ir::BlockIdx new_block() {
    const ir::BlockIdx idx = builder_.block({.instrs = {}, .block_params = {}});
    blocks_.push_back(std::make_unique<ir::InstrSeq>());
    return idx;
  }

  void set_cur(ir::BlockIdx idx) {
    current_ = idx.idx - start_;
    terminated_ = false;
  }
  void set_terminated() { terminated_ = true; }
  bool terminated() const { return terminated_; }
  void set_broke() { broke_ = true; }
  bool broke() const { return broke_; }
  void clear_broke() { broke_ = false; }
  void reset_terminated() { terminated_ = false; }

  void push_return_target(ir::BlockIdx block, ir::RegisterIdx reg) {
    return_targets_.emplace_back(block, reg);
  }
  void pop_return_target() { return_targets_.pop_back(); }
  bool return_enabled() const { return !return_targets_.empty(); }
  ir::BlockIdx return_target() const {
    return return_targets_.empty() ? ir::BlockIdx(base::INVALID_IDX)
                                   : return_targets_.back().first;
  }
  ir::RegisterIdx return_reg() const {
    return return_targets_.empty() ? ir::RegisterIdx(base::INVALID_IDX)
                                   : return_targets_.back().second;
  }

  void set_loop_targets(ir::BlockIdx brk, ir::BlockIdx cont) {
    break_target_ = brk;
    continue_target_ = cont;
    ++loop_depth_;
  }
  void clear_loop_targets() {
    break_target_ = ir::BlockIdx(base::INVALID_IDX);
    continue_target_ = ir::BlockIdx(base::INVALID_IDX);
    if (loop_depth_ != 0) {
      --loop_depth_;
    }
  }
  bool in_loop() const { return loop_depth_ != 0; }
  ir::BlockIdx break_target() const { return break_target_; }
  ir::BlockIdx continue_target() const { return continue_target_; }

  ir::OperandIdx void_operand() {
    const ir::TypeIdx i1 = builder_.primitive(ir::TypeTag::I1);
    const ir::ImmutableIdx imm =
        builder_.immutable({.type = i1, .data = {.i1_value = false}});
    return builder_.operand(ir::Operand::from_immutable(imm, i1));
  }

  ir::InstructionIdx emit_alloca(ir::TypeIdx type, ir::RegisterIdx dst) {
    const ir::TypeIdx i32_ty = builder_.primitive(ir::TypeTag::I32);
    const ir::OperandIdx count = builder_.operand(ir::Operand::from_immutable(
        builder_.immutable({.type = i32_ty, .data = {.i32_value = 1}}),
        i32_ty));
    return builder_.instr({.op = ir::Opcode::Alloca,
                           .flags = {},
                           .dst = dst,
                           .measure = type,
                           .operands = ir::OperandIdxRange{count, 1}});
  }

  ir::RegisterIdx claim_reg(ir::TypeIdx type) {
    const ir::RegisterIdx reg(builder_.state().registers.size());
    builder_.reg(
        {.type = type, .def_idx = ir::InstructionIdx(base::INVALID_IDX)});
    return reg;
  }

  ir::BlockParamIdx block_param(ir::TypeIdx type, ir::RegisterIdx reg) {
    return builder_.block_param({.type = type, .reg = reg});
  }

  void set_block_params(ir::BlockIdx block, ir::BlockParamIdx idx) {
    builder_.set_block_params(block, ir::BlockParamIdxRange{idx, 1});
  }

  // Emits a compile-time value as a real scalar operand when it is
  // Int/Bool (so arithmetic and comparisons can consume it), otherwise
  // as a semantic slot in the side table.
  ir::OperandIdx add_value(const comp::CompVal& value, ir::TypeIdx type) {
    if (value.value.tag == comp::CompValue::Tag::Int ||
        value.value.tag == comp::CompValue::Tag::Bool) {
      const ir::TypeTag tag = builder_.state().types[type].tag;
      const u64 bits = value.value.tag == comp::CompValue::Tag::Bool
                           ? (value.value.bool_value ? 1u : 0u)
                           : value.value.int_value;
      const ir::ImmutableIdx imm = builder_.immutable(
          {.type = type, .data = {.u64_value = bits & comp::mask(tag)}});
      const ir::OperandIdx result =
          builder_.operand(ir::Operand::from_immutable(imm, type));
      // Keep the semantic value reachable from the scalar operand too,
      // so an enclosing aggregate can read it with get_semantic while
      // arithmetic still sees a scalar.
      const u32 index = static_cast<u32>(semantic_values_.size());
      semantic_values_.push_back(value);
      semantic_by_operand_[result.idx] = index;
      return result;
    }
    return add_semantic(value, type);
  }

  std::span<const comp::CompVal> semantic_values() const {
    return semantic_values_;
  }

  const comp::CompVal* get_semantic(ir::OperandIdx operand) const {
    const auto found = semantic_by_operand_.find(operand.idx);
    if (found != semantic_by_operand_.end()) {
      return &semantic_values_[found->second];
    }
    // A value tagged on a register is reached through whichever operand
    // names that register; creating an operand gives it a fresh index.
    const ir::Operand& op = builder_.state().operands[operand.idx];
    if (op.is<ir::RegisterIdx>()) {
      const auto reg_found = semantic_by_register_.find(op.as_register().idx);
      if (reg_found != semantic_by_register_.end()) {
        return &semantic_values_[reg_found->second];
      }
    }
    return nullptr;
  }

  // The synthetic function lives only until it has been interpreted. A
  // range per emitted expression is enough to map an IR failure back to
  // the source that produced the instruction, without carrying span state
  // through the builder's published tables.
  struct SpanRange {
    u32 first = 0;
    u32 last = 0;
    diag::Span span;
  };
  std::vector<SpanRange> span_ranges_;
  std::vector<diag::Span> span_stack_;

  // Records the instruction window one expression occupies. The window
  // closes on scope exit so a lookup can tell the instruction emitted by
  // the expression from the ones its children emitted.
  class SpanScope {
   public:
    SpanScope(ConstEmitter& emitter, diag::Span span)
        : emitter_(emitter), index_(emitter.span_ranges_.size()) {
      const u32 at = static_cast<u32>(emitter.builder().state().instrs.size());
      emitter.span_ranges_.push_back({at, at, span});
      emitter.span_stack_.push_back(span);
    }
    ~SpanScope() {
      emitter_.span_stack_.pop_back();
      emitter_.span_ranges_[index_].last =
          static_cast<u32>(emitter_.builder().state().instrs.size());
    }

   private:
    ConstEmitter& emitter_;
    usize index_;
  };

  std::optional<diag::Span> span_of(ir::InstructionIdx instr) const {
    if (!instr.is_valid()) {
      return std::nullopt;
    }
    std::optional<diag::Span> found;
    for (const SpanRange& range : span_ranges_) {
      if (range.first <= instr.idx && instr.idx < range.last) {
        found = range.span;
      }
    }
    return found;
  }

  ir::OperandIdx add_semantic(const comp::CompVal& value, ir::TypeIdx type) {
    const u32 index = static_cast<u32>(semantic_values_.size());
    semantic_values_.push_back(value);
    const ir::TypeIdx i32_ty = builder_.primitive(ir::TypeTag::I32);
    const ir::OperandIdx index_op =
        builder_.operand(ir::Operand::from_immutable(
            builder_.immutable(
                {.type = i32_ty,
                 .data = {.i32_value = static_cast<i32>(index)}}),
            i32_ty));
    const ir::RegisterIdx dst(builder_.state().registers.size());
    const ir::InstructionIdx instr =
        builder_.instr({.op = ir::Opcode::CompValue,
                        .flags = {},
                        .dst = dst,
                        .measure = ir::TypeIdx::invalid(),
                        .operands = ir::OperandIdxRange{index_op, 1}});
    builder_.reg({.type = type, .def_idx = instr});
    cur().push(instr);
    const ir::OperandIdx result =
        builder_.operand(ir::Operand::from_register(dst, type));
    semantic_by_operand_[result.idx] = index;
    return result;
  }

  // Records that a register carries a compile-time value, so a caller
  // reading that register (e.g. a comp fn's return block parameter)
  // still sees the semantic value instead of only lowered bits.
  void tag_register(ir::RegisterIdx reg, const comp::CompVal& value) {
    const u32 index = static_cast<u32>(semantic_values_.size());
    semantic_values_.push_back(value);
    semantic_by_register_.emplace(reg.idx, index);
  }

  void set_context(const EmitContext& ctx) { context_ = &ctx; }
  bool resolve_variant_index(ir::TypeIdx type,
                             std::string_view name,
                             u32& idx) const {
    return context_->resolve_variant_index(type, name, idx);
  }

  static constexpr u32 MAX_INLINE_DEPTH = comp::COMP_MAX_CALL_DEPTH;

  bool enter_inline() {
    if (inline_depth_ >= MAX_INLINE_DEPTH) {
      return false;
    }
    ++inline_depth_;
    return true;
  }
  void leave_inline() { --inline_depth_; }

  // A value produced by a runtime semantic opcode has no compile-time
  // representation; drop any tag attached to its operand or register.
  void clear_semantic(ir::OperandIdx operand) {
    semantic_by_operand_.erase(operand.idx);
    const ir::Operand& op = builder_.state().operands[operand.idx];
    if (op.is<ir::RegisterIdx>()) {
      semantic_by_register_.erase(op.as_register().idx);
    }
  }

  usize value_bindings_size() const { return value_bindings_.size(); }
  void restore_value_bindings(usize size) {
    value_bindings_.erase(value_bindings_.begin() + size,
                          value_bindings_.end());
  }
  void bind_value(std::string_view name, ir::OperandIdx operand) {
    value_bindings_.emplace_back(name, operand);
  }
  void rebind_value(std::string_view name, ir::OperandIdx operand) {
    for (usize i = value_bindings_.size(); i > 0; --i) {
      if (value_bindings_[i - 1].first == name) {
        value_bindings_[i - 1].second = operand;
        return;
      }
    }
  }
  ir::OperandIdx lookup_value(std::string_view name) const {
    for (usize i = value_bindings_.size(); i > 0; --i) {
      if (value_bindings_[i - 1].first == name) {
        return value_bindings_[i - 1].second;
      }
    }
    return ir::OperandIdx(base::INVALID_IDX);
  }

  usize locals_size() const { return locals_.size(); }
  void restore_locals(usize size) { locals_.resize(size); }
  void bind_local(std::string_view name,
                  ir::OperandIdx addr,
                  ir::TypeIdx type,
                  std::optional<comp::CompVal> value = std::nullopt) {
    locals_.emplace_back(name, LocalBinding{addr, type, std::move(value)});
  }
  void set_local_value(std::string_view name, const comp::CompVal& value) {
    for (usize i = locals_.size(); i > 0; --i) {
      if (locals_[i - 1].first == name) {
        locals_[i - 1].second.value = value;
        return;
      }
    }
  }
  const LocalBinding* lookup_local(std::string_view name) const {
    for (usize i = locals_.size(); i > 0; --i) {
      if (locals_[i - 1].first == name) {
        return &locals_[i - 1].second;
      }
    }
    return nullptr;
  }

  void emit_br(ir::BlockIdx target, ir::OperandIdx value) {
    ir::OperandSeq ops;
    ops.push(builder_.operand(ir::Operand::from_block(
        target, builder_.primitive(ir::TypeTag::Void))));
    if (value.is_valid()) {
      ops.push(
          builder_.operand(ir::Operand{builder_.state().operands[value.idx]}));
    }
    cur().push(builder_.instr({.op = ir::Opcode::Br,
                               .flags = {},
                               .dst = ir::RegisterIdx(base::INVALID_IDX),
                               .measure = ir::TypeIdx::invalid(),
                               .operands = ops.finish()}));
    terminated_ = true;
  }

  void emit_cond_br(ir::OperandIdx cond,
                    ir::BlockIdx then_block,
                    ir::BlockIdx else_block) {
    const ir::TypeIdx void_ty = builder_.primitive(ir::TypeTag::Void);
    ir::OperandSeq ops;
    ops.push(builder_.operand(
        ir::Operand{ir::Operand{builder_.state().operands[cond.idx]}}));
    ops.push(builder_.operand(ir::Operand::from_block(then_block, void_ty)));
    ops.push(builder_.operand(ir::Operand::from_block(else_block, void_ty)));
    cur().push(builder_.instr({.op = ir::Opcode::CondBr,
                               .flags = {},
                               .dst = ir::RegisterIdx(base::INVALID_IDX),
                               .measure = ir::TypeIdx::invalid(),
                               .operands = ops.finish()}));
    terminated_ = true;
  }

  enum class EmitError : u8 {
    None,
    Budget,
    OutOfBounds,
    ExpansionCap,
    Overflow,
    UnresolvedPath,
    SliceBoundary
  };
  EmitError emit_error = EmitError::None;
  diag::Span error_span_{};
  bool has_error_span_ = false;
  void set_emit_error(EmitError e) {
    if (emit_error == EmitError::None) {
      emit_error = e;
      if (!span_stack_.empty()) {
        error_span_ = span_stack_.back();
        has_error_span_ = true;
      }
    }
  }

  void finish() {
    for (u32 i = 0; i < blocks_.size(); ++i) {
      builder_.set_block_instrs(ir::BlockIdx(start_ + i), blocks_[i]->finish());
    }
  }

 private:
  ir::StorageBuilder& builder_;
  u64& quota_;
  std::vector<std::unique_ptr<ir::InstrSeq>> blocks_;
  LocalEnv locals_;
  std::vector<comp::CompVal> semantic_values_;
  std::unordered_map<u32, u32> semantic_by_operand_;
  std::unordered_map<u32, u32> semantic_by_register_;
  const EmitContext* context_ = nullptr;
  std::vector<std::pair<std::string_view, ir::OperandIdx>> value_bindings_;
  u32 inline_depth_ = 0;
  std::vector<std::pair<ir::BlockIdx, ir::RegisterIdx>> return_targets_;

  u32 start_;
  u32 current_ = 0;
  bool terminated_ = false;
  bool broke_ = false;
  u32 loop_depth_ = 0;
  ir::BlockIdx break_target_ = ir::BlockIdx(base::INVALID_IDX);
  ir::BlockIdx continue_target_ = ir::BlockIdx(base::INVALID_IDX);
};

ir::OperandIdx emit_expr(const ast::AstArena& ast,
                         ast::ExprIdx expr,
                         ir::TypeIdx type,
                         ConstEmitter& emitter,
                         const EmitContext& ctx,
                         const ParamEnv* params);

ir::OperandIdx emit_comp_for(const ast::AstArena& ast,
                             const ast::Block& block,
                             ConstEmitter& emitter,
                             const EmitContext& ctx,
                             const ParamEnv* params);

static bool operand_int(ConstEmitter& emitter, ir::OperandIdx op, u64& out);

// Resolves a reassign place naming a semantic binding to the binding's
// name and the chain of struct/tuple child indices for the written slot.
static bool semantic_place_path(
    const ast::AstArena& ast,
    ast::ExprIdx place,
    std::string_view& root_name,
    std::vector<u32>& indices,
    const ExprTypeFn& expr_type,
    ConstEmitter& emitter,
    const EmitContext::FieldIndex& resolve_field_index) {
  const ast::ExprNode& node = ast.exprs[place];
  if (node.kind == ast::ExprKind::Path) {
    const auto& segs =
        ast.paths[node.payload.get<ast::ExprPath>().idx].segments;
    if (segs.size() != 1) {
      return false;
    }
    root_name = segs[0].name;
    return true;
  }
  if (node.kind == ast::ExprKind::Field) {
    const ast::ExprField& field = node.payload.get<ast::ExprField>();
    if (!semantic_place_path(ast, field.receiver, root_name, indices, expr_type,
                             emitter, resolve_field_index)) {
      return false;
    }
    ir::TypeIdx recv_ty = expr_type(field.receiver);
    const ir::TypeTag recv_tag = emitter.builder().state().types[recv_ty].tag;
    if (recv_tag == ir::TypeTag::Ref || recv_tag == ir::TypeTag::MutRef) {
      recv_ty =
          emitter.builder()
              .state()
              .ref_types[emitter.builder().state().types[recv_ty].as_ref()]
              .pointee;
    }
    u32 index = 0;
    if (!resolve_field_index(recv_ty, field.name.name, index)) {
      std::string_view numeric = field.name.name;
      u32 parsed = 0;
      bool all_digits = !numeric.empty();
      for (char c : numeric) {
        if (c < '0' || c > '9') {
          all_digits = false;
          break;
        }
        parsed = parsed * 10 + static_cast<u32>(c - '0');
      }
      if (!all_digits) {
        return false;
      }
      index = parsed;
    }
    indices.push_back(index);
    return true;
  }
  if (node.kind == ast::ExprKind::Index) {
    const ast::ExprIndex& idx = node.payload.get<ast::ExprIndex>();
    if (!semantic_place_path(ast, idx.receiver, root_name, indices, expr_type,
                             emitter, resolve_field_index)) {
      return false;
    }
    const ast::ExprNode& index_node = ast.exprs[idx.index];
    if (index_node.kind != ast::ExprKind::Literal) {
      return false;
    }
    const ast::Literal& lit =
        ast.literals[index_node.payload.get<ast::ExprLiteral>().value];
    if (lit.kind != ast::LiteralKind::Integer) {
      return false;
    }
    indices.push_back(
        static_cast<u32>(comp::parse_numeric_value(lit.spelling)));
    return true;
  }
  return false;
}

// Walks a semantic value to the slot addressed by `indices` and returns
// a reference to it.
static comp::CompValue* semantic_slot(comp::CompValue& root,
                                      const std::vector<u32>& indices) {
  comp::CompValue* cur = &root;
  for (u32 index : indices) {
    if (index >= cur->fields.size()) {
      return nullptr;
    }
    cur = &cur->fields[index];
  }
  return cur;
}

// The address of an lvalue the emitter can read and write at runtime.
// Scalars already use allocas; aggregates now do too, so a place chain
// resolves to a GEP from the binding's address.
static ir::OperandIdx emit_place_address(const ast::AstArena& ast,
                                         ast::ExprIdx place,
                                         ConstEmitter& emitter,
                                         const EmitContext& ctx,
                                         const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[place];
  if (node.kind == ast::ExprKind::Path) {
    const auto& segs =
        ast.paths[node.payload.get<ast::ExprPath>().idx].segments;
    if (segs.size() != 1) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    if (const LocalBinding* local = emitter.lookup_local(segs[0].name)) {
      return local->addr;
    }
    if (params != nullptr) {
      for (const auto& [name, operand] : *params) {
        if (name == segs[0].name) {
          return operand;
        }
      }
    }
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (node.kind != ast::ExprKind::Field && node.kind != ast::ExprKind::Index) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::OperandIdx base =
      emit_place_address(ast,
                         node.kind == ast::ExprKind::Field
                             ? node.payload.get<ast::ExprField>().receiver
                             : node.payload.get<ast::ExprIndex>().receiver,
                         emitter, ctx, params);
  if (!base.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  ir::TypeIdx base_type = emitter.builder().state().operands[base.idx].type;
  if (emitter.builder().state().types[base_type].tag == ir::TypeTag::Ref ||
      emitter.builder().state().types[base_type].tag == ir::TypeTag::MutRef) {
    base_type =
        emitter.builder()
            .state()
            .ref_types[emitter.builder().state().types[base_type].as_ref()]
            .pointee;
  }
  u32 index = 0;
  ir::TypeIdx field_type = ir::TypeIdx(base::INVALID_IDX);
  if (node.kind == ast::ExprKind::Field) {
    const ast::ExprField& field = node.payload.get<ast::ExprField>();
    if (emitter.builder().state().types[base_type].tag == ir::TypeTag::Tuple) {
      bool digits = !field.name.name.empty();
      for (char c : field.name.name) {
        if (c < '0' || c > '9') {
          digits = false;
          break;
        }
        index = index * 10 + static_cast<u32>(c - '0');
      }
      if (!digits) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
    } else if (!ctx.resolve_field_index(base_type, field.name.name, index)) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const ir::TypeTag tag = emitter.builder().state().types[base_type].tag;
    if (tag == ir::TypeTag::Struct) {
      const ir::StructType& shape =
          emitter.builder().state().struct_types
              [emitter.builder().state().types[base_type].as_struct()];
      if (index < shape.fields.size()) {
        field_type = shape.fields[index];
      }
    } else if (tag == ir::TypeTag::Tuple) {
      const ir::TupleType& shape =
          emitter.builder().state().tuple_types
              [emitter.builder().state().types[base_type].as_tuple()];
      if (index < shape.elements.size()) {
        field_type = shape.elements[index];
      }
    }
  } else {
    const ast::ExprIndex& access = node.payload.get<ast::ExprIndex>();
    const ir::OperandIdx position = emit_expr(
        ast, access.index, ctx.expr_type(access.index), emitter, ctx, params);
    u64 value = 0;
    if (!position.is_valid() || !operand_int(emitter, position, value)) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    index = static_cast<u32>(value);
    if (emitter.builder().state().types[base_type].tag == ir::TypeTag::Array) {
      field_type =
          emitter.builder()
              .state()
              .array_types
                  [emitter.builder().state().types[base_type].as_array()]
              .element;
    }
  }
  if (!field_type.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::TypeIdx i32_ty = emitter.builder().primitive(ir::TypeTag::I32);
  ir::OperandSeq ops;
  ops.push(emitter.builder().operand(
      ir::Operand{emitter.builder().state().operands[base.idx]}));
  ops.push(emitter.builder().operand(ir::Operand::from_immutable(
      emitter.builder().immutable({.type = i32_ty, .data = {.i32_value = 0}}),
      i32_ty)));
  ops.push(emitter.builder().operand(ir::Operand::from_immutable(
      emitter.builder().immutable(
          {.type = i32_ty, .data = {.i32_value = static_cast<i32>(index)}}),
      i32_ty)));
  const ir::RegisterIdx dst(emitter.builder().state().registers.size());
  const ir::InstructionIdx instr =
      emitter.builder().instr({.op = ir::Opcode::GetElementPtr,
                               .flags = {},
                               .dst = dst,
                               .measure = field_type,
                               .operands = ops.finish()});
  const ir::TypeIdx ref = emitter.builder().reference_type(field_type, true);
  emitter.builder().reg({.type = ref, .def_idx = instr});
  emitter.cur().push(instr);
  return emitter.builder().operand(ir::Operand::from_register(dst, ref));
}

// Emits a block's statements and trailing value into `emitter`.
ir::OperandIdx emit_block(const ast::AstArena& ast,
                          const ast::Block& block,
                          ir::TypeIdx type,
                          ConstEmitter& emitter,
                          const EmitContext& ctx,
                          const ParamEnv* params) {
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  if (block.statements.size() == 1 && block.value.is_valid()) {
    const ast::StmtNode& stmt = ast.stmts[block.statements[0]];
    if (stmt.kind == ast::StmtKind::Decl &&
        stmt.payload.get<ast::StmtDecl>().for_cursor &&
        ast.exprs[block.value].kind == ast::ExprKind::Loop) {
      return emit_comp_for(ast, block, emitter, ctx, params);
    }
  }
  const bool has_value = block.value.is_valid();
  const usize saved_locals = emitter.locals_size();
  for (const ast::StmtIdx stmt_idx : block.statements) {
    const ast::StmtNode& stmt = ast.stmts[stmt_idx];
    ConstEmitter::SpanScope stmt_scope(emitter, stmt.span);
    if (stmt.kind == ast::StmtKind::Decl) {
      const ast::StmtDecl& decl = stmt.payload.get<ast::StmtDecl>();
      if (!decl.init.is_valid() || decl.for_cursor) {
        emitter.restore_locals(saved_locals);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      const ast::PatternNode& pat = ast.patterns[decl.pattern];
      const bool discard = pat.kind == ast::PatternKind::Wildcard;
      if (!discard && pat.kind != ast::PatternKind::Ident &&
          pat.kind != ast::PatternKind::MutIdent) {
        emitter.restore_locals(saved_locals);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      const ir::TypeIdx binding_type = expr_type(decl.init);
      const ir::OperandIdx init =
          emit_expr(ast, decl.init, binding_type, emitter, ctx, params);
      if (!init.is_valid()) {
        emitter.restore_locals(saved_locals);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      if (discard) {
        continue;
      }
      const std::string_view local_name = pat.kind == ast::PatternKind::Ident
                                              ? pat.payload.ident.name.name
                                              : pat.payload.mut_ident.name.name;
      const ir::TypeIdx ref_type =
          emitter.builder().reference_type(binding_type, true);
      const ir::RegisterIdx addr(emitter.builder().state().registers.size());
      const ir::InstructionIdx alloca = emitter.emit_alloca(binding_type, addr);
      emitter.builder().reg({.type = binding_type, .def_idx = alloca});
      emitter.cur().push(alloca);
      ir::OperandSeq store_ops;
      store_ops.push(emitter.builder().operand(
          ir::Operand{emitter.builder().state().operands[init.idx]}));
      store_ops.push(emitter.builder().operand(
          ir::Operand::from_register(addr, ref_type)));
      emitter.cur().push(
          emitter.builder().instr({.op = ir::Opcode::Store,
                                   .flags = {},
                                   .dst = ir::RegisterIdx(base::INVALID_IDX),
                                   .measure = binding_type,
                                   .operands = store_ops.finish()}));
      std::optional<comp::CompVal> init_value;
      const ir::Operand& op = emitter.builder().state().operands[init.idx];
      if (op.is<ir::ImmutableIdx>()) {
        const ir::TypeTag tag =
            emitter.builder().state().types[binding_type].tag;
        const u64 bits = emitter.builder()
                             .state()
                             .immutables[op.as_immutable()]
                             .data.u64_value &
                         comp::mask(tag);
        comp::CompVal value;
        value.type = binding_type;
        value.value.tag = tag == ir::TypeTag::I1 ? comp::CompValue::Tag::Bool
                                                 : comp::CompValue::Tag::Int;
        value.value.int_value = bits;
        value.value.bool_value = bits != 0;
        init_value = std::move(value);
      }
      emitter.bind_local(
          local_name,
          emitter.builder().operand(ir::Operand::from_register(addr, ref_type)),
          binding_type, std::move(init_value));
      continue;
    }
    if (stmt.kind == ast::StmtKind::Reassign) {
      const ast::StmtReassign& reassign = stmt.payload.get<ast::StmtReassign>();
      const ast::ExprNode& place_node = ast.exprs[reassign.place];
      // Reassignment into a compile-time binding that holds a semantic
      // aggregate: update the value in the side table instead of
      // storing through an alloca the interpreter cannot read.
      {
        std::string_view root_name;
        std::vector<u32> indices;
        if (semantic_place_path(ast, reassign.place, root_name, indices,
                                expr_type, emitter, resolve_field_index)) {
          ir::OperandIdx root_op = emitter.lookup_value(root_name);
          if (!root_op.is_valid() && params != nullptr) {
            for (const auto& [name, operand] : *params) {
              if (name == root_name) {
                root_op = operand;
                break;
              }
            }
          }
          comp::CompVal root_value;
          bool is_semantic_root = false;
          if (root_op.is_valid()) {
            if (const comp::CompVal* sem = emitter.get_semantic(root_op)) {
              root_value = *sem;
              is_semantic_root = true;
            }
          }
          if (is_semantic_root) {
            const ir::TypeIdx value_ty = expr_type(reassign.value);
            const ir::OperandIdx value_op =
                emit_expr(ast, reassign.value, value_ty, emitter, ctx, params);
            if (!value_op.is_valid()) {
              emitter.restore_locals(saved_locals);
              return ir::OperandIdx(base::INVALID_IDX);
            }
            comp::CompValue new_field;
            if (const comp::CompVal* sem = emitter.get_semantic(value_op)) {
              new_field = sem->value;
            } else {
              u64 bits = 0;
              if (operand_int(emitter, value_op, bits)) {
                new_field.tag = emitter.builder().state().types[value_ty].tag ==
                                        ir::TypeTag::I1
                                    ? comp::CompValue::Tag::Bool
                                    : comp::CompValue::Tag::Int;
                new_field.int_value = bits;
                new_field.bool_value = bits != 0;
              }
            }
            comp::CompValue* slot = semantic_slot(root_value.value, indices);
            if (slot == nullptr) {
              emitter.restore_locals(saved_locals);
              return ir::OperandIdx(base::INVALID_IDX);
            }
            if (reassign.compound && slot->tag == comp::CompValue::Tag::Int &&
                new_field.tag == comp::CompValue::Tag::Int) {
              const ir::TypeTag op_tag =
                  emitter.builder().state().types[value_ty].tag;
              const u64 mask = comp::mask(op_tag);
              const u64 l = slot->int_value & mask;
              const u64 r = new_field.int_value & mask;
              bool overflow = false;
              switch (reassign.op) {
                case ast::BinaryOp::Add:
                  slot->int_value = (l + r) & mask;
                  overflow = comp::add_overflows(l, r, op_tag);
                  break;
                case ast::BinaryOp::Sub:
                  slot->int_value = (l - r) & mask;
                  overflow = comp::sub_overflows(l, r, op_tag);
                  break;
                case ast::BinaryOp::Mul:
                  slot->int_value = (l * r) & mask;
                  overflow = comp::mul_overflows(l, r, op_tag);
                  break;
                case ast::BinaryOp::BitAnd: slot->int_value = l & r; break;
                case ast::BinaryOp::BitOr: slot->int_value = l | r; break;
                case ast::BinaryOp::BitXor: slot->int_value = l ^ r; break;
                default:
                  // Div/rem and shifts are not folded here; decline the
                  // shape rather than silently leaving the slot unchanged.
                  emitter.restore_locals(saved_locals);
                  return ir::OperandIdx(base::INVALID_IDX);
              }
              if (overflow) {
                emitter.set_emit_error(ConstEmitter::EmitError::Overflow);
              }
            } else {
              *slot = std::move(new_field);
            }
            emitter.bind_value(
                root_name, emitter.add_semantic(root_value, root_value.type));
            continue;
          }
        }
      }
      if (place_node.kind == ast::ExprKind::Field ||
          place_node.kind == ast::ExprKind::Index) {
        const ir::OperandIdx address =
            emit_place_address(ast, reassign.place, emitter, ctx, params);
        if (!address.is_valid()) {
          emitter.restore_locals(saved_locals);
          return ir::OperandIdx(base::INVALID_IDX);
        }
        const ir::TypeIdx field_type =
            emitter.builder().state().operands[address.idx].type;
        const ir::TypeIdx pointee =
            emitter.builder()
                .state()
                .ref_types[emitter.builder().state().types[field_type].as_ref()]
                .pointee;
        ir::OperandIdx stored =
            emit_expr(ast, reassign.value, pointee, emitter, ctx, params);
        if (!stored.is_valid()) {
          emitter.restore_locals(saved_locals);
          return ir::OperandIdx(base::INVALID_IDX);
        }
        if (reassign.compound) {
          const ir::RegisterIdx loaded(
              emitter.builder().state().registers.size());
          const ir::InstructionIdx load = emitter.builder().instr(
              {.op = ir::Opcode::Load,
               .flags = {},
               .dst = loaded,
               .measure = pointee,
               .operands = ir::OperandIdxRange{address, 1}});
          emitter.builder().reg({.type = pointee, .def_idx = load});
          emitter.cur().push(load);
          ir::OperandSeq ops;
          ops.push(emitter.builder().operand(
              ir::Operand::from_register(loaded, pointee)));
          ops.push(emitter.builder().operand(
              ir::Operand{emitter.builder().state().operands[stored.idx]}));
          const ir::RegisterIdx dst(emitter.builder().state().registers.size());
          const ir::InstructionIdx add =
              emitter.builder().instr({.op = ir::Opcode::IntAdd,
                                       .flags = {},
                                       .dst = dst,
                                       .measure = pointee,
                                       .operands = ops.finish()});
          emitter.builder().reg({.type = pointee, .def_idx = add});
          emitter.cur().push(add);
          stored = emitter.builder().operand(
              ir::Operand::from_register(dst, pointee));
        }
        ir::OperandSeq store_ops;
        store_ops.push(emitter.builder().operand(
            ir::Operand{emitter.builder().state().operands[stored.idx]}));
        store_ops.push(emitter.builder().operand(
            ir::Operand{emitter.builder().state().operands[address.idx]}));
        emitter.cur().push(
            emitter.builder().instr({.op = ir::Opcode::Store,
                                     .flags = {},
                                     .dst = ir::RegisterIdx(base::INVALID_IDX),
                                     .measure = pointee,
                                     .operands = store_ops.finish()}));
        continue;
      }
      if (place_node.kind != ast::ExprKind::Path) {
        emitter.restore_locals(saved_locals);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      const ast::PathIdx pidx = place_node.payload.get<ast::ExprPath>().idx;
      const std::span<const ast::Ident> psegs = ast.paths[pidx].segments;
      if (psegs.size() != 1) {
        emitter.restore_locals(saved_locals);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      const LocalBinding* local = emitter.lookup_local(psegs[0].name);
      if (local == nullptr) {
        emitter.restore_locals(saved_locals);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      ir::OperandIdx value = emit_expr(
          ast, reassign.value, expr_type(reassign.value), emitter, ctx, params);
      if (!value.is_valid()) {
        emitter.restore_locals(saved_locals);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      if (const comp::CompVal* sem = emitter.get_semantic(value)) {
        if (sem->value.tag == comp::CompValue::Tag::Int ||
            sem->value.tag == comp::CompValue::Tag::Bool) {
          const ir::TypeTag tag =
              emitter.builder().state().types[expr_type(reassign.value)].tag;
          const u64 bits = sem->value.tag == comp::CompValue::Tag::Bool
                               ? (sem->value.bool_value ? 1u : 0u)
                               : sem->value.int_value;
          const ir::ImmutableIdx imm = emitter.builder().immutable(
              {.type = expr_type(reassign.value),
               .data = {.u64_value = bits & comp::mask(tag)}});
          value = emitter.builder().operand(
              ir::Operand::from_immutable(imm, expr_type(reassign.value)));
        }
      }
      if (reassign.compound) {
        ir::Opcode op = ir::Opcode::Noop;
        switch (reassign.op) {
          case ast::BinaryOp::Add: op = ir::Opcode::IntAdd; break;
          case ast::BinaryOp::Sub: op = ir::Opcode::IntSub; break;
          case ast::BinaryOp::Mul: op = ir::Opcode::IntMul; break;
          case ast::BinaryOp::BitAnd: op = ir::Opcode::And; break;
          case ast::BinaryOp::BitOr: op = ir::Opcode::Or; break;
          case ast::BinaryOp::BitXor: op = ir::Opcode::Xor; break;
          default: break;
        }
        if (op == ir::Opcode::Noop) {
          emitter.restore_locals(saved_locals);
          return ir::OperandIdx(base::INVALID_IDX);
        }
        const ir::RegisterIdx loaded(
            emitter.builder().state().registers.size());
        const ir::InstructionIdx load = emitter.builder().instr(
            {.op = ir::Opcode::Load,
             .flags = {},
             .dst = loaded,
             .measure = local->type,
             .operands = ir::OperandIdxRange{local->addr, 1}});
        emitter.builder().reg({.type = local->type, .def_idx = load});
        emitter.cur().push(load);
        ir::OperandSeq ops;
        ops.push(emitter.builder().operand(
            ir::Operand::from_register(loaded, local->type)));
        ops.push(emitter.builder().operand(
            ir::Operand{emitter.builder().state().operands[value.idx]}));
        const ir::RegisterIdx dst(emitter.builder().state().registers.size());
        const ir::InstructionIdx instr =
            emitter.builder().instr({.op = op,
                                     .flags = {},
                                     .dst = dst,
                                     .measure = ir::TypeIdx::invalid(),
                                     .operands = ops.finish()});
        emitter.builder().reg({.type = local->type, .def_idx = instr});
        emitter.cur().push(instr);
        value = emitter.builder().operand(
            ir::Operand::from_register(dst, local->type));
      }
      ir::OperandSeq store_ops;
      store_ops.push(emitter.builder().operand(
          ir::Operand{emitter.builder().state().operands[value.idx]}));
      store_ops.push(emitter.builder().operand(
          ir::Operand{emitter.builder().state().operands[local->addr.idx]}));
      emitter.cur().push(
          emitter.builder().instr({.op = ir::Opcode::Store,
                                   .flags = {},
                                   .dst = ir::RegisterIdx(base::INVALID_IDX),
                                   .measure = local->type,
                                   .operands = store_ops.finish()}));
      std::optional<comp::CompVal> stored;
      if (const comp::CompVal* sem = emitter.get_semantic(value)) {
        stored = *sem;
      } else {
        const ir::Operand& op = emitter.builder().state().operands[value.idx];
        if (op.is<ir::ImmutableIdx>()) {
          const ir::TypeTag tag =
              emitter.builder().state().types[local->type].tag;
          const u64 bits = emitter.builder()
                               .state()
                               .immutables[op.as_immutable()]
                               .data.u64_value &
                           comp::mask(tag);
          comp::CompVal v;
          v.type = local->type;
          v.value.tag = tag == ir::TypeTag::I1 ? comp::CompValue::Tag::Bool
                                               : comp::CompValue::Tag::Int;
          v.value.int_value = bits;
          v.value.bool_value = bits != 0;
          stored = std::move(v);
        }
      }
      if (stored.has_value()) {
        emitter.set_local_value(psegs[0].name, *stored);
      }
      continue;
    }
    if (stmt.kind == ast::StmtKind::Expr) {
      const ast::StmtExpr& expr_stmt = stmt.payload.get<ast::StmtExpr>();
      const ast::ExprKind kind = ast.exprs[expr_stmt.value].kind;
      const bool allowed =
          (kind == ast::ExprKind::Return &&
           emitter.return_target().is_valid()) ||
          (kind == ast::ExprKind::Break && emitter.break_target().is_valid()) ||
          (kind == ast::ExprKind::Continue &&
           emitter.continue_target().is_valid());
      if ((kind == ast::ExprKind::Break || kind == ast::ExprKind::Continue ||
           kind == ast::ExprKind::Return) &&
          !allowed) {
        emitter.restore_locals(saved_locals);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      if (!emit_expr(ast, expr_stmt.value, expr_type(expr_stmt.value), emitter,
                     ctx, params)
               .is_valid()) {
        emitter.restore_locals(saved_locals);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      continue;
    }
    emitter.restore_locals(saved_locals);
    return ir::OperandIdx(base::INVALID_IDX);
  }
  ir::OperandIdx result = emitter.void_operand();
  if (has_value) {
    result = emit_expr(ast, block.value, type, emitter, ctx, params);
  } else {
    result = emitter.void_operand();
  }
  emitter.restore_locals(saved_locals);
  return result;
}

ir::OperandIdx emit_range(const ast::AstArena& ast,
                          const ast::ExprRange& range,
                          ir::TypeIdx type,
                          ConstEmitter& emitter,
                          const EmitContext& ctx,
                          const ParamEnv* params) {
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ir::TypeTag range_tag = emitter.builder().state().types[type].tag;
  if (range_tag != ir::TypeTag::Struct) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::StructType& shape =
      emitter.builder()
          .state()
          .struct_types[emitter.builder().state().types[type].as_struct()];
  if (shape.fields.size() != 2) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::RegisterIdx addr(emitter.builder().state().registers.size());
  const ir::InstructionIdx alloca = emitter.emit_alloca(type, addr);
  emitter.builder().reg({.type = type, .def_idx = alloca});
  emitter.cur().push(alloca);
  const ir::OperandIdx base =
      emitter.builder().operand(ir::Operand::from_register(addr, type));
  const ir::TypeIdx i32_ty = emitter.builder().primitive(ir::TypeTag::I32);
  for (u32 i = 0; i < 2; ++i) {
    const ir::TypeIdx bound_ty = shape.fields[i];
    const ir::TypeTag bound_tag = emitter.builder().state().types[bound_ty].tag;
    if (bound_tag != ir::TypeTag::Enum) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const ir::EnumType& enum_shape =
        emitter.builder()
            .state()
            .enum_types[emitter.builder().state().types[bound_ty].as_enum()];
    const ast::ExprIdx endpoint = i == 0 ? range.start : range.end;
    const bool included = i == 0 || range.inclusive;
    const std::string_view variant_name =
        !endpoint.is_valid() ? "Unbounded"
                             : (included ? "Included" : "Excluded");
    u32 variant = 0;
    if (!emitter.resolve_variant_index(bound_ty, variant_name, variant)) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    ir::OperandSeq gep_ops;
    gep_ops.push(emitter.builder().operand(
        ir::Operand{emitter.builder().state().operands[base.idx]}));
    gep_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
        emitter.builder().immutable({.type = i32_ty, .data = {.i32_value = 0}}),
        i32_ty)));
    gep_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
        emitter.builder().immutable(
            {.type = i32_ty, .data = {.i32_value = static_cast<i32>(i)}}),
        i32_ty)));
    const ir::TypeIdx bound_ref =
        emitter.builder().reference_type(bound_ty, true);
    const ir::RegisterIdx field_addr(
        emitter.builder().state().registers.size());
    const ir::InstructionIdx field_instr =
        emitter.builder().instr({.op = ir::Opcode::GetElementPtr,
                                 .flags = {},
                                 .dst = field_addr,
                                 .measure = bound_ty,
                                 .operands = gep_ops.finish()});
    emitter.builder().reg({.type = bound_ref, .def_idx = field_instr});
    emitter.cur().push(field_instr);
    const ir::OperandIdx field_op = emitter.builder().operand(
        ir::Operand::from_register(field_addr, bound_ref));
    ir::OperandSeq tag_ops;
    tag_ops.push(field_op);
    tag_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
        emitter.builder().immutable(
            {.type = i32_ty, .data = {.i32_value = static_cast<i32>(variant)}}),
        i32_ty)));
    emitter.cur().push(
        emitter.builder().instr({.op = ir::Opcode::EnumTagSet,
                                 .flags = {},
                                 .dst = ir::RegisterIdx(base::INVALID_IDX),
                                 .measure = ir::TypeIdx::invalid(),
                                 .operands = tag_ops.finish()}));
    if (endpoint.is_valid()) {
      const ir::EnumVariantTypeIdx vidx(enum_shape.variants.head().idx +
                                        variant);
      const ir::EnumVariantType& vshape =
          emitter.builder().state().enum_variant_types[vidx];
      if (vshape.fields.size() != 1) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      const ir::TypeIdx payload_ty = vshape.fields[0];
      const ir::OperandIdx payload =
          emit_expr(ast, endpoint, payload_ty, emitter, ctx, params);
      if (!payload.is_valid()) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      ir::OperandSeq fa_ops;
      fa_ops.push(emitter.builder().operand(
          ir::Operand::from_register(field_addr, bound_ref)));
      fa_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
          emitter.builder().immutable(
              {.type = i32_ty,
               .data = {.i32_value = static_cast<i32>(variant)}}),
          i32_ty)));
      fa_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
          emitter.builder().immutable(
              {.type = i32_ty, .data = {.i32_value = 0}}),
          i32_ty)));
      const ir::RegisterIdx payload_addr(
          emitter.builder().state().registers.size());
      const ir::InstructionIdx fa_instr =
          emitter.builder().instr({.op = ir::Opcode::EnumFieldAddr,
                                   .flags = {},
                                   .dst = payload_addr,
                                   .measure = payload_ty,
                                   .operands = fa_ops.finish()});
      emitter.builder().reg({.type = payload_ty, .def_idx = fa_instr});
      emitter.cur().push(fa_instr);
      ir::OperandSeq store_ops;
      store_ops.push(emitter.builder().operand(
          ir::Operand{emitter.builder().state().operands[payload.idx]}));
      store_ops.push(emitter.builder().operand(
          ir::Operand::from_register(payload_addr, payload_ty)));
      emitter.cur().push(
          emitter.builder().instr({.op = ir::Opcode::Store,
                                   .flags = {},
                                   .dst = ir::RegisterIdx(base::INVALID_IDX),
                                   .measure = payload_ty,
                                   .operands = store_ops.finish()}));
    }
  }
  return base;
}

ir::OperandIdx emit_pattern_if(const ast::AstArena& ast,
                               const ast::ExprIf& if_node,
                               const ast::Cond& cond,
                               ir::TypeIdx type,
                               ConstEmitter& emitter,
                               const EmitContext& ctx,
                               const ParamEnv* params) {
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  if (!if_node.else_block.is_valid() &&
      emitter.builder().state().types[type].tag != ir::TypeTag::Void) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ast::PatternNode& pat = ast.patterns[cond.pattern];
  if (pat.kind != ast::PatternKind::Tuple ||
      pat.payload.tuple.elements.size() != 1 ||
      ast.patterns[pat.payload.tuple.elements[0]].kind !=
          ast::PatternKind::Ident ||
      !pat.payload.tuple.path.is_valid() ||
      ast.paths[pat.payload.tuple.path].segments.empty() ||
      !cond.init.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::TypeIdx init_type = expr_type(cond.init);
  if (emitter.builder().state().types[init_type].tag != ir::TypeTag::Enum) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::OperandIdx init =
      emit_expr(ast, cond.init, init_type, emitter, ctx, params);
  if (!init.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const std::string_view vname =
      ast.paths[pat.payload.tuple.path].segments.back().name;
  u32 variant = 0;
  if (!emitter.resolve_variant_index(init_type, vname, variant)) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  // A compile-time enum scrutinee is a semantic value, not an address,
  // so resolve the arm directly instead of loading a tag from storage.
  if (const comp::CompVal* sem = emitter.get_semantic(init)) {
    const bool matched = sem->value.tag == comp::CompValue::Tag::Enum &&
                         sem->value.variant == variant &&
                         sem->value.fields.size() == 1;
    const ast::Block& chosen = matched ? ast.blocks[if_node.then_block]
                                       : (if_node.else_block.is_valid()
                                              ? ast.blocks[if_node.else_block]
                                              : ast.blocks[if_node.then_block]);
    if (!matched && !if_node.else_block.is_valid()) {
      return emitter.void_operand();
    }
    const usize saved = emitter.value_bindings_size();
    if (matched) {
      const ir::EnumType& shape =
          emitter.builder()
              .state()
              .enum_types[emitter.builder().state().types[init_type].as_enum()];
      const ir::EnumVariantTypeIdx vidx(shape.variants.head().idx + variant);
      const ir::EnumVariantType& variant_shape =
          emitter.builder().state().enum_variant_types[vidx];
      const ir::TypeIdx payload_ty =
          variant_shape.fields.empty() ? init_type : variant_shape.fields[0];
      comp::CompVal payload;
      payload.value = sem->value.fields[0];
      payload.type = payload_ty;
      const ast::PatternIdx binding = pat.payload.tuple.elements[0];
      emitter.bind_value(ast.patterns[binding].payload.ident.name.name,
                         emitter.add_value(payload, payload_ty));
    }
    const ir::OperandIdx result =
        chosen.value.is_valid()
            ? emit_expr(ast, chosen.value, type, emitter, ctx, params)
            : emitter.void_operand();
    emitter.restore_value_bindings(saved);
    return result;
  }
  const ir::TypeIdx i32_ty = emitter.builder().primitive(ir::TypeTag::I32);
  const ir::RegisterIdx disc(emitter.builder().state().registers.size());
  const ir::InstructionIdx tag_instr =
      emitter.builder().instr({.op = ir::Opcode::EnumTagLoad,
                               .flags = {},
                               .dst = disc,
                               .measure = ir::TypeIdx::invalid(),
                               .operands = ir::OperandIdxRange{init, 1}});
  emitter.builder().reg({.type = i32_ty, .def_idx = tag_instr});
  emitter.cur().push(tag_instr);
  // Each instruction's operands must occupy a contiguous range in the
  // storage, so build each sequence's operands in push order rather than
  // sharing one operand across two sequences.
  ir::OperandSeq cmp_ops;
  cmp_ops.push(
      emitter.builder().operand(ir::Operand::from_register(disc, i32_ty)));
  cmp_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
      emitter.builder().immutable(
          {.type = i32_ty, .data = {.i32_value = static_cast<i32>(variant)}}),
      i32_ty)));
  const ir::RegisterIdx is_match(emitter.builder().state().registers.size());
  const ir::InstructionIdx cmp_instr =
      emitter.builder().instr({.op = ir::Opcode::Eq,
                               .flags = {},
                               .dst = is_match,
                               .measure = ir::TypeIdx::invalid(),
                               .operands = cmp_ops.finish()});
  emitter.builder().reg({.type = emitter.builder().primitive(ir::TypeTag::I1),
                         .def_idx = cmp_instr});
  emitter.cur().push(cmp_instr);
  const ir::OperandIdx cond_val =
      emitter.builder().operand(ir::Operand::from_register(
          is_match, emitter.builder().primitive(ir::TypeTag::I1)));

  const bool has_else = if_node.else_block.is_valid();
  const ast::Block& then_block = ast.blocks[if_node.then_block];
  const ast::Block* else_block =
      has_else ? &ast.blocks[if_node.else_block] : nullptr;
  if (!then_block.value.is_valid() ||
      (has_else && !else_block->value.is_valid())) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::BlockIdx then_b = emitter.new_block();
  const ir::BlockIdx else_b = emitter.new_block();
  const ir::BlockIdx merge_b = emitter.new_block();
  emitter.emit_cond_br(cond_val, then_b, else_b);
  ir::RegisterIdx result_reg = ir::RegisterIdx(base::INVALID_IDX);
  if (has_else &&
      emitter.builder().state().types[type].tag != ir::TypeTag::Void) {
    result_reg = emitter.claim_reg(type);
    emitter.set_block_params(merge_b, emitter.block_param(type, result_reg));
  }
  const usize saved_bindings = emitter.value_bindings_size();

  emitter.set_cur(then_b);
  const ir::EnumType& shape =
      emitter.builder()
          .state()
          .enum_types[emitter.builder().state().types[init_type].as_enum()];
  const ir::EnumVariantTypeIdx vidx(shape.variants.head().idx + variant);
  const ir::EnumVariantType& variant_shape =
      emitter.builder().state().enum_variant_types[vidx];
  if (variant_shape.fields.size() != 1 || !then_block.statements.empty()) {
    emitter.restore_value_bindings(saved_bindings);
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::TypeIdx payload_ty = variant_shape.fields[0];
  ir::OperandSeq fa_ops;
  fa_ops.push(emitter.builder().operand(
      ir::Operand{emitter.builder().state().operands[init.idx]}));
  fa_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
      emitter.builder().immutable(
          {.type = i32_ty, .data = {.i32_value = static_cast<i32>(variant)}}),
      i32_ty)));
  fa_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
      emitter.builder().immutable({.type = i32_ty, .data = {.i32_value = 0}}),
      i32_ty)));
  const ir::RegisterIdx payload_addr(
      emitter.builder().state().registers.size());
  const ir::InstructionIdx fa_instr =
      emitter.builder().instr({.op = ir::Opcode::EnumFieldAddr,
                               .flags = {},
                               .dst = payload_addr,
                               .measure = payload_ty,
                               .operands = fa_ops.finish()});
  emitter.builder().reg({.type = payload_ty, .def_idx = fa_instr});
  emitter.cur().push(fa_instr);
  const ir::RegisterIdx loaded(emitter.builder().state().registers.size());
  const ir::InstructionIdx load_instr = emitter.builder().instr(
      {.op = ir::Opcode::Load,
       .flags = {},
       .dst = loaded,
       .measure = payload_ty,
       .operands = ir::OperandIdxRange{
           emitter.builder().operand(
               ir::Operand::from_register(payload_addr, payload_ty)),
           1}});
  emitter.builder().reg({.type = payload_ty, .def_idx = load_instr});
  emitter.cur().push(load_instr);
  emitter.bind_value(
      ast.patterns[pat.payload.tuple.elements[0]].payload.ident.name.name,
      emitter.builder().operand(
          ir::Operand::from_register(loaded, payload_ty)));

  const ir::OperandIdx then_val =
      emit_expr(ast, then_block.value, type, emitter, ctx, params);
  emitter.restore_value_bindings(saved_bindings);
  if (!then_val.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (!emitter.terminated()) {
    emitter.emit_br(merge_b, result_reg.is_valid()
                                 ? then_val
                                 : ir::OperandIdx(base::INVALID_IDX));
  }
  emitter.set_cur(else_b);
  if (has_else) {
    const ir::OperandIdx else_val =
        emit_expr(ast, else_block->value, type, emitter, ctx, params);
    if (!else_val.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    if (!emitter.terminated()) {
      emitter.emit_br(merge_b, else_val);
    }
  } else {
    emitter.emit_br(merge_b, ir::OperandIdx(base::INVALID_IDX));
  }
  emitter.set_cur(merge_b);
  if (result_reg.is_valid()) {
    return emitter.builder().operand(
        ir::Operand::from_register(result_reg, type));
  }
  return emitter.void_operand();
}

// Reads a compile-time integer from an operand, whether it rides as a
// semantic value or as a lowered immutable.
static bool operand_int(ConstEmitter& emitter, ir::OperandIdx op, u64& out) {
  if (const comp::CompVal* sem = emitter.get_semantic(op)) {
    if (sem->value.tag != comp::CompValue::Tag::Int) {
      return false;
    }
    out = sem->value.int_value;
    return true;
  }
  const ir::Operand& operand = emitter.builder().state().operands[op.idx];
  if (!operand.is<ir::ImmutableIdx>()) {
    return false;
  }
  out = emitter.builder()
            .state()
            .immutables[operand.as_immutable()]
            .data.u64_value;
  return true;
}

// Reads a compile-time boolean from a semantic value or a lowered I1
// immutable. Logical operators fold on these instead of emitting IR.
static bool operand_bool(ConstEmitter& emitter, ir::OperandIdx op, bool& out) {
  if (const comp::CompVal* sem = emitter.get_semantic(op)) {
    if (sem->value.tag == comp::CompValue::Tag::Bool) {
      out = sem->value.bool_value;
      return true;
    }
    if (sem->value.tag == comp::CompValue::Tag::Int) {
      out = sem->value.int_value != 0;
      return true;
    }
    return false;
  }
  const ir::Operand& operand = emitter.builder().state().operands[op.idx];
  if (!operand.is<ir::ImmutableIdx>()) {
    return false;
  }
  out = emitter.builder()
            .state()
            .immutables[operand.as_immutable()]
            .data.u64_value != 0;
  return true;
}

static ir::OperandIdx bool_immutable(ConstEmitter& emitter,
                                     ir::TypeIdx type,
                                     bool value) {
  const ir::ImmutableIdx imm =
      emitter.builder().immutable({.type = type, .data = {.i1_value = value}});
  return emitter.builder().operand(ir::Operand::from_immutable(imm, type));
}

static ir::OperandIdx emit_comp_str_slice(ConstEmitter& emitter,
                                          ir::TypeIdx type,
                                          ir::OperandIdx string,
                                          ir::OperandIdx start,
                                          ir::OperandIdx end) {
  const auto copy = [&emitter](ir::OperandIdx index) {
    return emitter.builder().state().operands[index.idx];
  };
  const ir::OperandIdx head = emitter.builder().operand(copy(string));
  emitter.builder().operand(copy(start));
  emitter.builder().operand(copy(end));
  const ir::RegisterIdx dst(emitter.builder().state().registers.size());
  const ir::InstructionIdx instr =
      emitter.builder().instr({.op = ir::Opcode::CompStrSlice,
                               .flags = {},
                               .dst = dst,
                               .measure = ir::TypeIdx::invalid(),
                               .operands = ir::OperandIdxRange{head, 3}});
  emitter.builder().reg({.type = type, .def_idx = instr});
  emitter.cur().push(instr);
  return emitter.builder().operand(ir::Operand::from_register(dst, type));
}

ir::OperandIdx emit_str_intrinsic(const ast::AstArena& ast,
                                  const ast::ExprCall& call,
                                  std::string_view cname,
                                  ir::TypeIdx type,
                                  ConstEmitter& emitter,
                                  const EmitContext& ctx,
                                  const ParamEnv* params) {
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  if (cname == "str_len" || cname == "str_byte" || cname == "str_slice") {
    if ((cname == "str_len" && call.args.size() != 1) ||
        (cname == "str_byte" && call.args.size() != 2) ||
        (cname == "str_slice" && call.args.size() != 3)) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const ir::OperandIdx receiver = emit_expr(
        ast, call.args[0], expr_type(call.args[0]), emitter, ctx, params);
    if (!receiver.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const comp::CompVal* const src = emitter.get_semantic(receiver);
    if (src == nullptr && cname == "str_len") {
      ir::OperandSeq len_ops;
      len_ops.push(emitter.builder().operand(
          ir::Operand{emitter.builder().state().operands[receiver.idx]}));
      const ir::RegisterIdx dst(emitter.builder().state().registers.size());
      const ir::InstructionIdx instr =
          emitter.builder().instr({.op = ir::Opcode::CompStrLen,
                                   .flags = {},
                                   .dst = dst,
                                   .measure = ir::TypeIdx::invalid(),
                                   .operands = len_ops.finish()});
      emitter.builder().reg({.type = type, .def_idx = instr});
      emitter.cur().push(instr);
      return emitter.builder().operand(ir::Operand::from_register(dst, type));
    }
    if (src == nullptr || src->value.tag != comp::CompValue::Tag::Str) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const comp::CompVal value = *src;
    const std::string_view bytes =
        comp::string_bytes(value.value, ctx.evaluator.strings());
    if (cname == "str_len") {
      const ir::ImmutableIdx imm = emitter.builder().immutable(
          {.type = type,
           .data = {.u64_value = static_cast<u64>(bytes.size())}});
      return emitter.builder().operand(ir::Operand::from_immutable(imm, type));
    }
    const ir::OperandIdx first = emit_expr(
        ast, call.args[1], expr_type(call.args[1]), emitter, ctx, params);
    if (!first.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    u64 index = 0;
    if (!operand_int(emitter, first, index)) {
      // A runtime index (e.g. a loop counter): let the interpreter read
      // the byte from the semantic string at execution time.
      if (cname == "str_byte" && emitter.get_semantic(receiver) != nullptr) {
        ir::Operand recv_copy =
            emitter.builder().state().operands[receiver.idx];
        const ir::OperandIdx first_op =
            emitter.builder().operand(std::move(recv_copy));
        ir::Operand idx_copy = emitter.builder().state().operands[first.idx];
        emitter.builder().operand(std::move(idx_copy));
        const ir::RegisterIdx dst(emitter.builder().state().registers.size());
        const ir::InstructionIdx instr = emitter.builder().instr(
            {.op = ir::Opcode::CompStrByte,
             .flags = {},
             .dst = dst,
             .measure = ir::TypeIdx::invalid(),
             .operands = ir::OperandIdxRange{first_op, 2}});
        emitter.builder().reg({.type = type, .def_idx = instr});
        emitter.cur().push(instr);
        return emitter.builder().operand(ir::Operand::from_register(dst, type));
      }
      if (cname == "str_slice") {
        const ir::OperandIdx second = emit_expr(
            ast, call.args[2], expr_type(call.args[2]), emitter, ctx, params);
        if (!second.is_valid()) {
          return ir::OperandIdx(base::INVALID_IDX);
        }
        return emit_comp_str_slice(emitter, type, receiver, first, second);
      }
      return ir::OperandIdx(base::INVALID_IDX);
    }
    if (index > bytes.size()) {
      emitter.set_emit_error(ConstEmitter::EmitError::OutOfBounds);
      return ir::OperandIdx(base::INVALID_IDX);
    }
    if (cname == "str_byte") {
      if (index >= bytes.size()) {
        emitter.set_emit_error(ConstEmitter::EmitError::OutOfBounds);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      const ir::ImmutableIdx imm = emitter.builder().immutable(
          {.type = type,
           .data = {.u64_value =
                        static_cast<u8>(bytes[static_cast<usize>(index)])}});
      return emitter.builder().operand(ir::Operand::from_immutable(imm, type));
    }
    const ir::OperandIdx second = emit_expr(
        ast, call.args[2], expr_type(call.args[2]), emitter, ctx, params);
    if (!second.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    u64 end = 0;
    if (!operand_int(emitter, second, end)) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    if (end < index || end > bytes.size()) {
      emitter.set_emit_error(ConstEmitter::EmitError::OutOfBounds);
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const auto boundary = [&](u64 at) {
      if (at == 0 || at == bytes.size()) {
        return true;
      }
      return (static_cast<u8>(bytes[static_cast<usize>(at)]) & 0xC0u) != 0x80u;
    };
    if (!boundary(index) || !boundary(end)) {
      emitter.set_emit_error(ConstEmitter::EmitError::SliceBoundary);
      return ir::OperandIdx(base::INVALID_IDX);
    }
    comp::CompVal result;
    result.type = type;
    result.value.tag = comp::CompValue::Tag::Str;
    result.value.owned_str = std::string(bytes.substr(
        static_cast<usize>(index), static_cast<usize>(end - index)));
    return emitter.add_semantic(result, type);
  }
  return ir::OperandIdx(base::INVALID_IDX);
}

ir::OperandIdx emit_method_call(const ast::AstArena& ast,
                                const ast::ExprMethodCall& method,
                                ir::TypeIdx type,
                                ConstEmitter& emitter,
                                const EmitContext& ctx,
                                const ParamEnv* params) {
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ir::TypeIdx receiver_type = expr_type(method.receiver);
  if (method.name.name == "len" && method.args.empty()) {
    const ir::OperandIdx recv =
        emit_expr(ast, method.receiver, receiver_type, emitter, ctx, params);
    if (recv.is_valid()) {
      if (const comp::CompVal* sem = emitter.get_semantic(recv)) {
        if (sem->value.tag == comp::CompValue::Tag::Variadic) {
          const ir::ImmutableIdx imm = emitter.builder().immutable(
              {.type = type,
               .data = {.u64_value = sem->value.slot_types.size()}});
          return emitter.builder().operand(
              ir::Operand::from_immutable(imm, type));
        }
      }
    }
  }
  if (method.name.name == "len" && method.args.empty() &&
      receiver_type.is_valid() &&
      emitter.builder().state().types[receiver_type].tag ==
          ir::TypeTag::Array) {
    const ir::ArrayType& arr =
        emitter.builder().state().array_types
            [emitter.builder().state().types[receiver_type].as_array()];
    const ir::ImmutableIdx imm = emitter.builder().immutable(
        {.type = type, .data = {.u64_value = arr.count}});
    return emitter.builder().operand(ir::Operand::from_immutable(imm, type));
  }
  const bool is_len_bytes = method.name.name == "len_bytes";
  const bool is_is_empty = method.name.name == "is_empty";
  const bool is_byte = method.name.name == "byte";
  const bool is_byte_slice = method.name.name == "byte_slice";
  if (!is_len_bytes && !is_is_empty && !is_byte && !is_byte_slice) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (((is_len_bytes || is_is_empty) && !method.args.empty()) ||
      ((is_byte || is_byte_slice) && method.args.size() != 1 &&
       !(is_byte_slice && method.args.size() == 2))) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::OperandIdx receiver =
      emit_expr(ast, method.receiver, receiver_type, emitter, ctx, params);
  if (!receiver.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const comp::CompVal* const src = emitter.get_semantic(receiver);
  if (src == nullptr || src->value.tag != comp::CompValue::Tag::Str) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const comp::CompVal value = *src;
  const std::string_view bytes =
      comp::string_bytes(value.value, ctx.evaluator.strings());
  if (is_len_bytes) {
    const ir::ImmutableIdx imm = emitter.builder().immutable(
        {.type = type, .data = {.u64_value = static_cast<u64>(bytes.size())}});
    return emitter.builder().operand(ir::Operand::from_immutable(imm, type));
  }
  if (is_is_empty) {
    const ir::ImmutableIdx imm = emitter.builder().immutable(
        {.type = type, .data = {.i1_value = bytes.empty()}});
    return emitter.builder().operand(ir::Operand::from_immutable(imm, type));
  }
  const ir::OperandIdx first = emit_expr(
      ast, method.args[0], expr_type(method.args[0]), emitter, ctx, params);
  if (!first.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  u64 index = 0;
  if (!operand_int(emitter, first, index)) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (is_byte) {
    if (index >= bytes.size()) {
      emitter.set_emit_error(ConstEmitter::EmitError::OutOfBounds);
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const ir::ImmutableIdx imm = emitter.builder().immutable(
        {.type = type,
         .data = {.u64_value =
                      static_cast<u8>(bytes[static_cast<usize>(index)])}});
    return emitter.builder().operand(ir::Operand::from_immutable(imm, type));
  }
  if (method.args.size() != 2) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::OperandIdx second = emit_expr(
      ast, method.args[1], expr_type(method.args[1]), emitter, ctx, params);
  if (!second.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  u64 end = 0;
  if (!operand_int(emitter, second, end)) {
    return emit_comp_str_slice(emitter, type, receiver, first, second);
  }
  // `byte_slice` returns `Option<str>`: build the enum as a semantic
  // value so a `match` over it can be resolved without lowering the
  // string to memory. Out-of-range boundaries answer `None`.
  u32 some_variant = 0;
  u32 none_variant = 0;
  if (!emitter.resolve_variant_index(type, "Some", some_variant) ||
      !emitter.resolve_variant_index(type, "None", none_variant)) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (end < index || end > bytes.size()) {
    comp::CompVal none;
    none.type = type;
    none.value.tag = comp::CompValue::Tag::Enum;
    none.value.variant = none_variant;
    return emitter.add_semantic(none, type);
  }
  const auto boundary = [&](u64 at) {
    if (at == 0 || at == bytes.size()) {
      return true;
    }
    return (static_cast<u8>(bytes[static_cast<usize>(at)]) & 0xC0u) != 0x80u;
  };
  if (!boundary(index) || !boundary(end)) {
    comp::CompVal none;
    none.type = type;
    none.value.tag = comp::CompValue::Tag::Enum;
    none.value.variant = none_variant;
    return emitter.add_semantic(none, type);
  }
  const ir::EnumType& shape =
      emitter.builder()
          .state()
          .enum_types[emitter.builder().state().types[type].as_enum()];
  const ir::EnumVariantTypeIdx some_idx(shape.variants.head().idx +
                                        some_variant);
  const ir::EnumVariantType& some_shape =
      emitter.builder().state().enum_variant_types[some_idx];
  if (some_shape.fields.size() != 1) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::TypeIdx payload_ty = some_shape.fields[0];
  comp::CompVal payload;
  payload.type = payload_ty;
  payload.value.tag = comp::CompValue::Tag::Str;
  payload.value.owned_str = std::string(
      bytes.substr(static_cast<usize>(index), static_cast<usize>(end - index)));
  comp::CompVal result;
  result.type = type;
  result.value.tag = comp::CompValue::Tag::Enum;
  result.value.variant = some_variant;
  result.value.fields.push_back(payload.value);
  return emitter.add_semantic(result, type);
}

ir::OperandIdx emit_comp_for(const ast::AstArena& ast,
                             const ast::Block& block,
                             ConstEmitter& emitter,
                             const EmitContext& ctx,
                             const ParamEnv* params) {
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::StmtDecl& decl =
      ast.stmts[block.statements[0]].payload.get<ast::StmtDecl>();
  const ast::ExprIdx into = decl.init;
  if (!into.is_valid() || ast.exprs[into].kind != ast::ExprKind::MethodCall) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ast::ExprIdx head =
      ast.exprs[into].payload.get<ast::ExprMethodCall>().receiver;
  if (!head.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const bool from_range = ast.exprs[head].kind == ast::ExprKind::Range;
  ir::OperandIdx array_op = ir::OperandIdx(base::INVALID_IDX);
  std::vector<comp::CompVal> elements;
  if (ast.exprs[head].kind == ast::ExprKind::Range) {
    const ast::ExprRange& range = ast.exprs[head].payload.get<ast::ExprRange>();
    if (!range.start.is_valid() || !range.end.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const ir::OperandIdx start_op = emit_expr(
        ast, range.start, expr_type(range.start), emitter, ctx, params);
    const ir::OperandIdx end_op =
        emit_expr(ast, range.end, expr_type(range.end), emitter, ctx, params);
    u64 first = 0;
    u64 last = 0;
    if (!start_op.is_valid() || !end_op.is_valid() ||
        !operand_int(emitter, start_op, first) ||
        !operand_int(emitter, end_op, last)) {
      // A bound written as a path that is not comp-known is the source
      // diagnostic; flag it so the caller can report it instead of
      // silently falling through to the source diagnostic.
      if (ast.exprs[range.start].kind == ast::ExprKind::Path ||
          ast.exprs[range.end].kind == ast::ExprKind::Path) {
        emitter.set_emit_error(ConstEmitter::EmitError::UnresolvedPath);
      }
      return ir::OperandIdx(base::INVALID_IDX);
    }
    if (last < first) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const ir::TypeIdx elem_type = expr_type(range.start);
    // Check the cap inside the loop: an inclusive bound at U64_MAX would
    // otherwise wrap forever, and even a large bound allocates before the
    // post-loop cap check gets a chance to refuse it.
    for (u64 v = first; range.inclusive ? v <= last : v < last; ++v) {
      if (!emitter.spend_quota()) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      if (elements.size() >= COMP_MAX_UNROLL) {
        emitter.set_emit_error(ConstEmitter::EmitError::ExpansionCap);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      comp::CompVal item;
      item.type = elem_type;
      item.value.tag = comp::CompValue::Tag::Int;
      item.value.int_value = v;
      elements.push_back(std::move(item));
      if (range.inclusive && v == U64_MAX) {
        break;
      }
    }
  } else {
    // Iterating a materialized array literal uses a slice cursor the
    // const IR does not carry; only a range or a comp-known sequence
    // constructor is unrolled here.
    if (ast.exprs[head].kind == ast::ExprKind::Array) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const ir::TypeIdx head_type = expr_type(head);
    array_op = emit_expr(ast, head, head_type, emitter, ctx, params);
    if (!array_op.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const comp::CompVal* seq = emitter.get_semantic(array_op);
    if (seq == nullptr || seq->value.tag != comp::CompValue::Tag::Array) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const ir::TypeIdx element_type =
        emitter.builder()
            .state()
            .array_types[emitter.builder().state().types[head_type].as_array()]
            .element;
    for (const comp::CompValue& field : seq->value.fields) {
      if (!emitter.spend_quota()) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      comp::CompVal item;
      item.value = field;
      item.type = element_type;
      elements.push_back(std::move(item));
    }
  }
  if (elements.empty()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (elements.size() > COMP_MAX_UNROLL) {
    emitter.set_emit_error(ConstEmitter::EmitError::ExpansionCap);
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ast::ExprLoop& loop_node =
      ast.exprs[block.value].payload.get<ast::ExprLoop>();
  const ast::ExprIdx match_expr = ast.blocks[loop_node.body].value;
  if (!match_expr.is_valid() ||
      ast.exprs[match_expr].kind != ast::ExprKind::Match) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ast::ExprMatch& match_node =
      ast.exprs[match_expr].payload.get<ast::ExprMatch>();
  if (match_node.arms.empty()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ast::PatternNode& bind_pat = ast.patterns[match_node.arms[0].pattern];
  if (bind_pat.kind != ast::PatternKind::Tuple ||
      bind_pat.payload.tuple.elements.empty()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ast::PatternNode& element_pat =
      ast.patterns[bind_pat.payload.tuple.elements[0]];
  const bool bind_element = element_pat.kind == ast::PatternKind::Ident ||
                            element_pat.kind == ast::PatternKind::MutIdent;
  if (!bind_element && element_pat.kind != ast::PatternKind::Wildcard) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const std::string_view var_name =
      bind_element ? (element_pat.kind == ast::PatternKind::Ident
                          ? element_pat.payload.ident.name.name
                          : element_pat.payload.mut_ident.name.name)
                   : std::string_view{};
  const ast::ExprIdx body_expr = match_node.arms[0].body;
  if (!body_expr.is_valid() ||
      ast.exprs[body_expr].kind != ast::ExprKind::Block) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ast::Block& body_block =
      ast.blocks[ast.exprs[body_expr].payload.get<ast::ExprBlock>().block];

  const u64 iterations = elements.size();
  if (!from_range) {
    // An array loop reads its element at runtime through the side table,
    // so the body can sit behind a back edge like a range loop.
    const ir::TypeIdx usize_ty =
        emitter.builder().primitive(ir::TypeTag::Usize);
    const ir::TypeIdx element_ty = elements[0].type;
    const ir::BlockIdx header = emitter.new_block();
    const ir::BlockIdx step_b = emitter.new_block();
    const ir::BlockIdx body_b = emitter.new_block();
    const ir::BlockIdx exit_b = emitter.new_block();
    const ir::RegisterIdx idx_reg = emitter.claim_reg(usize_ty);
    emitter.set_block_params(header, emitter.block_param(usize_ty, idx_reg));
    const ir::OperandIdx zero =
        emitter.builder().operand(ir::Operand::from_immutable(
            emitter.builder().immutable(
                {.type = usize_ty, .data = {.u64_value = 0}}),
            usize_ty));
    if (!emitter.terminated()) {
      emitter.emit_br(header, zero);
    }
    emitter.set_cur(header);
    const ir::OperandIdx current = emitter.builder().operand(
        ir::Operand::from_register(idx_reg, usize_ty));
    const ir::OperandIdx limit =
        emitter.builder().operand(ir::Operand::from_immutable(
            emitter.builder().immutable(
                {.type = usize_ty, .data = {.u64_value = iterations}}),
            usize_ty));
    ir::OperandSeq cmp_ops;
    cmp_ops.push(current);
    cmp_ops.push(limit);
    const ir::RegisterIdx cond_reg =
        emitter.claim_reg(emitter.builder().primitive(ir::TypeTag::I1));
    const ir::InstructionIdx cmp =
        emitter.builder().instr({.op = ir::Opcode::Lt,
                                 .flags = {},
                                 .dst = cond_reg,
                                 .measure = ir::TypeIdx::invalid(),
                                 .operands = cmp_ops.finish()});
    emitter.builder().reg(
        {.type = emitter.builder().primitive(ir::TypeTag::I1), .def_idx = cmp});
    emitter.cur().push(cmp);
    emitter.emit_cond_br(
        emitter.builder().operand(ir::Operand::from_register(
            cond_reg, emitter.builder().primitive(ir::TypeTag::I1))),
        body_b, exit_b);

    emitter.set_loop_targets(exit_b, step_b);
    emitter.set_cur(body_b);
    ir::OperandSeq index_ops;
    index_ops.push(emitter.builder().operand(
        ir::Operand{emitter.builder().state().operands[array_op.idx]}));
    index_ops.push(emitter.builder().operand(
        ir::Operand::from_register(idx_reg, usize_ty)));
    const ir::RegisterIdx element_reg = emitter.claim_reg(element_ty);
    const ir::InstructionIdx element_instr =
        emitter.builder().instr({.op = ir::Opcode::CompArrayIndex,
                                 .flags = {},
                                 .dst = element_reg,
                                 .measure = element_ty,
                                 .operands = index_ops.finish()});
    emitter.builder().reg({.type = element_ty, .def_idx = element_instr});
    emitter.cur().push(element_instr);
    const ir::OperandIdx element_op = emitter.builder().operand(
        ir::Operand::from_register(element_reg, element_ty));
    emitter.clear_semantic(element_op);
    if (bind_element) {
      emitter.bind_value(var_name, element_op);
    }
    const ir::OperandIdx body_val = emit_block(
        ast, body_block, emitter.builder().primitive(ir::TypeTag::Void),
        emitter, ctx, params);
    const bool body_ok = body_val.is_valid();
    if (body_ok && !emitter.terminated()) {
      emitter.emit_br(step_b, ir::OperandIdx(base::INVALID_IDX));
    }
    emitter.set_cur(step_b);
    const ir::OperandIdx step_current = emitter.builder().operand(
        ir::Operand::from_register(idx_reg, usize_ty));
    const ir::OperandIdx one =
        emitter.builder().operand(ir::Operand::from_immutable(
            emitter.builder().immutable(
                {.type = usize_ty, .data = {.u64_value = 1}}),
            usize_ty));
    ir::OperandSeq add_ops;
    add_ops.push(step_current);
    add_ops.push(one);
    const ir::RegisterIdx next_reg = emitter.claim_reg(usize_ty);
    const ir::InstructionIdx add =
        emitter.builder().instr({.op = ir::Opcode::IntAdd,
                                 .flags = {},
                                 .dst = next_reg,
                                 .measure = usize_ty,
                                 .operands = add_ops.finish()});
    emitter.builder().reg({.type = usize_ty, .def_idx = add});
    emitter.cur().push(add);
    emitter.emit_br(header,
                    emitter.builder().operand(
                        ir::Operand::from_register(next_reg, usize_ty)));
    emitter.clear_loop_targets();
    if (!body_ok) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    emitter.set_cur(exit_b);
    return emitter.void_operand();
  }

  // A range loop is a real IR loop: the index travels in a block param,
  // the back edge spends the shared quota, and break/continue keep their
  // ordinary targets. The cap is enforced while materializing the range
  // and again after collection.
  const ir::TypeIdx index_ty = elements[0].type;
  const ir::TypeTag index_tag = emitter.builder().state().types[index_ty].tag;
  const u64 first = elements[0].value.int_value & comp::mask(index_tag);
  const u64 after = elements.back().value.int_value + 1;
  const ir::BlockIdx header = emitter.new_block();
  const ir::BlockIdx step_b = emitter.new_block();
  const ir::BlockIdx body_b = emitter.new_block();
  const ir::BlockIdx exit_b = emitter.new_block();
  const ir::RegisterIdx index_reg = emitter.claim_reg(index_ty);
  emitter.set_block_params(header, emitter.block_param(index_ty, index_reg));

  const auto index_operand = [&](u64 value) {
    const ir::ImmutableIdx imm = emitter.builder().immutable(
        {.type = index_ty, .data = {.u64_value = value}});
    return emitter.builder().operand(
        ir::Operand::from_immutable(imm, index_ty));
  };
  const ir::OperandIdx initial = index_operand(first);
  if (!emitter.terminated()) {
    emitter.emit_br(header, initial);
  }

  emitter.set_cur(header);
  const ir::OperandIdx current = emitter.builder().operand(
      ir::Operand::from_register(index_reg, index_ty));
  const ir::OperandIdx limit = index_operand(after);
  const ir::RegisterIdx cond_reg =
      emitter.claim_reg(emitter.builder().primitive(ir::TypeTag::I1));
  ir::OperandSeq cmp_ops;
  cmp_ops.push(current);
  cmp_ops.push(limit);
  const ir::InstructionIdx cmp =
      emitter.builder().instr({.op = ir::Opcode::Lt,
                               .flags = {},
                               .dst = cond_reg,
                               .measure = ir::TypeIdx::invalid(),
                               .operands = cmp_ops.finish()});
  emitter.builder().reg(
      {.type = emitter.builder().primitive(ir::TypeTag::I1), .def_idx = cmp});
  emitter.cur().push(cmp);
  emitter.emit_cond_br(
      emitter.builder().operand(ir::Operand::from_register(
          cond_reg, emitter.builder().primitive(ir::TypeTag::I1))),
      body_b, exit_b);

  emitter.set_loop_targets(exit_b, step_b);
  emitter.set_cur(body_b);
  if (bind_element) {
    emitter.bind_value(var_name, current);
  }
  const ir::OperandIdx body_val = emit_block(
      ast, body_block, emitter.builder().primitive(ir::TypeTag::Void), emitter,
      ctx, params);
  const bool body_ok = body_val.is_valid();
  if (body_ok && !emitter.terminated()) {
    emitter.emit_br(step_b, ir::OperandIdx(base::INVALID_IDX));
  }

  emitter.set_cur(step_b);
  const ir::OperandIdx step_current = emitter.builder().operand(
      ir::Operand::from_register(index_reg, index_ty));
  const ir::OperandIdx one = index_operand(1);
  const ir::RegisterIdx next_reg = emitter.claim_reg(index_ty);
  ir::OperandSeq add_ops;
  add_ops.push(step_current);
  add_ops.push(one);
  const ir::InstructionIdx add =
      emitter.builder().instr({.op = ir::Opcode::IntAdd,
                               .flags = {},
                               .dst = next_reg,
                               .measure = index_ty,
                               .operands = add_ops.finish()});
  emitter.builder().reg({.type = index_ty, .def_idx = add});
  emitter.cur().push(add);
  emitter.emit_br(header, emitter.builder().operand(
                              ir::Operand::from_register(next_reg, index_ty)));
  emitter.clear_loop_targets();
  if (!body_ok) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  emitter.set_cur(exit_b);
  return emitter.void_operand();
}
ir::OperandIdx emit_cast(const ast::AstArena& ast,
                         const ast::ExprCast& cast,
                         ir::TypeIdx type,
                         ConstEmitter& emitter,
                         const EmitContext& ctx,
                         const ParamEnv* params) {
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ir::OperandIdx inner =
      emit_expr(ast, cast.inner, expr_type(cast.inner), emitter, ctx, params);
  if (!inner.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::TypeTag target_tag = emitter.builder().state().types[type].tag;
  const ir::TypeIdx inner_type = expr_type(cast.inner);
  const ir::TypeTag inner_tag = emitter.builder().state().types[inner_type].tag;
  // A cast of a compile-time scalar folds into an immutable; emitting
  // an opcode here would leave a register the aggregate reader cannot
  // read back as a scalar.
  if (target_tag != ir::TypeTag::I1 && ir::is_integer_type(inner_tag) &&
      ir::is_integer_type(target_tag)) {
    u64 from = 0;
    if (operand_int(emitter, inner, from)) {
      u64 bits = from & comp::mask(inner_tag);
      if (comp::is_signed(inner_tag)) {
        const i64 signed_value = comp::sign_extend(bits, inner_tag);
        bits = static_cast<u64>(signed_value) & comp::mask(target_tag);
      } else {
        bits &= comp::mask(target_tag);
      }
      comp::CompVal folded;
      folded.type = type;
      folded.value.tag = comp::CompValue::Tag::Int;
      folded.value.int_value = bits;
      return emitter.add_value(folded, type);
    }
  }
  ir::OperandSeq operands;
  operands.push(emitter.builder().operand(
      ir::Operand{ir::Operand{emitter.builder().state().operands[inner.idx]}}));
  const ir::RegisterIdx dst(emitter.builder().state().registers.size());
  const ir::Opcode op =
      target_tag == ir::TypeTag::I1 ? ir::Opcode::Ne : ir::Opcode::TypeCast;
  if (op == ir::Opcode::Ne) {
    const ir::TypeIdx inner_type = expr_type(cast.inner);
    const ir::ImmutableIdx zero = emitter.builder().immutable(
        {.type = inner_type, .data = {.u64_value = 0}});
    operands.push(emitter.builder().operand(
        ir::Operand::from_immutable(zero, inner_type)));
  }
  const ir::InstructionIdx instr =
      emitter.builder().instr({.op = op,
                               .flags = {},
                               .dst = dst,
                               .measure = ir::TypeIdx::invalid(),
                               .operands = operands.finish()});
  emitter.builder().reg({.type = type, .def_idx = instr});
  emitter.cur().push(instr);
  return emitter.builder().operand(ir::Operand::from_register(dst, type));
}

// Matches a pattern against a fully compile-time value. Used when the
// scrutinee is a semantic CompVal (enum, struct, tuple, literal) rather
// than a lowered register, so the branch can be resolved without IR
// control flow. `bind` receives each payload name and its operand.
template <typename Bind>
bool semantic_pattern_matches(const ast::AstArena& ast,
                              ast::PatternIdx pattern,
                              const comp::CompVal& value,
                              ConstEmitter& emitter,
                              const EmitContext& ctx,
                              const ParamEnv* params,
                              Bind&& bind) {
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::PatternNode& pat = ast.patterns[pattern];
  switch (pat.kind) {
    case ast::PatternKind::Wildcard: return true;
    case ast::PatternKind::Ident:
    case ast::PatternKind::MutIdent: {
      const std::string_view name = pat.kind == ast::PatternKind::Ident
                                        ? pat.payload.ident.name.name
                                        : pat.payload.mut_ident.name.name;
      const ir::OperandIdx op = emitter.add_value(value, value.type);
      bind(name, op);
      return true;
    }
    case ast::PatternKind::Range: {
      if (value.value.tag != comp::CompValue::Tag::Int) {
        return false;
      }
      const ast::PatternRange& range = pat.payload.range;
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
      const ast::Literal& lit = ast.literals[pat.payload.literal.value];
      if (lit.kind == ast::LiteralKind::Bool) {
        return value.value.tag == comp::CompValue::Tag::Bool &&
               value.value.bool_value == (lit.spelling == "true");
      }
      if (lit.kind == ast::LiteralKind::Integer) {
        if (value.value.tag != comp::CompValue::Tag::Int) {
          return false;
        }
        u64 expected = comp::parse_numeric_value(lit.spelling);
        if (lit.is_negative) {
          expected = 0 - expected;
        }
        const ir::TypeTag tag = emitter.builder().state().types[value.type].tag;
        return value.value.int_value == (expected & comp::mask(tag));
      }
      return false;
    }
    case ast::PatternKind::Tuple: {
      if (value.value.tag != comp::CompValue::Tag::Enum ||
          !pat.payload.tuple.path.is_valid()) {
        return false;
      }
      const std::string_view name =
          ast.paths[pat.payload.tuple.path].segments.back().name;
      u32 variant = 0;
      if (!emitter.resolve_variant_index(value.type, name, variant) ||
          variant != value.value.variant) {
        return false;
      }
      if (pat.payload.tuple.elements.size() != value.value.fields.size()) {
        return false;
      }
      const ir::EnumType& shape =
          emitter.builder().state().enum_types
              [emitter.builder().state().types[value.type].as_enum()];
      const ir::EnumVariantTypeIdx vidx(shape.variants.head().idx + variant);
      const ir::EnumVariantType& vshape =
          emitter.builder().state().enum_variant_types[vidx];
      for (usize i = 0; i < pat.payload.tuple.elements.size(); ++i) {
        comp::CompVal field;
        field.value = value.value.fields[i];
        field.type = i < vshape.fields.size() ? vshape.fields[i]
                                              : ir::TypeIdx(base::INVALID_IDX);
        if (!semantic_pattern_matches(ast, pat.payload.tuple.elements[i], field,
                                      emitter, ctx, params, bind)) {
          return false;
        }
      }
      return true;
    }
    default: return false;
  }
}

// Inlines a comp fn body into the caller. `args` lists the actual
// arguments (for a method call the receiver is the first argument,
// binding `self`). Parameter names are bound to the lowered arguments.
static ir::OperandIdx emit_comp_inline(const ast::AstArena& ast,
                                       ast::ExprIdx expr,
                                       const std::vector<ast::ExprIdx>& args,
                                       ConstEmitter& emitter,
                                       const EmitContext& ctx,
                                       const ParamEnv* params) {
  [[maybe_unused]] const ast::ExprNode& node = ast.exprs[expr];
  const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  const EmitContext::CompFn& resolve_comp_fn = ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  // A comp fn call is inlined: lower the callee body's value in the
  // caller, with parameter names bound to the lowered arguments.
  CompFnInfo info;
  if (!resolve_comp_fn(expr, info) || !info.body.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const EmitContext callee_ctx = ctx.with_site({info.module, info.inst});
  const bool arity_ok = info.variadic ? args.size() >= info.params.size() - 1
                                      : args.size() == info.params.size();
  if (!arity_ok || info.params.size() != info.param_types.size() ||
      !emitter.enter_inline()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  ParamEnv callee_env;
  const usize fixed_count =
      info.variadic ? info.params.size() - 1 : info.params.size();
  for (usize i = 0; i < args.size(); ++i) {
    ir::OperandIdx arg = ir::OperandIdx(base::INVALID_IDX);
    if (i == 0 && info.is_method &&
        ast.exprs[args[i]].kind == ast::ExprKind::Path) {
      const auto& segs =
          ast.paths[ast.exprs[args[i]].payload.get<ast::ExprPath>().idx]
              .segments;
      if (segs.size() == 1) {
        if (const LocalBinding* local = emitter.lookup_local(segs[0].name)) {
          arg = local->addr;
        }
      }
    }
    if (!arg.is_valid()) {
      arg = emit_expr(ast, args[i], expr_type(args[i]), emitter, ctx, params);
    }
    if (!arg.is_valid()) {
      emitter.leave_inline();
      return ir::OperandIdx(base::INVALID_IDX);
    }
    if (i < fixed_count) {
      const ast::PatternNode& pat = ast.patterns[info.params[i]];
      if (pat.kind == ast::PatternKind::Ident ||
          pat.kind == ast::PatternKind::MutIdent) {
        callee_env.emplace_back(pat.kind == ast::PatternKind::Ident
                                    ? pat.payload.ident.name.name
                                    : pat.payload.mut_ident.name.name,
                                arg);
      }
    }
  }
  if (info.variadic && fixed_count < info.params.size()) {
    comp::CompVal shape;
    shape.value.tag = comp::CompValue::Tag::Variadic;
    shape.value.element = info.element;
    for (usize i = fixed_count; i < args.size(); ++i) {
      shape.value.slot_types.push_back(expr_type(args[i]));
    }
    const ast::PatternNode& pat = ast.patterns[info.params[fixed_count]];
    if (pat.kind == ast::PatternKind::Ident ||
        pat.kind == ast::PatternKind::MutIdent) {
      emitter.bind_value(
          pat.kind == ast::PatternKind::Ident ? pat.payload.ident.name.name
                                              : pat.payload.mut_ident.name.name,
          emitter.add_semantic(shape, info.param_types[fixed_count]));
    }
  }
  const ast::Block& body = ast.blocks[info.body];
  if (!body.value.is_valid()) {
    emitter.leave_inline();
    return ir::OperandIdx(base::INVALID_IDX);
  }
  // Each inlined callee gets its own return block: a `ret` in the
  // callee branches there, and the block's parameter is the call's
  // value so evaluation resumes in the caller.
  const ir::BlockIdx callee_return = emitter.new_block();
  const ir::RegisterIdx callee_result = emitter.claim_reg(info.ret);
  emitter.set_block_params(callee_return,
                           emitter.block_param(info.ret, callee_result));
  emitter.push_return_target(callee_return, callee_result);
  const usize saved_bindings = emitter.value_bindings_size();
  const ir::OperandIdx body_value =
      emit_block(ast, body, info.ret, emitter, callee_ctx, &callee_env);
  emitter.restore_value_bindings(saved_bindings);
  emitter.pop_return_target();
  if (!body_value.is_valid()) {
    emitter.set_cur(callee_return);
    emitter.leave_inline();
    return ir::OperandIdx(base::INVALID_IDX);
  }
  emitter.set_cur(callee_return);
  emitter.leave_inline();
  const ir::OperandIdx result = emitter.builder().operand(
      ir::Operand::from_register(callee_result, info.ret));
  if (emitter.builder().state().types[info.ret].tag == ir::TypeTag::Enum) {
    emitter.clear_semantic(result);
  }
  return result;
}

static ir::OperandIdx emit_return_expr(const ast::AstArena& ast,
                                       [[maybe_unused]] ast::ExprIdx expr,
                                       ir::TypeIdx type,
                                       ConstEmitter& emitter,
                                       const EmitContext& ctx,
                                       const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprReturn& ret = node.payload.get<ast::ExprReturn>();
  if (!ret.value.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::TypeIdx value_type = expr_type(ret.value);
  const ir::OperandIdx val =
      emit_expr(ast, ret.value, value_type, emitter, ctx, params);
  if (!val.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (emitter.return_target().is_valid()) {
    ir::OperandIdx passed = val;
    if (const comp::CompVal* sem = emitter.get_semantic(val)) {
      const comp::CompVal sem_copy = *sem;
      emitter.tag_register(emitter.return_reg(), sem_copy);
      // A semantic scalar rides as a CompValue register, which the
      // interpreter cannot read as a scalar once it crosses a return
      // block parameter. Hand the block a real immutable instead;
      // the register still carries the semantic value for callers.
      if (sem_copy.value.tag == comp::CompValue::Tag::Int ||
          sem_copy.value.tag == comp::CompValue::Tag::Bool) {
        const ir::TypeTag rtag =
            emitter.builder().state().types[value_type].tag;
        const u64 bits = sem_copy.value.tag == comp::CompValue::Tag::Bool
                             ? (sem_copy.value.bool_value ? 1u : 0u)
                             : (sem_copy.value.int_value & comp::mask(rtag));
        const ir::ImmutableIdx imm = emitter.builder().immutable(
            {.type = value_type, .data = {.u64_value = bits}});
        passed = emitter.builder().operand(
            ir::Operand::from_immutable(imm, value_type));
      }
    }
    emitter.emit_br(emitter.return_target(), passed);
    emitter.set_terminated();
    return emitter.builder().operand(
        ir::Operand::from_register(emitter.return_reg(), type));
  }
  return val;
}

static ir::OperandIdx emit_literal_expr(
    const ast::AstArena& ast,
    [[maybe_unused]] ast::ExprIdx expr,
    ir::TypeIdx type,
    ConstEmitter& emitter,
    [[maybe_unused]] const EmitContext& ctx,
    [[maybe_unused]] const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::Literal& literal =
      ast.literals[node.payload.get<ast::ExprLiteral>().value];
  if (literal.kind == ast::LiteralKind::String) {
    comp::CompVal value;
    value.value.tag = comp::CompValue::Tag::Str;
    const std::string decoded = comp::unescape(literal.spelling);
    const std::optional<str::StringPoolId> id =
        ctx.evaluator.strings().intern_copied(decoded);
    if (!id.has_value()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    value.value.str_id = *id;
    value.type = type;
    return emitter.add_semantic(value, type);
  }
  if (literal.kind != ast::LiteralKind::Integer &&
      literal.kind != ast::LiteralKind::Bool) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  u64 bits = 0;
  if (literal.kind == ast::LiteralKind::Integer) {
    bits = comp::parse_numeric_value(literal.spelling);
    if (literal.is_negative) {
      bits = 0 - bits;
    }
  } else {
    bits = literal.spelling == "true" ? 1 : 0;
  }
  const ir::TypeTag tag = emitter.builder().state().types[type].tag;
  const ir::ImmutableIdx imm = emitter.builder().immutable(
      {.type = type, .data = {.u64_value = bits & comp::mask(tag)}});
  return emitter.builder().operand(ir::Operand::from_immutable(imm, type));
}

static ir::OperandIdx emit_call_expr(const ast::AstArena& ast,
                                     ast::ExprIdx expr,
                                     ir::TypeIdx type,
                                     ConstEmitter& emitter,
                                     const EmitContext& ctx,
                                     const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  const EmitContext::LayoutType& resolve_layout_type = ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
  const ast::ExprNode& callee = ast.exprs[call.callee];
  if (callee.kind == ast::ExprKind::Path) {
    const ast::PathIdx cpath = callee.payload.get<ast::ExprPath>().idx;
    const auto& segs = ast.paths[cpath].segments;
    if (!segs.empty()) {
      const std::string_view cname = segs.back().name;
      // An enum variant constructor such as `Grade::Point(mark)` is a
      // call whose result type is the enum. Resolve the variant by
      // name and build the value from its evaluated payload fields.
      if (emitter.builder().state().types[type].tag == ir::TypeTag::Enum) {
        u32 variant = 0;
        if (emitter.resolve_variant_index(type, cname, variant)) {
          std::vector<ir::OperandIdx> arg_ops;
          bool runtime_fields = false;
          for (ast::ExprIdx arg : call.args) {
            const ir::TypeIdx arg_ty = expr_type(arg);
            const ir::OperandIdx arg_op =
                emit_expr(ast, arg, arg_ty, emitter, ctx, params);
            if (!arg_op.is_valid()) {
              return ir::OperandIdx(base::INVALID_IDX);
            }
            arg_ops.push_back(arg_op);
            if (emitter.get_semantic(arg_op) == nullptr) {
              u64 bits = 0;
              if (!operand_int(emitter, arg_op, bits)) {
                runtime_fields = true;
              }
            }
          }
          if (runtime_fields) {
            const ir::TypeIdx usize_ty =
                emitter.builder().primitive(ir::TypeTag::Usize);
            const ir::OperandIdx tag =
                emitter.builder().operand(ir::Operand::from_immutable(
                    emitter.builder().immutable(
                        {.type = usize_ty, .data = {.u64_value = variant}}),
                    usize_ty));
            ir::OperandSeq ops;
            ops.push(emitter.builder().operand(
                ir::Operand{emitter.builder().state().operands[tag.idx]}));
            for (ir::OperandIdx op : arg_ops) {
              ops.push(emitter.builder().operand(
                  ir::Operand{emitter.builder().state().operands[op.idx]}));
            }
            const ir::RegisterIdx dst(
                emitter.builder().state().registers.size());
            const ir::InstructionIdx instr =
                emitter.builder().instr({.op = ir::Opcode::CompMakeVariant,
                                         .flags = {},
                                         .dst = dst,
                                         .measure = ir::TypeIdx::invalid(),
                                         .operands = ops.finish()});
            emitter.builder().reg({.type = type, .def_idx = instr});
            emitter.cur().push(instr);
            return emitter.builder().operand(
                ir::Operand::from_register(dst, type));
          }
          comp::CompVal built;
          built.type = type;
          built.value.tag = comp::CompValue::Tag::Enum;
          built.value.variant = variant;
          for (ir::OperandIdx arg_op : arg_ops) {
            comp::CompValue field;
            if (const comp::CompVal* sem = emitter.get_semantic(arg_op)) {
              field = sem->value;
            } else {
              u64 bits = 0;
              if (!operand_int(emitter, arg_op, bits)) {
                return ir::OperandIdx(base::INVALID_IDX);
              }
              const ir::TypeIdx arg_ty =
                  emitter.builder().state().operands[arg_op.idx].type;
              field.tag =
                  emitter.builder().state().types[arg_ty].tag == ir::TypeTag::I1
                      ? comp::CompValue::Tag::Bool
                      : comp::CompValue::Tag::Int;
              field.int_value = bits;
              field.bool_value = bits != 0;
            }
            built.value.fields.push_back(std::move(field));
          }
          return emitter.add_semantic(built, type);
        }
      }
      if (cname == "str_len" || cname == "str_byte" || cname == "str_slice") {
        return emit_str_intrinsic(ast, call, cname, type, emitter, ctx, params);
      }
      if (cname == "comp_pieces") {
        if (call.args.size() != 1) {
          return ir::OperandIdx(base::INVALID_IDX);
        }
        std::string format_bytes;
        const ir::TypeIdx arg_ty = expr_type(call.args[0]);
        const ir::OperandIdx arg_op =
            emit_expr(ast, call.args[0], arg_ty, emitter, ctx, params);
        if (!arg_op.is_valid()) {
          return ir::OperandIdx(base::INVALID_IDX);
        }
        const comp::CompVal* arg_sem = emitter.get_semantic(arg_op);
        if (arg_sem == nullptr ||
            arg_sem->value.tag != comp::CompValue::Tag::Str) {
          return ir::OperandIdx(base::INVALID_IDX);
        }
        format_bytes = std::string(
            comp::string_bytes(arg_sem->value, ctx.evaluator.strings()));
        const analyzer::FmtTemplate parsed =
            analyzer::parse_format_string(format_bytes);
        if (parsed.error != analyzer::FmtError::None) {
          return ir::OperandIdx(base::INVALID_IDX);
        }
        comp::CompVal value;
        value.type = type;
        value.value.tag = comp::CompValue::Tag::Array;
        for (const analyzer::FmtPiece& piece : parsed.pieces) {
          comp::CompValue item;
          item.tag = comp::CompValue::Tag::Struct;
          item.fields.resize(5);
          item.fields[0].tag = comp::CompValue::Tag::Bool;
          item.fields[0].bool_value = piece.is_arg;
          item.fields[1].tag = comp::CompValue::Tag::Str;
          item.fields[1].owned_str = piece.literal;
          item.fields[2].tag = comp::CompValue::Tag::Int;
          item.fields[2].int_value = piece.precision;
          item.fields[3].tag = comp::CompValue::Tag::Int;
          item.fields[3].int_value = piece.arg;
          item.fields[4].tag = comp::CompValue::Tag::Bool;
          item.fields[4].bool_value = piece.explicit_float;
          value.value.fields.push_back(std::move(item));
        }
        return emitter.add_semantic(value, type);
      }
      if (cname == "wrapping_add" || cname == "wrapping_sub" ||
          cname == "wrapping_mul") {
        if (call.args.size() != 2) {
          return ir::OperandIdx(base::INVALID_IDX);
        }
        const ir::OperandIdx lhs = emit_expr(
            ast, call.args[0], expr_type(call.args[0]), emitter, ctx, params);
        const ir::OperandIdx rhs = emit_expr(
            ast, call.args[1], expr_type(call.args[1]), emitter, ctx, params);
        if (!lhs.is_valid() || !rhs.is_valid()) {
          return ir::OperandIdx(base::INVALID_IDX);
        }
        ir::OperandSeq operands;
        operands.push(emitter.builder().operand(ir::Operand{
            ir::Operand{emitter.builder().state().operands[lhs.idx]}}));
        operands.push(emitter.builder().operand(ir::Operand{
            ir::Operand{emitter.builder().state().operands[rhs.idx]}}));
        const ir::RegisterIdx dst(emitter.builder().state().registers.size());
        const ir::Opcode op = cname == "wrapping_add" ? ir::Opcode::IntAddWrap
                              : cname == "wrapping_sub"
                                  ? ir::Opcode::IntSubWrap
                                  : ir::Opcode::IntMulWrap;
        const ir::InstructionIdx instr =
            emitter.builder().instr({.op = op,
                                     .flags = {},
                                     .dst = dst,
                                     .measure = ir::TypeIdx::invalid(),
                                     .operands = operands.finish()});
        emitter.builder().reg({.type = type, .def_idx = instr});
        emitter.cur().push(instr);
        return emitter.builder().operand(ir::Operand::from_register(dst, type));
      }
      if (cname == "size_of" || cname == "align_of") {
        const ir::TypeIdx measured = resolve_layout_type(expr);
        if (!measured.is_valid()) {
          return ir::OperandIdx(base::INVALID_IDX);
        }
        const ir::RegisterIdx dst(emitter.builder().state().registers.size());
        const ir::InstructionIdx instr = emitter.builder().instr(
            {.op = cname == "size_of" ? ir::Opcode::TypeSizeOf
                                      : ir::Opcode::TypeAlignOf,
             .flags = {},
             .dst = dst,
             .measure = measured,
             .operands = {}});
        emitter.builder().reg({.type = type, .def_idx = instr});
        emitter.cur().push(instr);
        return emitter.builder().operand(ir::Operand::from_register(dst, type));
      }
    }
  }
  const std::vector<ast::ExprIdx> call_args(call.args.begin(), call.args.end());
  return emit_comp_inline(ast, expr, call_args, emitter, ctx, params);
}

static ir::OperandIdx emit_path_expr(const ast::AstArena& ast,
                                     [[maybe_unused]] ast::ExprIdx expr,
                                     ir::TypeIdx type,
                                     ConstEmitter& emitter,
                                     [[maybe_unused]] const EmitContext& ctx,
                                     const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  const EmitContext::PathValue& resolve_path = ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::PathIdx path = node.payload.get<ast::ExprPath>().idx;
  const auto& segments = ast.paths[path].segments;
  if (segments.size() == 1) {
    const ir::OperandIdx rebound = emitter.lookup_value(segments[0].name);
    if (rebound.is_valid()) {
      return rebound;
    }
  }
  if (params != nullptr) {
    if (segments.size() == 1) {
      for (const auto& [name, operand] : *params) {
        if (name == segments[0].name) {
          return operand;
        }
      }
    }
  }
  const ast::PathIdx path_idx = node.payload.get<ast::ExprPath>().idx;
  const std::span<const ast::Ident> path_segs = ast.paths[path_idx].segments;
  // A unit variant such as `Grade::Pass` is a path whose result type
  // is the enum. Resolve the last segment as a variant name.
  if (emitter.builder().state().types[type].tag == ir::TypeTag::Enum &&
      path_segs.size() >= 2) {
    u32 variant = 0;
    if (emitter.resolve_variant_index(type, path_segs.back().name, variant)) {
      comp::CompVal built;
      built.type = type;
      built.value.tag = comp::CompValue::Tag::Enum;
      built.value.variant = variant;
      return emitter.add_semantic(built, type);
    }
  }
  if (path_segs.size() == 1) {
    const ir::OperandIdx bound = emitter.lookup_value(path_segs[0].name);
    if (bound.is_valid()) {
      return bound;
    }
    if (const LocalBinding* local = emitter.lookup_local(path_segs[0].name)) {
      const ir::RegisterIdx loaded(emitter.builder().state().registers.size());
      const ir::InstructionIdx instr = emitter.builder().instr(
          {.op = ir::Opcode::Load,
           .flags = {},
           .dst = loaded,
           .measure = local->type,
           .operands = ir::OperandIdxRange{local->addr, 1}});
      emitter.builder().reg({.type = local->type, .def_idx = instr});
      emitter.cur().push(instr);
      return emitter.builder().operand(
          ir::Operand::from_register(loaded, local->type));
    }
  }
  comp::CompVal resolved;
  if (!resolve_path(path, resolved)) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (resolved.value.tag != comp::CompValue::Tag::Int &&
      resolved.value.tag != comp::CompValue::Tag::Bool) {
    return emitter.add_semantic(resolved, type);
  }
  const ir::TypeTag tag = emitter.builder().state().types[type].tag;
  const ir::ImmutableIdx imm = emitter.builder().immutable(
      {.type = type,
       .data = {.u64_value = (resolved.value.tag == comp::CompValue::Tag::Bool
                                  ? (resolved.value.bool_value ? 1u : 0u)
                                  : resolved.value.int_value) &
                             comp::mask(tag)}});
  return emitter.builder().operand(ir::Operand::from_immutable(imm, type));
}

// Folds a comparison on two values of the same type. Returns false for
// non-comparison operators so callers that have not already restricted the
// operator can refuse instead of folding a wrong answer.
template <typename T>
static bool fold_compare(ast::BinaryOp op,
                         const T& a,
                         const T& b,
                         bool& answer) {
  switch (op) {
    case ast::BinaryOp::Eq: answer = a == b; return true;
    case ast::BinaryOp::NotEq: answer = a != b; return true;
    case ast::BinaryOp::Lt: answer = a < b; return true;
    case ast::BinaryOp::LtEq: answer = a <= b; return true;
    case ast::BinaryOp::Gt: answer = a > b; return true;
    case ast::BinaryOp::GtEq: answer = a >= b; return true;
    default: return false;
  }
}

static ir::OperandIdx emit_binary_expr(const ast::AstArena& ast,
                                       [[maybe_unused]] ast::ExprIdx expr,
                                       ir::TypeIdx type,
                                       ConstEmitter& emitter,
                                       const EmitContext& ctx,
                                       const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprBinary& bin = node.payload.get<ast::ExprBinary>();
  const ir::OperandIdx lhs =
      emit_expr(ast, bin.lhs, expr_type(bin.lhs), emitter, ctx, params);
  if (!lhs.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  // Logical operators short-circuit: if the left operand is a
  // compile-time boolean that decides the result, never emit RHS.
  if (bin.op == ast::BinaryOp::And || bin.op == ast::BinaryOp::Or) {
    bool l = false;
    if (operand_bool(emitter, lhs, l)) {
      const bool decides = bin.op == ast::BinaryOp::And ? !l : l;
      if (decides) {
        return bool_immutable(emitter, type, l);
      }
    }
  }
  const ir::OperandIdx rhs =
      emit_expr(ast, bin.rhs, expr_type(bin.rhs), emitter, ctx, params);
  if (!rhs.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (bin.op == ast::BinaryOp::And || bin.op == ast::BinaryOp::Or) {
    bool l = false;
    bool r = false;
    if (operand_bool(emitter, lhs, l) && operand_bool(emitter, rhs, r)) {
      const bool answer = bin.op == ast::BinaryOp::And ? (l && r) : (l || r);
      return bool_immutable(emitter, type, answer);
    }
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const comp::CompVal* lhs_sem = emitter.get_semantic(lhs);
  const comp::CompVal* rhs_sem = emitter.get_semantic(rhs);
  if (lhs_sem != nullptr && rhs_sem != nullptr &&
      lhs_sem->value.tag == comp::CompValue::Tag::Str &&
      rhs_sem->value.tag == comp::CompValue::Tag::Str) {
    bool answer = false;
    const std::string_view a =
        comp::string_bytes(lhs_sem->value, ctx.evaluator.strings());
    const std::string_view b =
        comp::string_bytes(rhs_sem->value, ctx.evaluator.strings());
    if (!fold_compare(bin.op, a, b, answer)) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const ir::ImmutableIdx imm = emitter.builder().immutable(
        {.type = type, .data = {.i1_value = answer}});
    return emitter.builder().operand(ir::Operand::from_immutable(imm, type));
  }
  // Guard expressions compare two compile-time integers; fold them
  // into an immutable so the match arm can read the answer without
  // running IR control flow.
  switch (bin.op) {
    case ast::BinaryOp::Eq:
    case ast::BinaryOp::NotEq:
    case ast::BinaryOp::Lt:
    case ast::BinaryOp::LtEq:
    case ast::BinaryOp::Gt:
    case ast::BinaryOp::GtEq: {
      const ir::TypeIdx operand_ty = expr_type(bin.lhs);
      const ir::TypeTag otag = emitter.builder().state().types[operand_ty].tag;
      if (ir::is_integer_type(otag) || otag == ir::TypeTag::I1) {
        u64 l = 0;
        u64 r = 0;
        if (operand_int(emitter, lhs, l) && operand_int(emitter, rhs, r)) {
          const u64 mask = comp::mask(otag);
          l &= mask;
          r &= mask;
          bool answer = false;
          if (comp::is_signed(otag)) {
            const i64 sl = comp::sign_extend(l, otag);
            const i64 sr = comp::sign_extend(r, otag);
            fold_compare(bin.op, sl, sr, answer);
          } else {
            fold_compare(bin.op, l, r, answer);
          }
          const ir::ImmutableIdx imm = emitter.builder().immutable(
              {.type = type, .data = {.i1_value = answer}});
          return emitter.builder().operand(
              ir::Operand::from_immutable(imm, type));
        }
      }
      break;
    }
    default: break;
  }
  if (bin.op == ast::BinaryOp::Add || bin.op == ast::BinaryOp::Sub ||
      bin.op == ast::BinaryOp::Mul) {
    const ir::TypeIdx operand_ty = expr_type(bin.lhs);
    const ir::TypeTag otag = emitter.builder().state().types[operand_ty].tag;
    if (ir::is_integer_type(otag)) {
      u64 l = 0;
      u64 r = 0;
      if (operand_int(emitter, lhs, l) && operand_int(emitter, rhs, r)) {
        const u64 mask = comp::mask(otag);
        l &= mask;
        r &= mask;
        u64 result = 0;
        bool overflow = false;
        switch (bin.op) {
          case ast::BinaryOp::Add:
            result = (l + r) & mask;
            overflow = comp::add_overflows(l, r, otag);
            break;
          case ast::BinaryOp::Sub:
            result = (l - r) & mask;
            overflow = comp::sub_overflows(l, r, otag);
            break;
          case ast::BinaryOp::Mul:
            result = (l * r) & mask;
            overflow = comp::mul_overflows(l, r, otag);
            break;
          default: break;
        }
        if (!overflow) {
          comp::CompVal folded;
          folded.type = operand_ty;
          folded.value.tag = comp::CompValue::Tag::Int;
          folded.value.int_value = result;
          return emitter.add_value(folded, operand_ty);
        }
      }
    }
  }
  const ir::TypeTag tag = emitter.builder().state().types[type].tag;
  if (bin.op == ast::BinaryOp::Pow) {
    // Both operands are compile-time constants; compute the result
    // directly so overflow can be reported as a source error.
    const ir::Operand& lop = emitter.builder().state().operands[lhs.idx];
    const ir::Operand& rop = emitter.builder().state().operands[rhs.idx];
    if (lop.is<ir::ImmutableIdx>() && rop.is<ir::ImmutableIdx>()) {
      u64 exponent = emitter.builder()
                         .state()
                         .immutables[rop.as_immutable()]
                         .data.u64_value;
      u64 result = 1;
      u64 base = emitter.builder()
                     .state()
                     .immutables[lop.as_immutable()]
                     .data.u64_value;
      const u64 mask = comp::mask(tag);
      bool overflow = false;
      while (exponent != 0) {
        if ((exponent & 1) != 0) {
          if (comp::mul_overflows(result, base, tag)) {
            overflow = true;
            break;
          }
          result = (result * base) & mask;
        }
        exponent >>= 1;
        if (exponent != 0) {
          if (comp::mul_overflows(base, base, tag)) {
            overflow = true;
            break;
          }
          base = (base * base) & mask;
        }
      }
      if (overflow) {
        emitter.set_emit_error(ConstEmitter::EmitError::Overflow);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      return emitter.builder().operand(ir::Operand::from_immutable(
          emitter.builder().immutable(
              {.type = type, .data = {.u64_value = result}}),
          type));
    }
    return ir::OperandIdx(base::INVALID_IDX);
  }
  ir::Opcode op = ir::Opcode::Noop;
  switch (bin.op) {
    case ast::BinaryOp::Add: op = ir::Opcode::IntAdd; break;
    case ast::BinaryOp::Sub: op = ir::Opcode::IntSub; break;
    case ast::BinaryOp::Mul: op = ir::Opcode::IntMul; break;
    case ast::BinaryOp::Div:
      op = ir::is_unsigned_integer_type(tag) ? ir::Opcode::UintDiv
                                             : ir::Opcode::IntDiv;
      break;
    case ast::BinaryOp::Mod:
      op = ir::is_unsigned_integer_type(tag) ? ir::Opcode::UintRem
                                             : ir::Opcode::IntRem;
      break;
    case ast::BinaryOp::BitAnd: op = ir::Opcode::And; break;
    case ast::BinaryOp::BitOr: op = ir::Opcode::Or; break;
    case ast::BinaryOp::BitXor: op = ir::Opcode::Xor; break;
    case ast::BinaryOp::Shl: op = ir::Opcode::ShiftLeft; break;
    case ast::BinaryOp::Shr:
      op = ir::is_unsigned_integer_type(tag) ? ir::Opcode::LogicalShiftRight
                                             : ir::Opcode::ArithmeticShiftRight;
      break;
    case ast::BinaryOp::Eq: op = ir::Opcode::Eq; break;
    case ast::BinaryOp::NotEq: op = ir::Opcode::Ne; break;
    case ast::BinaryOp::Gt: op = ir::Opcode::Gt; break;
    case ast::BinaryOp::Lt: op = ir::Opcode::Lt; break;
    case ast::BinaryOp::GtEq: op = ir::Opcode::Ge; break;
    case ast::BinaryOp::LtEq: op = ir::Opcode::Le; break;
    default: return ir::OperandIdx(base::INVALID_IDX);
  }
  // Semantic scalars ride as non-scalar registers; fold them into
  // real immutables before feeding an opcode the interpreter reads.
  ir::OperandSeq operands;
  for (ir::OperandIdx operand : {lhs, rhs}) {
    if (const comp::CompVal* sem = emitter.get_semantic(operand)) {
      if (sem->value.tag == comp::CompValue::Tag::Int ||
          sem->value.tag == comp::CompValue::Tag::Bool) {
        const ir::TypeIdx operand_ty =
            operand == lhs ? expr_type(bin.lhs) : expr_type(bin.rhs);
        const u64 bits = sem->value.tag == comp::CompValue::Tag::Bool
                             ? (sem->value.bool_value ? 1u : 0u)
                             : sem->value.int_value;
        const ir::ImmutableIdx imm = emitter.builder().immutable(
            {.type = operand_ty, .data = {.u64_value = bits}});
        operands.push(emitter.builder().operand(
            ir::Operand::from_immutable(imm, operand_ty)));
        continue;
      }
    }
    operands.push(emitter.builder().operand(
        ir::Operand{emitter.builder().state().operands[operand.idx]}));
  }
  const ir::RegisterIdx dst(emitter.builder().state().registers.size());
  const ir::InstructionIdx instr =
      emitter.builder().instr({.op = op,
                               .flags = {},
                               .dst = dst,
                               .measure = ir::TypeIdx::invalid(),
                               .operands = operands.finish()});
  const ir::TypeIdx result_type =
      (op == ir::Opcode::Eq || op == ir::Opcode::Ne || op == ir::Opcode::Gt ||
       op == ir::Opcode::Lt || op == ir::Opcode::Ge || op == ir::Opcode::Le)
          ? emitter.builder().primitive(ir::TypeTag::I1)
          : type;
  emitter.builder().reg({.type = result_type, .def_idx = instr});
  emitter.cur().push(instr);
  return emitter.builder().operand(
      ir::Operand::from_register(dst, result_type));
}

static ir::OperandIdx emit_tuple_expr(const ast::AstArena& ast,
                                      [[maybe_unused]] ast::ExprIdx expr,
                                      ir::TypeIdx type,
                                      ConstEmitter& emitter,
                                      const EmitContext& ctx,
                                      const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprTuple& tup = node.payload.get<ast::ExprTuple>();
  const ir::TupleType& shape =
      emitter.builder()
          .state()
          .tuple_types[emitter.builder().state().types[type].as_tuple()];
  if (shape.elements.size() != tup.elements.size()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  comp::CompVal result;
  result.type = type;
  result.value.tag = comp::CompValue::Tag::Tuple;
  for (u32 i = 0; i < tup.elements.size(); ++i) {
    const ir::OperandIdx val = emit_expr(
        ast, tup.elements[i], shape.elements[i], emitter, ctx, params);
    if (!val.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    comp::CompValue field;
    if (const comp::CompVal* sem = emitter.get_semantic(val)) {
      field = sem->value;
    } else {
      const ir::Operand& operand = emitter.builder().state().operands[val.idx];
      if (!operand.is<ir::ImmutableIdx>()) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      const u64 bits = emitter.builder()
                           .state()
                           .immutables[operand.as_immutable()]
                           .data.u64_value;
      field.tag = emitter.builder().state().types[shape.elements[i]].tag ==
                          ir::TypeTag::I1
                      ? comp::CompValue::Tag::Bool
                      : comp::CompValue::Tag::Int;
      field.int_value = bits;
      field.bool_value = bits != 0;
    }
    result.value.fields.push_back(std::move(field));
  }
  return emitter.add_semantic(result, type);
}

static ir::OperandIdx emit_array_expr(const ast::AstArena& ast,
                                      [[maybe_unused]] ast::ExprIdx expr,
                                      ir::TypeIdx type,
                                      ConstEmitter& emitter,
                                      const EmitContext& ctx,
                                      const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprArray& arr = node.payload.get<ast::ExprArray>();
  const ir::ArrayType& shape =
      emitter.builder()
          .state()
          .array_types[emitter.builder().state().types[type].as_array()];
  std::vector<ast::ExprIdx> elements;
  if (arr.repeat.is_valid()) {
    elements.assign(shape.count, arr.repeat);
  } else {
    for (ast::ExprIdx element : arr.elements) {
      elements.push_back(element);
    }
  }
  if (elements.size() != shape.count) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  comp::CompVal result;
  result.type = type;
  result.value.tag = comp::CompValue::Tag::Array;
  for (ast::ExprIdx element : elements) {
    const ir::OperandIdx val =
        emit_expr(ast, element, shape.element, emitter, ctx, params);
    if (!val.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    comp::CompValue field;
    if (const comp::CompVal* sem = emitter.get_semantic(val)) {
      field = sem->value;
    } else {
      u64 bits = 0;
      if (!operand_int(emitter, val, bits)) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      field.tag =
          emitter.builder().state().types[shape.element].tag == ir::TypeTag::I1
              ? comp::CompValue::Tag::Bool
              : comp::CompValue::Tag::Int;
      field.int_value = bits;
      field.bool_value = bits != 0;
    }
    result.value.fields.push_back(std::move(field));
  }
  return emitter.add_semantic(result, type);
}

static ir::OperandIdx emit_struct_expr(const ast::AstArena& ast,
                                       [[maybe_unused]] ast::ExprIdx expr,
                                       ir::TypeIdx type,
                                       ConstEmitter& emitter,
                                       const EmitContext& ctx,
                                       const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  const EmitContext::FieldIndex& resolve_field_index = ctx.resolve_field_index;
  const ast::ExprStruct& strukt = node.payload.get<ast::ExprStruct>();
  if (strukt.base_expr.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::StructType& shape =
      emitter.builder()
          .state()
          .struct_types[emitter.builder().state().types[type].as_struct()];
  std::vector<comp::CompValue> fields(shape.fields.size());
  for (const ast::ExprFieldInit& field : strukt.init) {
    u32 index = 0;
    if (!resolve_field_index(type, field.name.name, index) ||
        index >= shape.fields.size()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    const ir::TypeIdx field_type = shape.fields[index];
    const ir::OperandIdx val =
        emit_expr(ast, field.value, field_type, emitter, ctx, params);
    if (!val.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    if (const comp::CompVal* sem = emitter.get_semantic(val)) {
      fields[index] = sem->value;
    } else {
      comp::CompValue f;
      f.tag = emitter.builder().state().types[field_type].tag == ir::TypeTag::I1
                  ? comp::CompValue::Tag::Bool
                  : comp::CompValue::Tag::Int;
      u64 bits = 0;
      if (!operand_int(emitter, val, bits)) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      f.int_value = bits;
      f.bool_value = bits != 0;
      fields[index] = std::move(f);
    }
  }
  comp::CompVal result;
  result.type = type;
  result.value.tag = comp::CompValue::Tag::Struct;
  result.value.fields = std::move(fields);
  return emitter.add_semantic(result, type);
}

static ir::OperandIdx emit_field_expr(const ast::AstArena& ast,
                                      [[maybe_unused]] ast::ExprIdx expr,
                                      ir::TypeIdx type,
                                      ConstEmitter& emitter,
                                      const EmitContext& ctx,
                                      const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  const EmitContext::FieldIndex& resolve_field_index = ctx.resolve_field_index;
  const ast::ExprField& field = node.payload.get<ast::ExprField>();
  ir::TypeIdx receiver_type = expr_type(field.receiver);
  ir::TypeTag receiver_tag = emitter.builder().state().types[receiver_type].tag;
  // A field through a reference reads the pointee's field.
  if (receiver_tag == ir::TypeTag::Ref || receiver_tag == ir::TypeTag::MutRef) {
    receiver_type =
        emitter.builder()
            .state()
            .ref_types[emitter.builder().state().types[receiver_type].as_ref()]
            .pointee;
    receiver_tag = emitter.builder().state().types[receiver_type].tag;
  }
  u32 index = 0;
  if (receiver_tag == ir::TypeTag::Tuple) {
    bool digits = !field.name.name.empty();
    for (char c : field.name.name) {
      if (c < '0' || c > '9') {
        digits = false;
        break;
      }
      index = index * 10 + static_cast<u32>(c - '0');
    }
    const ir::TupleType& shape =
        emitter.builder().state().tuple_types
            [emitter.builder().state().types[receiver_type].as_tuple()];
    if (!digits || index >= shape.elements.size()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
  } else if (receiver_tag == ir::TypeTag::Struct) {
    if (!resolve_field_index(receiver_type, field.name.name, index)) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
  } else {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  bool base_is_address = false;
  ir::OperandIdx base = ir::OperandIdx(base::INVALID_IDX);
  if (ast.exprs[field.receiver].kind == ast::ExprKind::Path ||
      ast.exprs[field.receiver].kind == ast::ExprKind::Field ||
      ast.exprs[field.receiver].kind == ast::ExprKind::Index) {
    base = emit_place_address(ast, field.receiver, emitter, ctx, params);
    base_is_address = base.is_valid();
    if (!base.is_valid()) {
      base =
          emit_expr(ast, field.receiver, receiver_type, emitter, ctx, params);
    }
  }
  if (!base.is_valid()) {
    base = emit_expr(ast, field.receiver, receiver_type, emitter, ctx, params);
  }
  if (!base.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (!base_is_address && emitter.get_semantic(base) == nullptr) {
    const ir::TypeIdx usize_ty =
        emitter.builder().primitive(ir::TypeTag::Usize);
    const ir::ImmutableIdx index_imm = emitter.builder().immutable(
        {.type = usize_ty, .data = {.u64_value = index}});
    ir::OperandSeq field_ops;
    field_ops.push(emitter.builder().operand(
        ir::Operand{emitter.builder().state().operands[base.idx]}));
    field_ops.push(emitter.builder().operand(
        ir::Operand::from_immutable(index_imm, usize_ty)));
    const ir::RegisterIdx dst(emitter.builder().state().registers.size());
    const ir::InstructionIdx instr =
        emitter.builder().instr({.op = ir::Opcode::CompField,
                                 .flags = {},
                                 .dst = dst,
                                 .measure = type,
                                 .operands = field_ops.finish()});
    emitter.builder().reg({.type = type, .def_idx = instr});
    emitter.cur().push(instr);
    const ir::OperandIdx result =
        emitter.builder().operand(ir::Operand::from_register(dst, type));
    emitter.clear_semantic(result);
    return result;
  }
  if (const comp::CompVal* sem = emitter.get_semantic(base)) {
    if ((sem->value.tag == comp::CompValue::Tag::Struct ||
         sem->value.tag == comp::CompValue::Tag::Tuple) &&
        index < sem->value.fields.size()) {
      comp::CompVal result;
      result.value = sem->value.fields[index];
      result.type = type;
      return emitter.add_value(result, type);
    }
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::TypeIdx i32_ty = emitter.builder().primitive(ir::TypeTag::I32);
  const i32 field_index = static_cast<i32>(index);
  ir::OperandSeq gep_ops;
  gep_ops.push(emitter.builder().operand(
      ir::Operand{ir::Operand{emitter.builder().state().operands[base.idx]}}));
  gep_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
      emitter.builder().immutable({.type = i32_ty, .data = {.i32_value = 0}}),
      i32_ty)));
  gep_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
      emitter.builder().immutable(
          {.type = i32_ty, .data = {.i32_value = field_index}}),
      i32_ty)));
  const ir::TypeIdx field_ref = emitter.builder().reference_type(type, true);
  const ir::RegisterIdx gep(emitter.builder().state().registers.size());
  const ir::InstructionIdx gep_instr =
      emitter.builder().instr({.op = ir::Opcode::GetElementPtr,
                               .flags = {},
                               .dst = gep,
                               .measure = type,
                               .operands = gep_ops.finish()});
  emitter.builder().reg({.type = field_ref, .def_idx = gep_instr});
  emitter.cur().push(gep_instr);
  const ir::RegisterIdx loaded(emitter.builder().state().registers.size());
  ir::OperandSeq load_ops;
  load_ops.push(
      emitter.builder().operand(ir::Operand::from_register(gep, field_ref)));
  const ir::InstructionIdx load_instr =
      emitter.builder().instr({.op = ir::Opcode::Load,
                               .flags = {},
                               .dst = loaded,
                               .measure = type,
                               .operands = load_ops.finish()});
  emitter.builder().reg({.type = type, .def_idx = load_instr});
  emitter.cur().push(load_instr);
  return emitter.builder().operand(ir::Operand::from_register(loaded, type));
}

static ir::OperandIdx emit_index_expr(const ast::AstArena& ast,
                                      [[maybe_unused]] ast::ExprIdx expr,
                                      ir::TypeIdx type,
                                      ConstEmitter& emitter,
                                      const EmitContext& ctx,
                                      const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprIndex& index_expr = node.payload.get<ast::ExprIndex>();
  const ir::TypeIdx receiver_type = expr_type(index_expr.receiver);
  ir::OperandIdx base = ir::OperandIdx(base::INVALID_IDX);
  if (ast.exprs[index_expr.receiver].kind == ast::ExprKind::Path ||
      ast.exprs[index_expr.receiver].kind == ast::ExprKind::Field ||
      ast.exprs[index_expr.receiver].kind == ast::ExprKind::Index) {
    base = emit_place_address(ast, index_expr.receiver, emitter, ctx, params);
  }
  if (!base.is_valid()) {
    base = emit_expr(ast, index_expr.receiver, receiver_type, emitter, ctx,
                     params);
  }
  if (!base.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (const comp::CompVal* sem_ptr = emitter.get_semantic(base)) {
    // Copy before emitting the index: recursive emission can grow the
    // side table and invalidate the pointer into it.
    const comp::CompVal sem = *sem_ptr;
    if (sem.value.tag == comp::CompValue::Tag::Variadic) {
      const ir::OperandIdx pos =
          emit_expr(ast, index_expr.index, expr_type(index_expr.index), emitter,
                    ctx, params);
      if (!pos.is_valid()) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      const ir::Operand& pop = emitter.builder().state().operands[pos.idx];
      if (!pop.is<ir::ImmutableIdx>()) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      const u64 slot = emitter.builder()
                           .state()
                           .immutables[pop.as_immutable()]
                           .data.u64_value;
      if (slot >= sem.value.slot_types.size()) {
        emitter.set_emit_error(ConstEmitter::EmitError::OutOfBounds);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      comp::CompVal slot_value;
      slot_value.type = sem.value.element.is_valid()
                            ? sem.value.element
                            : sem.value.slot_types[static_cast<usize>(slot)];
      slot_value.value.tag = comp::CompValue::Tag::RuntimeSlot;
      slot_value.value.int_value = slot;
      return emitter.add_semantic(slot_value, slot_value.type);
    }
    if (sem.value.tag == comp::CompValue::Tag::Array) {
      const ir::OperandIdx pos =
          emit_expr(ast, index_expr.index, expr_type(index_expr.index), emitter,
                    ctx, params);
      if (!pos.is_valid()) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      u64 idx = 0;
      if (!operand_int(emitter, pos, idx)) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      if (idx >= sem.value.fields.size()) {
        emitter.set_emit_error(ConstEmitter::EmitError::OutOfBounds);
        return ir::OperandIdx(base::INVALID_IDX);
      }
      const ir::ArrayType& shape =
          emitter.builder().state().array_types
              [emitter.builder().state().types[receiver_type].as_array()];
      comp::CompVal element;
      element.type = shape.element;
      element.value = sem.value.fields[static_cast<usize>(idx)];
      return emitter.add_value(element, shape.element);
    }
  }
  if (emitter.builder().state().types[receiver_type].tag !=
      ir::TypeTag::Array) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::TypeIdx position_type = expr_type(index_expr.index);
  const ir::OperandIdx position =
      emit_expr(ast, index_expr.index, position_type, emitter, ctx, params);
  if (!base.is_valid() || !position.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  // A compile-time index past the array's end is a source error.
  const ir::ArrayType& array_shape =
      emitter.builder().state().array_types
          [emitter.builder().state().types[receiver_type].as_array()];
  const ir::Operand& pos_op = emitter.builder().state().operands[position.idx];
  if (pos_op.is<ir::ImmutableIdx>()) {
    const u64 idx = emitter.builder()
                        .state()
                        .immutables[pos_op.as_immutable()]
                        .data.u64_value;
    if (idx >= array_shape.count) {
      emitter.set_emit_error(ConstEmitter::EmitError::OutOfBounds);
      // Keep the IR well-formed so the recorded error can be reported.
      const ir::ImmutableIdx zero =
          emitter.builder().immutable({.type = type, .data = {.u64_value = 0}});
      return emitter.builder().operand(ir::Operand::from_immutable(zero, type));
    }
  }
  const ir::TypeIdx i32_ty = emitter.builder().primitive(ir::TypeTag::I32);
  ir::OperandSeq gep_ops;
  gep_ops.push(emitter.builder().operand(
      ir::Operand{ir::Operand{emitter.builder().state().operands[base.idx]}}));
  gep_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
      emitter.builder().immutable({.type = i32_ty, .data = {.i32_value = 0}}),
      i32_ty)));
  gep_ops.push(emitter.builder().operand(ir::Operand{
      ir::Operand{emitter.builder().state().operands[position.idx]}}));
  const ir::TypeIdx elem_ref = emitter.builder().reference_type(type, true);
  const ir::RegisterIdx gep(emitter.builder().state().registers.size());
  const ir::InstructionIdx gep_instr =
      emitter.builder().instr({.op = ir::Opcode::GetElementPtr,
                               .flags = {},
                               .dst = gep,
                               .measure = type,
                               .operands = gep_ops.finish()});
  emitter.builder().reg({.type = elem_ref, .def_idx = gep_instr});
  emitter.cur().push(gep_instr);
  ir::OperandSeq load_ops;
  load_ops.push(
      emitter.builder().operand(ir::Operand::from_register(gep, elem_ref)));
  const ir::RegisterIdx loaded(emitter.builder().state().registers.size());
  const ir::InstructionIdx load_instr =
      emitter.builder().instr({.op = ir::Opcode::Load,
                               .flags = {},
                               .dst = loaded,
                               .measure = type,
                               .operands = load_ops.finish()});
  emitter.builder().reg({.type = type, .def_idx = load_instr});
  emitter.cur().push(load_instr);
  return emitter.builder().operand(ir::Operand::from_register(loaded, type));
}

static ir::OperandIdx emit_block_value(const ast::AstArena& ast,
                                       [[maybe_unused]] ast::ExprIdx expr,
                                       ir::TypeIdx type,
                                       ConstEmitter& emitter,
                                       const EmitContext& ctx,
                                       const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  return emit_block(ast, ast.blocks[node.payload.get<ast::ExprBlock>().block],
                    type, emitter, ctx, params);
}

static ir::OperandIdx emit_borrow_deref_expr(const ast::AstArena& ast,
                                             [[maybe_unused]] ast::ExprIdx expr,
                                             [[maybe_unused]] ir::TypeIdx type,
                                             ConstEmitter& emitter,
                                             const EmitContext& ctx,
                                             const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprIdx inner = node.kind == ast::ExprKind::Borrow
                                 ? node.payload.get<ast::ExprBorrow>().inner
                                 : node.payload.get<ast::ExprDeref>().inner;
  return emit_expr(ast, inner, expr_type(inner), emitter, ctx, params);
}

static ir::OperandIdx emit_if_expr(const ast::AstArena& ast,
                                   [[maybe_unused]] ast::ExprIdx expr,
                                   ir::TypeIdx type,
                                   ConstEmitter& emitter,
                                   const EmitContext& ctx,
                                   const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprIf& if_node = node.payload.get<ast::ExprIf>();
  const ast::Cond& cond = ast.conds[if_node.cond];
  if (cond.is_pattern) {
    return emit_pattern_if(ast, if_node, cond, type, emitter, ctx, params);
  }
  if (!cond.value.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const bool has_else = if_node.else_block.is_valid();
  const bool value_if =
      emitter.builder().state().types[type].tag != ir::TypeTag::Void;
  if (!has_else && value_if) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ast::Block& then_block = ast.blocks[if_node.then_block];
  const ast::Block* else_block =
      has_else ? &ast.blocks[if_node.else_block] : nullptr;
  if (value_if && !then_block.value.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (has_else && value_if && !else_block->value.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::OperandIdx cond_val =
      emit_expr(ast, cond.value, expr_type(cond.value), emitter, ctx, params);
  if (!cond_val.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  bool const_cond = false;
  if (operand_bool(emitter, cond_val, const_cond)) {
    // A compile-time condition picks one arm directly; emitting both
    // arms would let a `ret` or `break` in the untaken arm terminate
    // the surrounding block.
    const ast::Block& chosen =
        const_cond ? then_block : (has_else ? *else_block : then_block);
    if (!const_cond && !has_else) {
      return emitter.void_operand();
    }
    const ir::OperandIdx chosen_val =
        value_if && chosen.value.is_valid()
            ? emit_expr(ast, chosen.value, type, emitter, ctx, params)
            : emit_block(ast, chosen, type, emitter, ctx, params);
    return chosen_val.is_valid() ? chosen_val : emitter.void_operand();
  }
  const ir::BlockIdx then_b = emitter.new_block();
  const ir::BlockIdx else_b = emitter.new_block();
  const ir::BlockIdx merge_b = emitter.new_block();
  emitter.emit_cond_br(cond_val, then_b, else_b);

  ir::RegisterIdx result_reg = ir::RegisterIdx(base::INVALID_IDX);
  if (has_else && value_if) {
    result_reg = emitter.claim_reg(type);
    emitter.set_block_params(merge_b, emitter.block_param(type, result_reg));
  }

  // A statement-form `if` has branches whose blocks carry statements
  // but no value, so emit the whole block rather than just its value.
  emitter.set_cur(then_b);
  const ir::OperandIdx then_val =
      value_if
          ? emit_expr(ast, then_block.value, type, emitter, ctx, params)
          : emit_block(ast, then_block,
                       then_block.value.is_valid() ? expr_type(then_block.value)
                                                   : type,
                       emitter, ctx, params);
  if (!then_val.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (!emitter.terminated()) {
    emitter.emit_br(merge_b, has_else && value_if
                                 ? then_val
                                 : ir::OperandIdx(base::INVALID_IDX));
  }

  emitter.set_cur(else_b);
  if (has_else) {
    const ir::OperandIdx else_val =
        value_if ? emit_expr(ast, else_block->value, type, emitter, ctx, params)
                 : emit_block(ast, *else_block,
                              else_block->value.is_valid()
                                  ? expr_type(else_block->value)
                                  : type,
                              emitter, ctx, params);
    if (!else_val.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    if (!emitter.terminated()) {
      emitter.emit_br(merge_b,
                      value_if ? else_val : ir::OperandIdx(base::INVALID_IDX));
    }
  } else {
    emitter.emit_br(merge_b, ir::OperandIdx(base::INVALID_IDX));
  }

  emitter.set_cur(merge_b);
  if (has_else && value_if) {
    return emitter.builder().operand(
        ir::Operand::from_register(result_reg, type));
  }
  return emitter.void_operand();
}

static ir::OperandIdx emit_break_continue_expr(
    const ast::AstArena& ast,
    [[maybe_unused]] ast::ExprIdx expr,
    [[maybe_unused]] ir::TypeIdx type,
    ConstEmitter& emitter,
    [[maybe_unused]] const EmitContext& ctx,
    [[maybe_unused]] const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ir::BlockIdx target = node.kind == ast::ExprKind::Break
                                  ? emitter.break_target()
                                  : emitter.continue_target();
  if (!target.is_valid() || emitter.terminated()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  emitter.emit_br(target, ir::OperandIdx(base::INVALID_IDX));
  if (node.kind == ast::ExprKind::Break) {
    emitter.set_broke();
  }
  return emitter.void_operand();
}

static ir::OperandIdx emit_while_expr(const ast::AstArena& ast,
                                      [[maybe_unused]] ast::ExprIdx expr,
                                      [[maybe_unused]] ir::TypeIdx type,
                                      ConstEmitter& emitter,
                                      const EmitContext& ctx,
                                      const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprWhile& while_node = node.payload.get<ast::ExprWhile>();
  const ast::Cond& cond = ast.conds[while_node.cond];
  if (cond.is_pattern || !cond.value.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::BlockIdx header = emitter.new_block();
  const ir::BlockIdx body_b = emitter.new_block();
  const ir::BlockIdx exit_b = emitter.new_block();
  if (!emitter.terminated()) {
    emitter.emit_br(header, ir::OperandIdx(base::INVALID_IDX));
  }

  emitter.set_cur(header);
  const ir::OperandIdx cond_val =
      emit_expr(ast, cond.value, expr_type(cond.value), emitter, ctx, params);
  if (!cond_val.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  emitter.emit_cond_br(cond_val, body_b, exit_b);

  emitter.set_loop_targets(exit_b, header);
  emitter.set_cur(body_b);
  const ir::TypeIdx void_ty = emitter.builder().primitive(ir::TypeTag::Void);
  const ir::OperandIdx body_val = emit_block(ast, ast.blocks[while_node.body],
                                             void_ty, emitter, ctx, params);
  const bool body_ok = body_val.is_valid();
  if (body_ok && !emitter.terminated()) {
    emitter.emit_br(header, ir::OperandIdx(base::INVALID_IDX));
  }
  emitter.clear_loop_targets();
  if (!body_ok) {
    return ir::OperandIdx(base::INVALID_IDX);
  }

  emitter.set_cur(exit_b);
  return emitter.void_operand();
}

// A loop body is emitted once and re-executed over IR back edges, so a
// variant folded at emission would never change. Read the tag and payload
// through semantic opcodes instead.
static ir::OperandIdx emit_runtime_enum_match(const ast::AstArena& ast,
                                              const ast::ExprMatch& match_node,
                                              ir::TypeIdx scrutinee_type,
                                              ir::OperandIdx scrutinee,
                                              ir::TypeIdx type,
                                              ConstEmitter& emitter,
                                              const EmitContext& ctx,
                                              const ParamEnv* params) {
  std::vector<u32> variants;
  for (const ast::ExprMatchArm& arm : match_node.arms) {
    const ast::PatternNode& pat = ast.patterns[arm.pattern];
    if (arm.guard.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    if (pat.kind == ast::PatternKind::Wildcard) {
      variants.push_back(U32_MAX);
      continue;
    }
    if (pat.kind != ast::PatternKind::Tuple ||
        !pat.payload.tuple.path.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    for (ast::PatternIdx element : pat.payload.tuple.elements) {
      const ast::PatternKind kind = ast.patterns[element].kind;
      if (kind != ast::PatternKind::Ident &&
          kind != ast::PatternKind::MutIdent) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
    }
    const std::string_view name =
        ast.paths[pat.payload.tuple.path].segments.back().name;
    u32 variant = 0;
    if (!emitter.resolve_variant_index(scrutinee_type, name, variant)) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    variants.push_back(variant);
  }

  const ir::TypeIdx usize_ty = emitter.builder().primitive(ir::TypeTag::Usize);
  ir::OperandSeq tag_ops;
  tag_ops.push(emitter.builder().operand(
      ir::Operand{emitter.builder().state().operands[scrutinee.idx]}));
  const ir::RegisterIdx tag_reg(emitter.builder().state().registers.size());
  const ir::InstructionIdx tag_instr =
      emitter.builder().instr({.op = ir::Opcode::CompVariantTag,
                               .flags = {},
                               .dst = tag_reg,
                               .measure = ir::TypeIdx::invalid(),
                               .operands = tag_ops.finish()});
  emitter.builder().reg({.type = usize_ty, .def_idx = tag_instr});
  emitter.cur().push(tag_instr);
  const ir::OperandIdx tag_op =
      emitter.builder().operand(ir::Operand::from_register(tag_reg, usize_ty));

  std::vector<ir::BlockIdx> test_blocks;
  std::vector<ir::BlockIdx> body_blocks;
  for (usize i = 0; i < match_node.arms.size(); ++i) {
    test_blocks.push_back(emitter.new_block());
    body_blocks.push_back(emitter.new_block());
  }
  const ir::BlockIdx merge_b = emitter.new_block();
  const ir::RegisterIdx result_reg = emitter.claim_reg(type);
  emitter.set_block_params(merge_b, emitter.block_param(type, result_reg));
  emitter.emit_br(test_blocks[0], ir::OperandIdx(base::INVALID_IDX));

  for (usize i = 0; i < match_node.arms.size(); ++i) {
    const ast::ExprMatchArm& arm = match_node.arms[i];
    emitter.set_cur(test_blocks[i]);
    if (variants[i] != U32_MAX) {
      const ir::ImmutableIdx imm = emitter.builder().immutable(
          {.type = usize_ty, .data = {.u64_value = variants[i]}});
      const ir::OperandIdx pat_op =
          emitter.builder().operand(ir::Operand::from_immutable(imm, usize_ty));
      ir::OperandSeq cmp_ops;
      cmp_ops.push(emitter.builder().operand(
          ir::Operand{emitter.builder().state().operands[tag_op.idx]}));
      cmp_ops.push(emitter.builder().operand(
          ir::Operand{emitter.builder().state().operands[pat_op.idx]}));
      const ir::RegisterIdx cmp_dst(emitter.builder().state().registers.size());
      const ir::InstructionIdx cmp_instr =
          emitter.builder().instr({.op = ir::Opcode::Eq,
                                   .flags = {},
                                   .dst = cmp_dst,
                                   .measure = ir::TypeIdx::invalid(),
                                   .operands = cmp_ops.finish()});
      emitter.builder().reg(
          {.type = emitter.builder().primitive(ir::TypeTag::I1),
           .def_idx = cmp_instr});
      emitter.cur().push(cmp_instr);
      const ir::OperandIdx cmp_op =
          emitter.builder().operand(ir::Operand::from_register(
              cmp_dst, emitter.builder().primitive(ir::TypeTag::I1)));
      const ir::BlockIdx false_target = i + 1 < match_node.arms.size()
                                            ? test_blocks[i + 1]
                                            : body_blocks.back();
      emitter.emit_cond_br(cmp_op, body_blocks[i], false_target);
    } else {
      emitter.emit_br(body_blocks[i], ir::OperandIdx(base::INVALID_IDX));
    }

    emitter.set_cur(body_blocks[i]);
    const ast::PatternNode& pat = ast.patterns[arm.pattern];
    const usize saved = emitter.value_bindings_size();
    if (pat.kind == ast::PatternKind::Tuple &&
        pat.payload.tuple.path.is_valid()) {
      const ir::EnumType& shape =
          emitter.builder().state().enum_types
              [emitter.builder().state().types[scrutinee_type].as_enum()];
      const ir::EnumVariantTypeIdx vidx(shape.variants.head().idx +
                                        variants[i]);
      const ir::EnumVariantType& vshape =
          emitter.builder().state().enum_variant_types[vidx];
      for (usize field = 0; field < pat.payload.tuple.elements.size();
           ++field) {
        const ast::PatternNode& element =
            ast.patterns[pat.payload.tuple.elements[field]];
        const ir::TypeIdx field_type = field < vshape.fields.size()
                                           ? vshape.fields[field]
                                           : ir::TypeIdx(base::INVALID_IDX);
        const ir::ImmutableIdx index = emitter.builder().immutable(
            {.type = usize_ty, .data = {.u64_value = field}});
        ir::OperandSeq field_ops;
        field_ops.push(emitter.builder().operand(
            ir::Operand{emitter.builder().state().operands[scrutinee.idx]}));
        field_ops.push(emitter.builder().operand(
            ir::Operand::from_immutable(index, usize_ty)));
        const ir::RegisterIdx dst(emitter.builder().state().registers.size());
        const ir::InstructionIdx instr =
            emitter.builder().instr({.op = ir::Opcode::CompField,
                                     .flags = {},
                                     .dst = dst,
                                     .measure = field_type,
                                     .operands = field_ops.finish()});
        emitter.builder().reg({.type = field_type, .def_idx = instr});
        emitter.cur().push(instr);
        const std::string_view name = element.kind == ast::PatternKind::Ident
                                          ? element.payload.ident.name.name
                                          : element.payload.mut_ident.name.name;
        emitter.bind_value(name,
                           emitter.builder().operand(
                               ir::Operand::from_register(dst, field_type)));
      }
    }
    const ir::OperandIdx val =
        emit_expr(ast, arm.body, type, emitter, ctx, params);
    emitter.restore_value_bindings(saved);
    if (!val.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    if (!emitter.terminated()) {
      emitter.emit_br(merge_b, val);
    }
  }
  emitter.set_cur(merge_b);
  return emitter.builder().operand(
      ir::Operand::from_register(result_reg, type));
}

static ir::OperandIdx emit_match_expr(const ast::AstArena& ast,
                                      [[maybe_unused]] ast::ExprIdx expr,
                                      ir::TypeIdx type,
                                      ConstEmitter& emitter,
                                      const EmitContext& ctx,
                                      const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprMatch& match_node = node.payload.get<ast::ExprMatch>();
  if (match_node.arms.empty()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::TypeIdx scrutinee_type = expr_type(match_node.scrutinee);
  const ir::OperandIdx scrutinee = emit_expr(
      ast, match_node.scrutinee, scrutinee_type, emitter, ctx, params);
  if (!scrutinee.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (emitter.get_semantic(scrutinee) == nullptr &&
      emitter.builder().state().types[scrutinee_type].tag ==
          ir::TypeTag::Enum) {
    const ir::OperandIdx dispatched = emit_runtime_enum_match(
        ast, match_node, scrutinee_type, scrutinee, type, emitter, ctx, params);
    if (dispatched.is_valid()) {
      return dispatched;
    }
  }
  // A fully compile-time scrutinee resolves to one arm without IR
  // control flow. This is how semantic enums (e.g. `byte_slice`'s
  // `Option`) and literal values are matched. An integer literal rides
  // as an immutable, so read it into a scratch value too.
  comp::CompVal scratch;
  const comp::CompVal* sem = emitter.get_semantic(scrutinee);
  if (sem == nullptr) {
    const ir::Operand& sop = emitter.builder().state().operands[scrutinee.idx];
    if (sop.is<ir::ImmutableIdx>()) {
      const ir::TypeTag stag =
          emitter.builder().state().types[scrutinee_type].tag;
      const u64 bits = emitter.builder()
                           .state()
                           .immutables[sop.as_immutable()]
                           .data.u64_value;
      scratch.type = scrutinee_type;
      if (stag == ir::TypeTag::I1) {
        scratch.value.tag = comp::CompValue::Tag::Bool;
        scratch.value.bool_value = bits != 0;
      } else if (ir::is_integer_type(stag)) {
        scratch.value.tag = comp::CompValue::Tag::Int;
        scratch.value.int_value = bits;
      }
      if (scratch.value.tag != comp::CompValue::Tag::Void) {
        sem = &scratch;
      }
    }
  }
  if (sem != nullptr) {
    // Matching a pattern emits values, which can reallocate the side
    // table; keep the scrutinee before that happens.
    const comp::CompVal scrutinee_value = *sem;
    const usize saved = emitter.value_bindings_size();
    for (const ast::ExprMatchArm& arm : match_node.arms) {
      std::vector<std::pair<std::string_view, ir::OperandIdx>> binds;
      const bool matched = semantic_pattern_matches(
          ast, arm.pattern, scrutinee_value, emitter, ctx, params,
          [&](std::string_view name, ir::OperandIdx op) {
            binds.emplace_back(name, op);
          });
      if (!matched) {
        continue;
      }
      for (const auto& [name, op] : binds) {
        emitter.bind_value(name, op);
      }
      if (arm.guard.is_valid()) {
        const ir::OperandIdx guard = emit_expr(
            ast, arm.guard, emitter.builder().primitive(ir::TypeTag::I1),
            emitter, ctx, params);
        bool pass = false;
        if (guard.is_valid()) {
          if (const comp::CompVal* gs = emitter.get_semantic(guard)) {
            pass = gs->value.tag == comp::CompValue::Tag::Bool
                       ? gs->value.bool_value
                       : gs->value.int_value != 0;
          } else {
            u64 bits = 0;
            pass = operand_int(emitter, guard, bits) && bits != 0;
          }
        }
        emitter.restore_value_bindings(saved);
        if (!pass) {
          continue;
        }
        for (const auto& [name, op] : binds) {
          emitter.bind_value(name, op);
        }
      }
      const ir::OperandIdx val =
          emit_expr(ast, arm.body, type, emitter, ctx, params);
      emitter.restore_value_bindings(saved);
      return val;
    }
    emitter.restore_value_bindings(saved);
    return ir::OperandIdx(base::INVALID_IDX);
  }
  // The lowered path handles only literal and wildcard patterns with
  // a trailing wildcard arm; semantic values were resolved above.
  for (const ast::ExprMatchArm& arm : match_node.arms) {
    const ast::PatternNode& pat = ast.patterns[arm.pattern];
    if ((pat.kind != ast::PatternKind::Literal &&
         pat.kind != ast::PatternKind::Wildcard) ||
        arm.guard.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
  }
  if (ast.patterns[match_node.arms.back().pattern].kind !=
      ast::PatternKind::Wildcard) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  std::vector<ir::BlockIdx> test_blocks;
  std::vector<ir::BlockIdx> body_blocks;
  for (usize i = 0; i < match_node.arms.size(); ++i) {
    test_blocks.push_back(emitter.new_block());
    body_blocks.push_back(emitter.new_block());
  }
  const ir::BlockIdx merge_b = emitter.new_block();
  const ir::RegisterIdx result_reg = emitter.claim_reg(type);
  emitter.set_block_params(merge_b, emitter.block_param(type, result_reg));
  emitter.emit_br(test_blocks[0], ir::OperandIdx(base::INVALID_IDX));

  for (usize i = 0; i < match_node.arms.size(); ++i) {
    const ast::ExprMatchArm& arm = match_node.arms[i];
    emitter.set_cur(test_blocks[i]);
    const ast::PatternNode& pat = ast.patterns[arm.pattern];
    if (pat.kind == ast::PatternKind::Wildcard) {
      emitter.emit_br(body_blocks[i], ir::OperandIdx(base::INVALID_IDX));
    } else {
      const ast::Literal& lit = ast.literals[pat.payload.literal.value];
      if (lit.kind != ast::LiteralKind::Integer &&
          lit.kind != ast::LiteralKind::Bool) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      u64 bits = lit.kind == ast::LiteralKind::Integer
                     ? comp::parse_numeric_value(lit.spelling)
                     : (lit.spelling == "true" ? 1u : 0u);
      if (lit.is_negative) {
        bits = 0 - bits;
      }
      const ir::TypeTag tag =
          emitter.builder().state().types[scrutinee_type].tag;
      const ir::ImmutableIdx imm = emitter.builder().immutable(
          {.type = scrutinee_type,
           .data = {.u64_value = bits & comp::mask(tag)}});
      const ir::OperandIdx pat_op = emitter.builder().operand(
          ir::Operand::from_immutable(imm, scrutinee_type));
      ir::OperandSeq cmp_ops;
      cmp_ops.push(emitter.builder().operand(
          ir::Operand{emitter.builder().state().operands[scrutinee.idx]}));
      cmp_ops.push(emitter.builder().operand(
          ir::Operand{emitter.builder().state().operands[pat_op.idx]}));
      const ir::RegisterIdx cmp_dst(emitter.builder().state().registers.size());
      const ir::InstructionIdx cmp_instr =
          emitter.builder().instr({.op = ir::Opcode::Eq,
                                   .flags = {},
                                   .dst = cmp_dst,
                                   .measure = ir::TypeIdx::invalid(),
                                   .operands = cmp_ops.finish()});
      emitter.builder().reg(
          {.type = emitter.builder().primitive(ir::TypeTag::I1),
           .def_idx = cmp_instr});
      emitter.cur().push(cmp_instr);
      const ir::OperandIdx cmp_op =
          emitter.builder().operand(ir::Operand::from_register(
              cmp_dst, emitter.builder().primitive(ir::TypeTag::I1)));
      const ir::BlockIdx false_target = i + 1 < match_node.arms.size()
                                            ? test_blocks[i + 1]
                                            : body_blocks.back();
      emitter.emit_cond_br(cmp_op, body_blocks[i], false_target);
    }
    emitter.set_cur(body_blocks[i]);
    const ir::OperandIdx val =
        emit_expr(ast, arm.body, type, emitter, ctx, params);
    if (!val.is_valid()) {
      return ir::OperandIdx(base::INVALID_IDX);
    }
    if (!emitter.terminated()) {
      emitter.emit_br(merge_b, val);
    }
  }
  emitter.set_cur(merge_b);
  return emitter.builder().operand(
      ir::Operand::from_register(result_reg, type));
}

static ir::OperandIdx emit_method_call_expr(const ast::AstArena& ast,
                                            ast::ExprIdx expr,
                                            ir::TypeIdx type,
                                            ConstEmitter& emitter,
                                            const EmitContext& ctx,
                                            const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprMethodCall& method = node.payload.get<ast::ExprMethodCall>();
  const ir::OperandIdx built =
      emit_method_call(ast, method, type, emitter, ctx, params);
  if (built.is_valid()) {
    return built;
  }
  // A user-defined method on a comp value: inline its body with the
  // receiver bound to `self` as the first argument.
  std::vector<ast::ExprIdx> method_args;
  method_args.push_back(method.receiver);
  method_args.insert(method_args.end(), method.args.begin(), method.args.end());
  const ir::OperandIdx result =
      emit_comp_inline(ast, expr, method_args, emitter, ctx, params);
  // A `mut self` method may have updated the receiver through its
  // `self` binding; write the new value back to a named receiver.
  bool receiver_is_address = false;
  if (method.receiver.is_valid() &&
      ast.exprs[method.receiver].kind == ast::ExprKind::Path) {
    const auto& segs =
        ast.paths[ast.exprs[method.receiver].payload.get<ast::ExprPath>().idx]
            .segments;
    if (segs.size() == 1 && emitter.lookup_local(segs[0].name) != nullptr) {
      receiver_is_address = true;
    }
  }
  if (!receiver_is_address && method.receiver.is_valid() &&
      ast.exprs[method.receiver].kind == ast::ExprKind::Path) {
    const ir::OperandIdx updated = emitter.lookup_value("self");
    if (updated.is_valid()) {
      const auto& segs =
          ast.paths[ast.exprs[method.receiver].payload.get<ast::ExprPath>().idx]
              .segments;
      if (segs.size() == 1) {
        if (emitter.lookup_value(segs[0].name).is_valid()) {
          emitter.rebind_value(segs[0].name, updated);
        } else {
          emitter.bind_value(segs[0].name, updated);
        }
      }
    }
  }
  return result;
}

static ir::OperandIdx emit_closure_expr(
    [[maybe_unused]] const ast::AstArena& ast,
    [[maybe_unused]] ast::ExprIdx expr,
    [[maybe_unused]] ir::TypeIdx type,
    ConstEmitter& emitter,
    [[maybe_unused]] const EmitContext& ctx,
    [[maybe_unused]] const ParamEnv* params) {
  [[maybe_unused]] const ast::ExprNode& node = ast.exprs[expr];
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  return emitter.void_operand();
}

static ir::OperandIdx emit_loop_expr(const ast::AstArena& ast,
                                     [[maybe_unused]] ast::ExprIdx expr,
                                     [[maybe_unused]] ir::TypeIdx type,
                                     ConstEmitter& emitter,
                                     const EmitContext& ctx,
                                     const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprLoop& loop_node = node.payload.get<ast::ExprLoop>();
  if (!loop_node.body.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::BlockIdx header = emitter.new_block();
  const ir::BlockIdx body_b = emitter.new_block();
  const ir::BlockIdx exit_b = emitter.new_block();
  if (!emitter.terminated()) {
    emitter.emit_br(header, ir::OperandIdx(base::INVALID_IDX));
  }
  emitter.set_cur(header);
  emitter.emit_br(body_b, ir::OperandIdx(base::INVALID_IDX));
  emitter.set_loop_targets(exit_b, header);
  emitter.set_cur(body_b);
  const ir::TypeIdx void_ty = emitter.builder().primitive(ir::TypeTag::Void);
  const ir::OperandIdx body_val = emit_block(ast, ast.blocks[loop_node.body],
                                             void_ty, emitter, ctx, params);
  const bool body_ok = body_val.is_valid();
  if (body_ok && !emitter.terminated()) {
    emitter.emit_br(header, ir::OperandIdx(base::INVALID_IDX));
  }
  emitter.clear_loop_targets();
  if (!body_ok) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  emitter.set_cur(exit_b);
  return emitter.void_operand();
}

static ir::OperandIdx emit_range_expr(const ast::AstArena& ast,
                                      [[maybe_unused]] ast::ExprIdx expr,
                                      ir::TypeIdx type,
                                      ConstEmitter& emitter,
                                      const EmitContext& ctx,
                                      const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  return emit_range(ast, node.payload.get<ast::ExprRange>(), type, emitter, ctx,
                    params);
}

static ir::OperandIdx emit_question_expr(const ast::AstArena& ast,
                                         [[maybe_unused]] ast::ExprIdx expr,
                                         [[maybe_unused]] ir::TypeIdx type,
                                         ConstEmitter& emitter,
                                         const EmitContext& ctx,
                                         const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprQuestion& question = node.payload.get<ast::ExprQuestion>();
  const ir::TypeIdx inner_type = expr_type(question.inner);
  const ir::OperandIdx inner =
      emit_expr(ast, question.inner, inner_type, emitter, ctx, params);
  if (!inner.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  if (emitter.builder().state().types[inner_type].tag != ir::TypeTag::Enum) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::TypeIdx i32_ty = emitter.builder().primitive(ir::TypeTag::I32);
  const ir::RegisterIdx disc(emitter.builder().state().registers.size());
  const ir::InstructionIdx tag_instr =
      emitter.builder().instr({.op = ir::Opcode::EnumTagLoad,
                               .flags = {},
                               .dst = disc,
                               .measure = ir::TypeIdx::invalid(),
                               .operands = ir::OperandIdxRange{inner, 1}});
  emitter.builder().reg({.type = i32_ty, .def_idx = tag_instr});
  emitter.cur().push(tag_instr);
  const ir::OperandIdx zero =
      emitter.builder().operand(ir::Operand::from_immutable(
          emitter.builder().immutable(
              {.type = i32_ty, .data = {.i32_value = 0}}),
          i32_ty));
  ir::OperandSeq cmp_ops;
  cmp_ops.push(
      emitter.builder().operand(ir::Operand::from_register(disc, i32_ty)));
  cmp_ops.push(zero);
  const ir::RegisterIdx is_ok(emitter.builder().state().registers.size());
  const ir::InstructionIdx cmp_instr =
      emitter.builder().instr({.op = ir::Opcode::Eq,
                               .flags = {},
                               .dst = is_ok,
                               .measure = ir::TypeIdx::invalid(),
                               .operands = cmp_ops.finish()});
  emitter.builder().reg({.type = emitter.builder().primitive(ir::TypeTag::I1),
                         .def_idx = cmp_instr});
  emitter.cur().push(cmp_instr);
  const ir::OperandIdx cond =
      emitter.builder().operand(ir::Operand::from_register(
          is_ok, emitter.builder().primitive(ir::TypeTag::I1)));
  const ir::BlockIdx ok_b = emitter.new_block();
  const ir::BlockIdx fail_b = emitter.new_block();
  const ir::BlockIdx merge_b = emitter.new_block();
  emitter.emit_cond_br(cond, ok_b, fail_b);

  emitter.set_cur(ok_b);
  const ir::EnumType& shape =
      emitter.builder()
          .state()
          .enum_types[emitter.builder().state().types[inner_type].as_enum()];
  const ir::EnumVariantTypeIdx vidx(shape.variants.head());
  const ir::EnumVariantType& variant =
      emitter.builder().state().enum_variant_types[vidx];
  if (variant.fields.size() < 1) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::TypeIdx payload_ty = variant.fields[0];
  ir::OperandSeq fa_ops;
  fa_ops.push(emitter.builder().operand(
      ir::Operand{emitter.builder().state().operands[inner.idx]}));
  fa_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
      emitter.builder().immutable({.type = i32_ty, .data = {.i32_value = 0}}),
      i32_ty)));
  fa_ops.push(emitter.builder().operand(ir::Operand::from_immutable(
      emitter.builder().immutable({.type = i32_ty, .data = {.i32_value = 0}}),
      i32_ty)));
  const ir::RegisterIdx payload_addr(
      emitter.builder().state().registers.size());
  const ir::InstructionIdx fa_instr =
      emitter.builder().instr({.op = ir::Opcode::EnumFieldAddr,
                               .flags = {},
                               .dst = payload_addr,
                               .measure = payload_ty,
                               .operands = fa_ops.finish()});
  emitter.builder().reg({.type = payload_ty, .def_idx = fa_instr});
  emitter.cur().push(fa_instr);
  const ir::RegisterIdx loaded(emitter.builder().state().registers.size());
  const ir::InstructionIdx load_instr = emitter.builder().instr(
      {.op = ir::Opcode::Load,
       .flags = {},
       .dst = loaded,
       .measure = payload_ty,
       .operands = ir::OperandIdxRange{
           emitter.builder().operand(
               ir::Operand::from_register(payload_addr, payload_ty)),
           1}});
  emitter.builder().reg({.type = payload_ty, .def_idx = load_instr});
  emitter.cur().push(load_instr);

  const ir::RegisterIdx result_reg = emitter.claim_reg(payload_ty);
  emitter.set_block_params(merge_b,
                           emitter.block_param(payload_ty, result_reg));
  emitter.emit_br(merge_b, emitter.builder().operand(
                               ir::Operand::from_register(loaded, payload_ty)));

  emitter.set_cur(fail_b);
  emitter.emit_br(merge_b, ir::OperandIdx(base::INVALID_IDX));

  emitter.set_cur(merge_b);
  return emitter.builder().operand(
      ir::Operand::from_register(result_reg, payload_ty));
}

static ir::OperandIdx emit_cast_expr(const ast::AstArena& ast,
                                     [[maybe_unused]] ast::ExprIdx expr,
                                     ir::TypeIdx type,
                                     ConstEmitter& emitter,
                                     const EmitContext& ctx,
                                     const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  [[maybe_unused]] const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  return emit_cast(ast, node.payload.get<ast::ExprCast>(), type, emitter, ctx,
                   params);
}

static ir::OperandIdx emit_unary_expr(const ast::AstArena& ast,
                                      [[maybe_unused]] ast::ExprIdx expr,
                                      ir::TypeIdx type,
                                      ConstEmitter& emitter,
                                      const EmitContext& ctx,
                                      const ParamEnv* params) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ExprTypeFn& expr_type = ctx.expr_type;
  [[maybe_unused]] const EmitContext::PathValue& resolve_path =
      ctx.resolve_path;
  [[maybe_unused]] const EmitContext::LayoutType& resolve_layout_type =
      ctx.resolve_layout_type;
  [[maybe_unused]] const EmitContext::CompFn& resolve_comp_fn =
      ctx.resolve_comp_fn;
  [[maybe_unused]] const EmitContext::FieldIndex& resolve_field_index =
      ctx.resolve_field_index;
  const ast::ExprUnary& unary = node.payload.get<ast::ExprUnary>();
  const ir::OperandIdx inner =
      emit_expr(ast, unary.inner, expr_type(unary.inner), emitter, ctx, params);
  if (!inner.is_valid()) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  ir::Opcode op = ir::Opcode::Noop;
  ir::TypeIdx result_type = type;
  switch (unary.op) {
    case ast::UnaryOp::BitNot: op = ir::Opcode::Not; break;
    case ast::UnaryOp::Neg:
    case ast::UnaryOp::Not: break;
  }
  if (unary.op == ast::UnaryOp::Not) {
    bool inner_bool = false;
    if (operand_bool(emitter, inner, inner_bool)) {
      return bool_immutable(emitter, type, !inner_bool);
    }
  }
  if (unary.op == ast::UnaryOp::Neg || unary.op == ast::UnaryOp::Not) {
    const ir::TypeIdx inner_type = expr_type(unary.inner);
    const ir::ImmutableIdx zero = emitter.builder().immutable(
        {.type = inner_type, .data = {.u64_value = 0}});
    const ir::OperandIdx zero_op = emitter.builder().operand(
        ir::Operand::from_immutable(zero, inner_type));
    ir::OperandSeq operands;
    operands.push(zero_op);
    operands.push(emitter.builder().operand(ir::Operand{
        ir::Operand{emitter.builder().state().operands[inner.idx]}}));
    // Negation wraps as `(0 - x) & mask`.
    // A checked subtraction would reject the signed minimum, whose
    // magnitude one past the positive maximum is exactly what a
    // negated integer literal may carry.
    op =
        unary.op == ast::UnaryOp::Neg ? ir::Opcode::IntSubWrap : ir::Opcode::Eq;
    if (unary.op == ast::UnaryOp::Not) {
      result_type = emitter.builder().primitive(ir::TypeTag::I1);
    }
    const ir::RegisterIdx dst(emitter.builder().state().registers.size());
    const ir::InstructionIdx instr =
        emitter.builder().instr({.op = op,
                                 .flags = {},
                                 .dst = dst,
                                 .measure = ir::TypeIdx::invalid(),
                                 .operands = operands.finish()});
    emitter.builder().reg({.type = result_type, .def_idx = instr});
    emitter.cur().push(instr);
    return emitter.builder().operand(
        ir::Operand::from_register(dst, result_type));
  }
  if (op == ir::Opcode::Noop) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  ir::OperandSeq operands;
  operands.push(emitter.builder().operand(
      ir::Operand{ir::Operand{emitter.builder().state().operands[inner.idx]}}));
  const ir::RegisterIdx dst(emitter.builder().state().registers.size());
  const ir::InstructionIdx instr =
      emitter.builder().instr({.op = op,
                               .flags = {},
                               .dst = dst,
                               .measure = ir::TypeIdx::invalid(),
                               .operands = operands.finish()});
  emitter.builder().reg({.type = result_type, .def_idx = instr});
  emitter.cur().push(instr);
  return emitter.builder().operand(
      ir::Operand::from_register(dst, result_type));
}

ir::OperandIdx emit_expr(const ast::AstArena& ast,
                         ast::ExprIdx expr,
                         ir::TypeIdx type,
                         ConstEmitter& emitter,
                         const EmitContext& ctx,
                         const ParamEnv* params = nullptr) {
  const ast::ExprNode& node = ast.exprs[expr];
  ConstEmitter::SpanScope span_scope(emitter, node.span);
  switch (node.kind) {
    case ast::ExprKind::Return:
      return emit_return_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Literal:
      return emit_literal_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Call:
      return emit_call_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Path:
      return emit_path_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Binary:
      return emit_binary_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Tuple:
      return emit_tuple_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Array:
      return emit_array_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Struct:
      return emit_struct_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Field:
      return emit_field_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Index:
      return emit_index_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Block:
      return emit_block_value(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Borrow:
      return emit_borrow_deref_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Deref:
      return emit_borrow_deref_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::If:
      return emit_if_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Break:
      return emit_break_continue_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Continue:
      return emit_break_continue_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::While:
      return emit_while_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Match:
      return emit_match_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::MethodCall:
      return emit_method_call_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Closure:
      return emit_closure_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Loop:
      return emit_loop_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Range:
      return emit_range_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Question:
      return emit_question_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Cast:
      return emit_cast_expr(ast, expr, type, emitter, ctx, params);
    case ast::ExprKind::Unary:
      return emit_unary_expr(ast, expr, type, emitter, ctx, params);
    default: break;
  }
  return ir::OperandIdx(base::INVALID_IDX);
}

}  // namespace

comp::IrRunError emit_failure(ConstEmitter::EmitError kind,
                              std::string_view fallback) {
  const ir::InstructionIdx no_instr(base::INVALID_IDX);
  switch (kind) {
    case ConstEmitter::EmitError::Budget:
      return {comp::IrRunError::Kind::Quota, no_instr,
              "comp evaluation quota exhausted"};
    case ConstEmitter::EmitError::OutOfBounds:
      return {comp::IrRunError::Kind::OutOfBounds, no_instr,
              "index out of bounds in comp evaluation"};
    case ConstEmitter::EmitError::Overflow:
      return {comp::IrRunError::Kind::Overflow, no_instr,
              "integer overflow in comp evaluation"};
    case ConstEmitter::EmitError::UnresolvedPath:
      return {comp::IrRunError::Kind::Unsupported, no_instr,
              "path is not comp-evaluable"};
    case ConstEmitter::EmitError::SliceBoundary:
      return {comp::IrRunError::Kind::Unsupported, no_instr,
              "slice not on a scalar boundary"};
    case ConstEmitter::EmitError::ExpansionCap:
      return {comp::IrRunError::Kind::ExpansionCap, no_instr,
              "comp expansion exceeded its iteration cap"};
    case ConstEmitter::EmitError::None: break;
  }
  return {comp::IrRunError::Kind::Unsupported, no_instr, fallback};
}

bool evaluate_const_ir(const ast::AstArena& ast,
                       ast::ExprIdx expr,
                       ir::TypeIdx type,
                       ir::StorageBuilder& builder,
                       ir::PointerWidth width,
                       const EmitContext& ctx,
                       u64& quota,
                       u32& depth,
                       comp::CompVal& out,
                       comp::IrRunError* error,
                       diag::Span* failure_span) {
  const ir::StorageBuilder::Checkpoint cp = builder.checkpoint();
  ConstEmitter emitter(builder, quota);
  emitter.set_context(ctx);
  emitter.new_block();  // entry
  const ir::BlockIdx return_b = emitter.new_block();
  const ir::RegisterIdx return_reg = emitter.claim_reg(type);
  emitter.set_block_params(return_b, emitter.block_param(type, return_reg));
  emitter.push_return_target(return_b, return_reg);
  const ir::OperandIdx value = emit_expr(ast, expr, type, emitter, ctx);
  if (!value.is_valid()) {
    if (error != nullptr) {
      *error =
          emit_failure(emitter.emit_error, "comp expression is not evaluable");
    }
    if (failure_span != nullptr && emitter.has_error_span_) {
      *failure_span = emitter.error_span_;
    }
    builder.rollback(cp);
    return false;
  }
  if (!emitter.terminated()) {
    emitter.emit_br(return_b, value);
  }
  emitter.set_cur(return_b);
  const ir::InstructionIdx ret =
      builder.instr({.op = ir::Opcode::Ret,
                     .flags = {},
                     .dst = ir::RegisterIdx(base::INVALID_IDX),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = ir::OperandIdxRange{
                         emitter.builder().operand(
                             ir::Operand::from_register(return_reg, type)),
                         1}});
  emitter.cur().push(ret);
  emitter.finish();
  if (emitter.emit_error != ConstEmitter::EmitError::None) {
    if (error != nullptr) {
      *error =
          emit_failure(emitter.emit_error, "comp expression is not evaluable");
    }
    if (failure_span != nullptr && emitter.has_error_span_) {
      *failure_span = emitter.error_span_;
    }
    builder.rollback(cp);
    return false;
  }
  const ir::FunctionIdx fn = builder.function(
      {.meta = {.return_type = type,
                .param_types = {},
                .name = str::EMPTY_STRING_ID,
                .path = str::EMPTY_STRING_ID,
                .kind = ir::SymbolKind::Free,
                .generics = ir::TypeIdxRange{}},
       .blocks = ir::BlockIdxRange{
           emitter.start(), static_cast<u32>(builder.state().blocks.size() -
                                             emitter.start().idx)}});
  base::Result<comp::CompVal, comp::IrRunError> result =
      comp::interpret_function(
          builder.state(), ctx.evaluator.strings(), width, fn, {}, quota, depth,
          emitter.semantic_values(), cp.registers,
          static_cast<u32>(builder.state().registers.size()));
  // The synthetic function existed only to read a value back. Roll it
  // out of the builder so no synthetic body reaches the artifact.
  builder.rollback(cp);
  if (result.is_err()) {
    comp::IrRunError run_error = std::move(result).unwrap_err();
    if (failure_span != nullptr) {
      if (const std::optional<diag::Span> span =
              emitter.span_of(run_error.instr)) {
        *failure_span = *span;
      }
    }
    if (error != nullptr) {
      *error = run_error;
    }
    return false;
  }
  out = std::move(result).unwrap();
  if (emitter.builder().state().types[type].tag == ir::TypeTag::Void) {
    out.value.tag = comp::CompValue::Tag::Void;
  }
  out.type = type;
  return true;
}

}  // namespace analyzer
