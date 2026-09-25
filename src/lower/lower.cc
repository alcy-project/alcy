// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lower/lower.h"

#include <cstdlib>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "debug/dcheck.h"
#include "debug/dlog.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "diag/span.h"
#include "fmt/format.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/immutable.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/verifier.h"
#include "lower/lowerer.h"

namespace lower {

bool Lowerer::is_block_terminator(ir::Opcode op) {
  return op == ir::Opcode::Br || op == ir::Opcode::CondBr ||
         op == ir::Opcode::Switch || op == ir::Opcode::Ret ||
         op == ir::Opcode::Unreachable;
}

usize Lowerer::at(ir::BlockIdx block) const {
  return block.idx - fn_block_base_;
}

ir::BlockIdx Lowerer::reserve_block() {
  streams_.emplace_back();
  stream_last_.emplace_back(base::kInvalidIdx);
  fn_blocks_.emplace_back(block_next_++);
  return fn_blocks_.back();
}

void Lowerer::switch_to(ir::BlockIdx block) {
  cur_ = block;
}

bool Lowerer::terminated(ir::BlockIdx block) {
  for (usize i = 0; i < fn_blocks_.size(); ++i) {
    if (fn_blocks_[i].idx == block.idx) {
      const ir::InstructionIdx last = stream_last_[i];
      return last.is_valid() &&
             is_block_terminator(builder.state().instrs[last].op);
    }
  }
  return false;
}

bool Lowerer::terminated_cur() {
  const ir::InstructionIdx last = stream_last_[at(cur_)];
  return last.is_valid() &&
         is_block_terminator(builder.state().instrs[last].op);
}

Lowerer::Lowerer(analyzer::CheckedPackage package,
                 ir::PointerWidth width,
                 ast::AstArena& ast,
                 str::StringInterner& strings,
                 diag::DiagBag& bag)
    : pkg(std::move(package)),
      builder(std::move(pkg.types).take_state()),
      width(width),
      ast(ast),
      strings(strings),
      bag(bag) {}

ir::RegisterIdx Lowerer::claim_reg() {
  return ir::RegisterIdx(static_cast<u32>(builder.state().registers.size()));
}

ir::OperandIdx Lowerer::to_operand(ir::RegisterIdx reg, ir::TypeIdx type) {
  return builder.operand(ir::Operand::from_register(reg, type));
}

ir::OperandIdx Lowerer::to_operand(ir::ImmutableIdx imm, ir::TypeIdx type) {
  return builder.operand(ir::Operand::from_immutable(imm, type));
}

ir::RegisterIdx Lowerer::emit(ir::Opcode op,
                              ir::TypeIdx type,
                              const std::vector<ir::OperandIdx>& ops) {
  const ir::RegisterIdx dst = claim_reg();
  // Operand ranges must be consecutive in storage, but inputs are
  // built bottom-up at arbitrary positions; re-append copies so the
  // range is always fresh and contiguous.
  const ir::OperandIdx head =
      ir::OperandIdx(static_cast<u32>(builder.state().operands.size()));
  for (ir::OperandIdx op_idx : ops) {
    builder.operand(ir::Operand(builder.state().operands[op_idx]));
  }
  const ir::OperandIdxRange range = {head, static_cast<u32>(ops.size())};
  const ir::InstructionIdx instr =
      builder.instr({.op = op,
                     .flags = {},
                     .dst = dst,
                     .measure = ir::TypeIdx::invalid(),
                     .operands = range});
  builder.reg({.type = type, .def_idx = instr});
  streams_[at(cur_)].push(instr);
  stream_last_[at(cur_)] = instr;
  instr_spans_.push_back(cur_span_);
  return dst;
}

// Emits a type query, which measures a type instead of consuming
// operands: the measured type rides on the instruction.
ir::RegisterIdx Lowerer::emit_type_query(ir::Opcode op, ir::TypeIdx measure) {
  if (failed) {
    return ir::RegisterIdx(base::kInvalidIdx);
  }
  const ir::TypeIdx usize_ty = usize_type();
  const ir::RegisterIdx dst = claim_reg();
  const ir::OperandIdx head =
      ir::OperandIdx(static_cast<u32>(builder.state().operands.size()));
  const ir::InstructionIdx instr = builder.instr({.op = op,
                                                  .flags = {},
                                                  .dst = dst,
                                                  .measure = measure,
                                                  .operands = {head, 0}});
  builder.reg({.type = usize_ty, .def_idx = instr});
  streams_[at(cur_)].push(instr);
  stream_last_[at(cur_)] = instr;
  instr_spans_.push_back(cur_span_);
  return dst;
}

const std::vector<ir::TypeIdx>& Lowerer::fn_instance_args(u32 module,
                                                          u32 sig_index) const {
  static const std::vector<ir::TypeIdx> kNone;
  for (const analyzer::FnInstance& instance : pkg.fn_insts) {
    if (instance.module == module && instance.sig_index == sig_index) {
      return instance.args;
    }
  }
  return kNone;
}

void Lowerer::emit_void(ir::Opcode op, const std::vector<ir::OperandIdx>& ops) {
  const ir::OperandIdx head =
      ir::OperandIdx(static_cast<u32>(builder.state().operands.size()));
  for (ir::OperandIdx op_idx : ops) {
    builder.operand(ir::Operand(builder.state().operands[op_idx]));
  }
  const ir::OperandIdxRange range = {head, static_cast<u32>(ops.size())};
  const ir::InstructionIdx void_instr =
      builder.instr({.op = op,
                     .flags = {},
                     .dst = ir::RegisterIdx(base::kInvalidIdx),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = range});
  streams_[at(cur_)].push(void_instr);
  stream_last_[at(cur_)] = void_instr;
  instr_spans_.push_back(cur_span_);
}

Val Lowerer::materialize(Val v) {
  if (!v.address || tag_of(v.type) == ir::TypeTag::Never) {
    return v;
  }
  const ir::RegisterIdx reg = emit(ir::Opcode::Load, v.type, {v.op});
  return Val{to_operand(reg, v.type), v.type, false, false};
}

Val Lowerer::address_of(Val v) {
  if (v.address) {
    return v;
  }
  const Val material = materialize(v);
  const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, v.type, {size_one});
  emit_void(ir::Opcode::Store, {material.op, to_operand(addr, v.type)});
  return Val{to_operand(addr, v.type), v.type, true, v.place};
}

// Uses a value, emitting a Move marker when a non-Copy place is
// consumed. The marker's source is the place address so later
// ownership analysis observes origins, not temporaries.
void Lowerer::mark_move(Val v) {
  if (v.place && v.address && !is_copy(v.type)) {
    const ir::RegisterIdx marker = emit(ir::Opcode::Move, v.type, {v.op});
    (void)marker;
  }
}

ir::OperandIdx Lowerer::use_value(Val v) {
  mark_move(v);
  return materialize(v).op;
}

const Local* Lowerer::lookup_local(std::string_view name) const {
  for (const auto& local : locals | std::views::reverse) {
    if (local.name == name) {
      return &local;
    }
  }
  return nullptr;
}

const analyzer::CheckedModule::StaticInfo* Lowerer::lookup_static(
    u32 mod,
    std::string_view name) const {
  for (const auto& info : pkg.modules[mod].statics) {
    if (info.name == name) {
      return &info;
    }
  }
  for (const analyzer::Import& import : pkg.tree.modules[mod]->imports) {
    if (import.ns != analyzer::Namespace::Value || import.name != name) {
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

ir::TypeIdx Lowerer::expr_type(ast::ExprIdx expr) {
  for (const auto& entry : pkg.modules[module].expr_types) {
    if (entry.expr == expr && entry.inst == cur_inst_) {
      return entry.type;
    }
  }
  return error_type();
}

const analyzer::CheckedModule::CallTarget* Lowerer::call_target(
    ast::ExprIdx callee) const {
  for (const auto& entry : pkg.modules[module].call_targets) {
    if (entry.callee == callee && entry.inst == cur_inst_) {
      return &entry;
    }
  }
  return nullptr;
}

// Comp formal positions of a function item, in order.
std::vector<u32> Lowerer::comp_positions(ast::ItemIdx item) const {
  std::vector<u32> positions;
  if (!item.is_valid()) {
    return positions;
  }
  const ast::ItemNode& node = ast.items[item];
  if (node.kind != ast::ItemKind::Fn) {
    return positions;
  }
  const std::span<const ast::ItemFnParam> params =
      node.payload.get<ast::ItemFn>().params;
  for (u32 i = 0; i < static_cast<u32>(params.size()); ++i) {
    if (params[i].is_comp) {
      positions.push_back(i);
    }
  }
  return positions;
}

// Deterministic specialization key over comp argument values.
void Lowerer::comp_key_into(std::string& key, const CompValue& value) {
  switch (value.tag) {
    case CompValue::Tag::Void: key += "v;"; return;
    case CompValue::Tag::Int:
      key += "i" + std::to_string(value.int_value) + ";";
      return;
    case CompValue::Tag::Bool: key += value.bool_value ? "t;" : "f;"; return;
    case CompValue::Tag::Str:
      key += "s" + std::to_string(value.str_value.size()) + ":";
      key += value.str_value;
      key.push_back(';');
      return;
    case CompValue::Tag::Tuple:
      key += "t(";
      for (const CompValue& field : value.fields) {
        comp_key_into(key, field);
      }
      key += ");";
      return;
    case CompValue::Tag::Array:
      key += "A(";
      for (const CompValue& field : value.fields) {
        comp_key_into(key, field);
      }
      key += ");";
      return;
    case CompValue::Tag::Struct:
      key += "S(";
      for (const CompValue& field : value.fields) {
        comp_key_into(key, field);
      }
      key += ");";
      return;
    case CompValue::Tag::Enum:
      key += "e" + std::to_string(value.variant) + "(";
      for (const CompValue& field : value.fields) {
        comp_key_into(key, field);
      }
      key += ");";
      return;
  }
}

// Finds or reserves the function index for (item, signature,
// comp arguments), enqueueing lowering work on first encounter.
// Recursive calls see the reserved index, so bodies may reference
// themselves.
ir::FunctionIdx Lowerer::fn_index(u32 mod,
                                  ast::ItemIdx item,
                                  std::string_view name,
                                  const std::vector<ir::TypeIdx>& params,
                                  ir::TypeIdx ret,
                                  u32 inst,
                                  std::vector<CompVal> comp_args) {
  std::string key = std::to_string(item.idx) + "|";
  for (ir::TypeIdx param : params) {
    key += std::to_string(param.idx) + ",";
  }
  key += "|" + std::to_string(ret.idx) + "|";
  for (const CompVal& arg : comp_args) {
    comp_key_into(key, arg.value);
  }
  for (const FnEntry& entry : fns) {
    if (entry.item == item && entry.comp_key == key) {
      return entry.idx;
    }
  }
  static constexpr usize kMaxFnEntries = 8192;
  if (fns.size() >= kMaxFnEntries) {
    internal(diag::Span{}, "function specialization budget exhausted");
    return ir::FunctionIdx(base::kInvalidIdx);
  }
  const ir::FunctionIdx idx(static_cast<u32>(fns.size()));
  FnEntry entry;
  entry.item = item;
  entry.comp_key = std::move(key);
  entry.idx = idx;
  entry.mod = mod;
  entry.name = std::string(name);
  entry.params = params;
  entry.ret = ret;
  entry.inst = inst;
  entry.comp_args = std::move(comp_args);
  fns.push_back(std::move(entry));
  worklist_.push_back(fns.size() - 1);
  return idx;
}

// Index of a generic instantiation, or kNoInst when the type is not
// a generic instantiation.
u32 Lowerer::generic_inst_index(ir::TypeIdx type) const {
  for (u32 i = 0; i < static_cast<u32>(pkg.generic_insts.size()); ++i) {
    if (pkg.generic_insts[i].idx == type.idx) {
      return i;
    }
  }
  return analyzer::kNoInst;
}

// Lowering context of a call target: the checker recorded the
// instantiation under which the callee body was checked; that is
// exactly the context its body reads side tables in.
// Lowering context of a call target: the instantiation the callee
// body was checked under. Methods derive it from the receiver's
// instance type; free functions carry it on their signature. The
// target's own `inst` records the *caller's* context, so it cannot
// answer this.
u32 Lowerer::callee_inst(
    const analyzer::CheckedModule::CallTarget* target) const {
  if (target == nullptr) {
    return analyzer::kNoInst;
  }
  if (target->is_method) {
    const analyzer::CheckedModule& def = pkg.modules[target->module];
    if (target->index >= def.methods.size()) {
      return analyzer::kNoInst;
    }
    return generic_inst_index(def.methods[target->index].self_type);
  }
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  if (target->index >= def.functions.size()) {
    return analyzer::kNoInst;
  }
  return def.functions[target->index].inst;
}

ir::TypeIdx Lowerer::type_origin(ir::TypeIdx type) const {
  for (usize i = pkg.type_origins.size(); i > 0; --i) {
    if (pkg.type_origins[i - 1].first.idx == type.idx) {
      return pkg.type_origins[i - 1].second;
    }
  }
  return type;
}

const analyzer::CheckedModule::StructInfo* Lowerer::struct_info(
    ir::TypeIdx type) {
  const ir::TypeIdx origin = type_origin(type);
  for (const auto& checked : pkg.modules) {
    for (const auto& info : checked.structs) {
      if (info.type.idx == origin.idx) {
        return &info;
      }
    }
  }
  return nullptr;
}

void Lowerer::lower_fn(const FnEntry& entry) {
  const u32 mod = entry.mod;
  module = mod;
  cur_inst_ = entry.inst;
  comp_inst_ = entry.inst;
  locals.clear();
  comp_scope_.clear();
  fn_blocks_.clear();
  streams_.clear();
  stream_last_.clear();
  fn_block_base_ = block_next_;
  break_targets_.clear();
  continue_targets_.clear();
  pending_params_.clear();

  const ast::ItemNode& item = ast.items[entry.item];
  const ast::ItemFn& fn = item.payload.get<ast::ItemFn>();
  if (!entry.item.is_valid() || !fn.body.is_valid()) {
    internal(!entry.item.is_valid() ? diag::Span{} : item.span,
             "function without body");
    return;
  }
  const std::vector<u32> comp = comp_positions(entry.item);
  switch_to(reserve_block());
  cur_span_ = fn.name.span;
  // Entry block parameters arrive in declaration order, skipping
  // comp parameters (their values ride the specialization).
  ir::BlockParamSeq param_seq;
  usize comp_at = 0;
  for (usize i = 0; i < entry.params.size() && !failed; ++i) {
    if (comp_at < comp.size() && comp[comp_at] == i) {
      ++comp_at;
      continue;
    }
    const ir::RegisterIdx reg = claim_reg();
    builder.reg({.type = entry.params[i],
                 .def_idx = ir::InstructionIdx(base::kInvalidIdx)});
    param_seq.push(builder.block_param({.type = entry.params[i], .reg = reg}));
    pending_params_.push_back(reg);
  }
  pending_block_params_ = param_seq.finish();
  if (failed) {
    return;
  }
  // Bind parameters (patterns may destructure) after allocas exist.
  binding_param_ = true;
  const std::span<const ast::ItemFnParam> params = fn.params;
  usize pending_at = 0;
  comp_at = 0;
  usize comp_arg_at = 0;
  for (usize i = 0; i < params.size() && !failed; ++i) {
    const ir::TypeIdx ptype = entry.params[i];
    if (comp_at < comp.size() && comp[comp_at] == i) {
      ++comp_at;
      // Comp parameters bind their specialized constants for nested
      // comp evaluation. Like comp declarations they take no runtime
      // addresses; runtime reads splice through comp_scope_.
      const CompVal& arg = entry.comp_args[comp_arg_at++];
      CompScope scope;
      scope.frames.emplace_back();
      if (!comp_bind_pattern(mod, params[i].pattern, arg, scope,
                             fn.name.span)) {
        return;
      }
      for (auto& binding : scope.frames.back()) {
        comp_scope_.push_back(std::move(binding));
      }
      continue;
    }
    const ir::RegisterIdx preg = pending_params_[pending_at++];
    Val param{to_operand(preg, ptype), ptype, false, true};
    // Parameters live in memory like locals so borrows observe them.
    if (tag_of(ptype) == ir::TypeTag::Void) {
      bind_pattern(params[i].pattern, param);
      continue;
    }
    const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, ptype, {size_one});
    emit_void(ir::Opcode::Store,
              {to_operand(preg, ptype), to_operand(addr, ptype)});
    bind_pattern(params[i].pattern,
                 Val{to_operand(addr, ptype), ptype, true, true});
  }
  pending_params_.clear();
  binding_param_ = false;
  if (failed) {
    return;
  }
  Val body = lower_block(fn.body, nullptr);
  if (failed) {
    return;
  }
  if (!terminated_cur()) {
    if (tag_of(body.type) == ir::TypeTag::Void) {
      emit_void(ir::Opcode::Ret, {});
    } else if (tag_of(body.type) == ir::TypeTag::Never) {
      emit_void(ir::Opcode::Unreachable, {});
    } else {
      emit_void(ir::Opcode::Ret, {use_value(body)});
    }
  }
}

void Lowerer::run() {
  block_next_ = static_cast<u32>(builder.state().blocks.size());
  // Seed one-time operands before any function body runs.
  {
    const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
    ir::Immutable one{.type = i32, .data = {}};
    one.data.i32_value = 1;
    size_one = to_operand(builder.immutable(one), i32);
    ir::Immutable zero{.type = i32, .data = {}};
    zero.data.i32_value = 0;
    zero_i32 = to_operand(builder.immutable(zero), i32);
  }
  // Seed every checked non-comp signature so bodies without call
  // sites still reach codegen. A generic impl contributes one
  // signature per instantiation, keyed by its self type.
  for (u32 m = 0; m < static_cast<u32>(pkg.modules.size()); ++m) {
    const analyzer::CheckedModule& checked = pkg.modules[m];
    for (const auto& method : checked.methods) {
      if (!method.item.is_valid() || !comp_positions(method.item).empty() ||
          ast.items[method.item].kind == ast::ItemKind::Intrinsic) {
        continue;
      }
      const u32 inst = generic_inst_index(method.self_type);
      fn_index(m, method.item, method.name, method.params, method.ret, inst,
               {});
      if (failed) {
        return;
      }
    }
    for (const auto& sig : checked.functions) {
      if (!sig.item.is_valid() || !comp_positions(sig.item).empty() ||
          ast.items[sig.item].kind == ast::ItemKind::Intrinsic) {
        continue;
      }
      bool is_method_copy = false;
      for (const auto& method : checked.methods) {
        if (method.item == sig.item) {
          is_method_copy = true;
          break;
        }
      }
      if (is_method_copy) {
        continue;
      }
      // Generic free functions reserve per instantiation on first
      // call, like comp specializations; seeding them here would
      // lower one body under the wrong instantiation key.
      if (ast.items[sig.item].kind == ast::ItemKind::Fn &&
          !ast.items[sig.item].payload.get<ast::ItemFn>().generic.empty()) {
        continue;
      }
      fn_index(m, sig.item, sig.name, sig.params, sig.ret, analyzer::kNoInst,
               {});
      if (failed) {
        return;
      }
    }
  }
  // Lower bodies, then publish functions in reservation order so
  // the pre-assigned indexes line up with storage positions.
  struct Done {
    FnEntry entry;
    std::vector<ir::BlockIdx> blocks;
  };
  std::vector<Done> done;
  for (usize w = 0; w < worklist_.size() && !failed; ++w) {
    const FnEntry entry = fns[worklist_[w]];
    lower_fn(entry);
    if (failed) {
      return;
    }
    // Publish reserved blocks in reservation order; entry block
    // carries the recorded parameters.
    std::vector<ir::BlockIdx> blocks;
    bool first = true;
    for (usize i = 0; i < fn_blocks_.size(); ++i) {
      ir::BlockIdx created =
          builder.block({.instrs = streams_[i].finish(),
                         .block_params = first ? pending_block_params_
                                               : ir::BlockParamIdxRange{}});
      DCHECK(created.idx == fn_blocks_[i].idx);
      first = false;
      blocks.push_back(created);
    }
    pending_block_params_ = ir::BlockParamIdxRange{};
    done.push_back({entry, std::move(blocks)});
  }
  if (failed) {
    return;
  }
  for (const Done& entry : done) {
    if (pkg.tree.modules[entry.entry.mod]->is_prelude) {
      ++prelude_functions_;
    }
  }
  for (const Done& entry : done) {
    const std::vector<u32> comp = comp_positions(entry.entry.item);
    ir::TypeSeq params;
    usize comp_at = 0;
    for (usize i = 0; i < entry.entry.params.size(); ++i) {
      if (comp_at < comp.size() && comp[comp_at] == i) {
        ++comp_at;
        continue;
      }
      params.push(builder.ref_type(entry.entry.params[i]));
    }
    ir::BlockIdxRange range{entry.blocks.front(),
                            static_cast<u32>(entry.blocks.size())};
    builder.function({.meta = {.return_type = entry.entry.ret,
                               .param_types = params.finish(),
                               .name = strings.intern(entry.entry.name)},
                      .blocks = range});
  }
}

ir::Storage Lowerer::finish() && {
  return std::move(builder).build();
}
diag::Fallible<LoweredPackage> lower_package(analyzer::CheckedPackage package,
                                             ir::PointerWidth width,
                                             ast::AstArena& ast,
                                             str::StringInterner& strings,
                                             diag::DiagBag& bag) {
  Lowerer lowerer(std::move(package), width, ast, strings, bag);
  lowerer.run();
  if (lowerer.failed) {
    return base::make_err(diag::Fatal{});
  }
  // Tables leave before the builder moves; aggregate init stays whole.
  std::vector<diag::Span> spans = std::move(lowerer.instr_spans_);
  std::vector<LoweredPackage::AddrInfo> addrs = std::move(lowerer.addr_names_);
  const usize prelude_functions = lowerer.prelude_functions_;
  ir::Storage storage = std::move(lowerer).finish();
  if (base::Result<void, ir::VerifyError> result = ir::verify_storage(storage);
      result.is_err()) {
    ir::VerifyError error = std::move(result).unwrap_err();
    const u32 index = bag.emit(diag::Severity::Error, kLowerInternal,
                               diag::Span{}, "lowered IR failed verification");
    (void)index;
    const diag::Diagnostic diag = ir::to_diagnostic(error);
    fmt::memory_buffer rendered;
    diag::render(diag, rendered);
    DLOG("verify failure: {} at index {}",
         std::string_view(rendered.data(), rendered.size()), error.index);
    return base::make_err(diag::Fatal{});
  }
  // Tables outlive the builder move above; the Lowerer shell is empty.
  LoweredPackage lowered{std::move(storage), std::move(spans), std::move(addrs),
                         prelude_functions};
  return base::make_ok(std::move(lowered));
}

}  // namespace lower
