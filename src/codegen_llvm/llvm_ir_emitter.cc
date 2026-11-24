// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/llvm_ir_emitter.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codegen_llvm/common.h"
#include "codegen_llvm/target.h"
#include "config/build_config.h"
#include "debug/dcheck.h"
#include "debug/dlog.h"
#include "debug/fatal.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/str/string_pool_id.h"
#include "ir/block.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "ir/type_util.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "overlay/overlay.h"
#include "source/source.h"
#include "symbol/mangle.h"

#if BUILD_FLAG(IS_DEBUG)
#include "ir/formatter.h"  // IWYU pragma: keep
#endif

namespace codegen_llvm {

LlvmIrEmitter::LlvmIrEmitter(
    llvm::Module* module,
    ir::VerifiedStorage storage,
    ir::SymbolTable* interner,
    const Target& target,
    bool emit_entry,
    std::optional<std::span<const ir::FunctionIdx>> exported_functions,
    bool freestanding,
    const overlay::Overlay* overlay,
    debug::Profiler* profiler,
    bool backtrace,
    std::span<const diag::Span> instr_spans,
    std::span<const str::StringPoolId> site_names)
    : module_(module),
      storage_(std::move(storage)),
      builder_(std::make_unique<IRBuilder>(module_->getContext())),
      interner_(interner),
      width_(target.width),
      triple_(target.triple),
      profiler_(profiler),
      emit_entry_(emit_entry),
      exported_functions_(exported_functions),
      freestanding_(freestanding),
      overlay_(overlay),
      backtrace_(backtrace),
      instr_spans_(instr_spans),
      site_names_(site_names) {}

void LlvmIrEmitter::check_state() {
  DCHECK_MSG(module_, "LLVM Module is null");
  DCHECK_MSG(builder_, "LLVM IR Builder is null");
  DCHECK_MSG(interner_, "String Interner is null");
}

llvm::Value* LlvmIrEmitter::argument_value(const ir::Operand& op,
                                           llvm::Type* wanted) {
  const ir::TypeTag tag = storage_->types()[op.type.idx].tag;
  if (tag == ir::TypeTag::Void || tag == ir::TypeTag::Never) {
    return llvm::UndefValue::get(wanted);
  }
  llvm::Value* value = resolve_operand_value(op);
  if (value->getType() != wanted) {
    // The unit type lowers to a dummy operand, so the parameter's own
    // zero-sized type is the value that belongs here.
    if (auto* structure = llvm::dyn_cast<llvm::StructType>(wanted);
        structure != nullptr && structure->getNumElements() == 0) {
      return llvm::UndefValue::get(wanted);
    }
  }
  return value;
}

llvm::Type* LlvmIrEmitter::return_type(ir::TypeIdx idx) const {
  const ir::TypeTag tag = storage_->types()[idx].tag;
  // A function's return position, and only that position, spells the
  // unit and uninhabited types as LLVM's `void`.
  if (tag == ir::TypeTag::Void || tag == ir::TypeTag::Never) {
    return builder_->getVoidTy();
  }
  return type(idx);
}

llvm::Type* LlvmIrEmitter::type(ir::TypeIdx idx) const {
  if (type_cache_.size() <= idx.idx) {
    type_cache_.resize(storage_->types().size(), nullptr);
  }
  if (llvm::Type* cached = type_cache_[idx.idx]) {
    return cached;
  }
  llvm::Type* built = build_type(idx);
  type_cache_[idx.idx] = built;
  return built;
}

// A zero-length field of a wide integer raises a struct's alignment
// without changing its allocation size. LLVM's struct types here do
// not carry an explicit alignment attribute, so this is how an
// `#[layout(min_align = N)]` reaches the emitted layout.
llvm::Type* LlvmIrEmitter::alignment_field(u64 align) const {
  llvm::Type* wide = llvm::IntegerType::get(module_->getContext(),
                                            static_cast<u32>(align * 8));
  return llvm::ArrayType::get(wide, 0);
}

llvm::Type* LlvmIrEmitter::build_type(ir::TypeIdx idx) const {
  const ir::TypeNode& node = storage_->types()[idx];
  const ir::TypeTag tag = node.tag;
  using T = ir::TypeTag;

  switch (tag) {
    case T::Void:
      // The unit type is a value of no bytes, not an unsized hole: a
      // field or an element holds it, and the layout gives it size
      // zero and alignment one. Only a return position wants void, and
      // `return_type` is what asks for that.
      return llvm::StructType::get(module_->getContext(), {});
    case T::Never:
      // Uninhabited, but a field or an element still has to hold a
      // type of the layout's size zero; a return position, the one
      // place LLVM wants void, goes through `return_type`.
      return llvm::StructType::get(module_->getContext(), {});
    case T::I1: return builder_->getInt1Ty();
    case T::I8:
    case T::U8:
    case T::Char: return builder_->getInt8Ty();
    case T::I16:
    case T::U16: return builder_->getInt16Ty();
    case T::I32:
    case T::U32:
    case T::Rune: return builder_->getInt32Ty();
    case T::I64:
    case T::U64: return builder_->getInt64Ty();
    case T::Isize:
    case T::Usize:
      return width_ == ir::PointerWidth::W64 ? builder_->getInt64Ty()
                                             : builder_->getInt32Ty();
    // case T::I128: return builder_->getInt128Ty();
    case T::F32: return builder_->getFloatTy();
    case T::F64: return builder_->getDoubleTy();
    case T::Str: {
      // Fat pointer: {bytes, len}. Length rides the target width;
      // storage may carry a trailing NUL, which the length excludes.
      llvm::Type* len_ty = width_ == ir::PointerWidth::W64
                               ? builder_->getInt64Ty()
                               : builder_->getInt32Ty();
      return llvm::StructType::get(module_->getContext(),
                                   {builder_->getPtrTy(), len_ty});
    }
    case T::Slice: {
      // Fat pointer: {bytes, len}. Same shape as `str`; only the
      // element type differs, and the length still rides the target.
      llvm::Type* len_ty = width_ == ir::PointerWidth::W64
                               ? builder_->getInt64Ty()
                               : builder_->getInt32Ty();
      return llvm::StructType::get(module_->getContext(),
                                   {builder_->getPtrTy(), len_ty});
    }
    case T::Ptr: return builder_->getPtrTy();
    case T::RawPtr:
    case T::RawMutPtr: return builder_->getPtrTy();
    case T::Ref:
    case T::MutRef: {
      // A reference to a slice is the fat pointer itself, matching the
      // layout above; every other reference rides as a thin pointer.
      const ir::TypeIdx pointee = storage_->ref_types()[node.as_ref()].pointee;
      if (storage_->types()[pointee].tag == ir::TypeTag::Slice) {
        llvm::Type* len_ty = width_ == ir::PointerWidth::W64
                                 ? builder_->getInt64Ty()
                                 : builder_->getInt32Ty();
        return llvm::StructType::get(module_->getContext(),
                                     {builder_->getPtrTy(), len_ty});
      }
      return builder_->getPtrTy();
    }
    case T::Struct: {
      const ir::StructType& struct_type =
          storage_->struct_types()[node.as_struct()];
      llvm::SmallVector<llvm::Type*, FUNCTION_ARGS_SOO_SIZE> field_types;
      field_types.reserve(struct_type.fields.size());
      for (u32 fi = 0; fi < struct_type.fields.size(); ++fi) {
        const ir::TypeIdx field = struct_type.fields[fi];
        llvm::Type* field_ty = type(field);
        const ir::TypeLayoutHintsIdxRange& hint_range = struct_type.field_hints;
        const u32 hint_idx = hint_range.head().idx + fi;
        if (fi < hint_range.size() &&
            hint_idx < storage_->state().layout_hints.size()) {
          const ir::TypeLayout natural =
              ir::type_layout(storage_->state(), field, width_);
          const u8 cap = storage_->state().layout_hints[hint_idx].max_align;
          if (cap != 0 && natural.align > cap) {
            field_ty = llvm::ArrayType::get(builder_->getInt8Ty(),
                                            static_cast<u64>(natural.size));
          }
        }
        field_types.emplace_back(field_ty);
      }
      // Anonymous structural type; LLVM deduplicates identical shapes.
      const ir::TypeLayout expected =
          ir::type_layout(storage_->state(), idx, width_);
      if (struct_type.hints.min_align != 0) {
        field_types.emplace_back(alignment_field(expected.align));
      }
      return llvm::StructType::get(module_->getContext(), field_types);
    }
    case T::Array: {
      const ir::ArrayType& array_type =
          storage_->array_types()[node.as_array()];
      return llvm::ArrayType::get(type(array_type.element), array_type.count);
    }
    case T::Tuple: {
      const ir::TupleType& tuple_type =
          storage_->tuple_types()[node.as_tuple()];
      llvm::SmallVector<llvm::Type*, FUNCTION_ARGS_SOO_SIZE> element_types;
      element_types.reserve(tuple_type.elements.size());
      for (const ir::TypeIdx element : tuple_type.elements) {
        element_types.emplace_back(type(element));
      }
      return llvm::StructType::get(module_->getContext(), element_types);
    }
    case T::Func: {
      // Code plus environment, whatever the signature says: two
      // words, matching the layout ir::type_layout publishes.
      return llvm::StructType::get(
          module_->getContext(), {builder_->getPtrTy(), builder_->getPtrTy()});
    }
    case T::Function: {
      // A bare code pointer: signatures live on function values
      // and call sites, never here.
      return builder_->getPtrTy();
    }
    case T::Enum: {
      const ir::EnumType& shape = storage_->state().enum_types[node.as_enum()];
      if (ir::enum_is_zst(storage_->state(), idx)) {
        llvm::SmallVector<llvm::Type*, 1> empty;
        if (shape.hints.min_align != 0) {
          empty.emplace_back(alignment_field(
              ir::type_layout(storage_->state(), idx, width_).align));
        }
        return llvm::StructType::get(module_->getContext(), empty);
      }
      // A niche-carrying enum is its payload: the discriminant lives in
      // the payload's spare pattern and takes no room of its own
      //. The layout check below holds the two against each
      // other, so a missing branch here fails the build.
      const ir::EnumNiche niche = ir::enum_niche(storage_->state(), idx);
      if (niche.has) {
        return type(niche.slot);
      }
      // A discriminant, then the payload in the slot itself. Keeping the
      // payload here rather than behind a pointer is what lets an enum
      // value outlive the frame that built it.
      const u64 disc = ir::enum_tag_width(storage_->state(), idx);
      llvm::Type* tag_ty = builder_->getInt32Ty();
      if (disc == 1) {
        tag_ty = builder_->getInt8Ty();
      } else if (disc == 2) {
        tag_ty = builder_->getInt16Ty();
      } else if (disc == 8) {
        tag_ty = builder_->getInt64Ty();
      }
      llvm::SmallVector<llvm::Type*, 2> slot_types;
      slot_types.emplace_back(tag_ty);
      slot_types.emplace_back(enum_payload_area_type(idx));
      // The layout ir::type_layout publishes is what field offsets are
      // computed against, so the type built here has to match it. The
      // module carries the target's layout before the emitter runs, so
      // this checks the two against each other on every build rather
      // than waiting for a layout to appear.
      const ir::TypeLayout expected =
          ir::type_layout(storage_->state(), idx, width_);
      const llvm::DataLayout& dl = module_->getDataLayout();
      DCHECK(!dl.isDefault());
      if (shape.hints.min_align != 0) {
        slot_types.emplace_back(alignment_field(expected.align));
      }
      llvm::StructType* slot =
          llvm::StructType::get(module_->getContext(), slot_types);
      DCHECK_EQ(dl.getTypeAllocSize(slot).getFixedValue(), expected.size);
      DCHECK_EQ(dl.getABITypeAlign(slot).value(), expected.align);
      return slot;
    }
    default: {
      DLOG("Unsupported type: {}", tag);
      DCHECK(false);
      UNREACHABLE();
    }
  }
}

// The payload half of an enum's slot: a byte area on the narrowest
// carrier primitive that carries the alignment ir::type_layout
// published. A plain byte array would be align 1, which stores correctly
// on x86 and is still wrong on a strict-alignment target. Taking the
// carrier from the published alignment is what makes LLVM lay the slot
// out the way ir::type_layout sizes it; the check in `type` holds the
// two against each other now that the module carries a layout before the
// emitter runs.
llvm::Type* LlvmIrEmitter::enum_payload_area_type(ir::TypeIdx idx) const {
  const ir::TypeLayout area =
      ir::enum_payload_area(storage_->state(), idx, width_);
  llvm::Type* carrier = builder_->getInt8Ty();
  u64 carrier_bytes = 1;
  if (area.align > 8) {
    carrier = llvm::ArrayType::get(builder_->getInt32Ty(), 4);
    carrier_bytes = 16;
  } else if (area.align == 8) {
    carrier = builder_->getInt64Ty();
    carrier_bytes = 8;
  } else if (area.align == 4) {
    carrier = builder_->getInt32Ty();
    carrier_bytes = 4;
  } else if (area.align == 2) {
    carrier = builder_->getInt16Ty();
    carrier_bytes = 2;
  }
  DCHECK_EQ(area.size % carrier_bytes, 0u);
  return llvm::ArrayType::get(carrier, area.size / carrier_bytes);
}

EmittedBacktrace LlvmIrEmitter::emit() && noexcept {
  check_state();

  values_.resize_functions(storage_->functions().size());
  values_.resize_registers(storage_->registers().size());
  values_.resize_blocks(storage_->blocks().size());
  values_.resize_immutables(storage_->immutables().size());
  values_.resize_external_functions(storage_->external_functions().size());
  values_.resize_alloca_types(storage_->registers().size());
  values_.resize_slot_types(storage_->registers().size());

  setup_immutables();
  setup_external_functions();

  if (backtrace_) {
    // The slots the panic walker loads, filled once the runtime exists.
    // They have to exist before the runtime builds, because the walker
    // bakes the load in and a table defined later only fills them.
    for (std::string_view name :
         {"alcy_backtrace_entries", "alcy_callsite_entries"}) {
      llvm::GlobalVariable* slot =
          module_->getOrInsertGlobal(std::string(name), builder_->getPtrTy());
      slot->setLinkage(llvm::GlobalValue::PrivateLinkage);
      slot->setInitializer(
          llvm::ConstantPointerNull::get(builder_->getPtrTy()));
    }
  }

  // A parametric function never reaches a backend: the specialization
  // pass clones it per instantiation, and its types would
  // not build here.
  std::vector<bool> parametric(storage_->functions().size(), false);
  for (const ir::FunctionIdx function_idx : storage_->parametric_functions()) {
    parametric[function_idx.idx] = true;
  }
  // Which functions a library's surface names: the only definitions a
  // dependent can reach by symbol. A binary internalizes everything.
  std::vector<bool> exported(storage_->functions().size(), false);
  if (exported_functions_.has_value()) {
    for (const ir::FunctionIdx function_idx : *exported_functions_) {
      if (function_idx.is_valid() && function_idx.idx < exported.size()) {
        exported[function_idx.idx] = true;
      }
    }
  }
  // Which functions the image can still reach: the entry a binary
  // names or the surface a library exports, and everything they call
  // or reference as a value. A function nothing reaches is one the
  // backend would translate for nothing - the middle end drops it in a
  // profile that has one, and a profile without a middle end pays for
  // every body here. A relocatable module keeps every symbol, so it
  // keeps every function too.
  std::vector<bool> reachable;
  if (emit_entry_ || exported_functions_.has_value()) {
    reachable = reachable_functions(parametric);
  }
  const auto kept = [&](const ir::FunctionIdx function_idx) {
    return reachable.empty() || reachable[function_idx.idx];
  };

  // Declare every function before defining any body so forward
  // calls resolve regardless of declaration order.
  llvm::Function* entry_function = nullptr;
  ir::TypeIdx entry_return = ir::TypeIdx::invalid();
  for (const ir::FunctionIdx function_idx : storage_->functions().idx_range()) {
    if (parametric[function_idx.idx] || !kept(function_idx)) {
      continue;
    }
    const ir::Function& function = storage_->functions()[function_idx];

    llvm::Function* llvm_function = nullptr;
    if (emit_entry_ && entry_function == nullptr &&
        is_entry_candidate(function)) {
      // The user entry becomes an implementation detail; a C-ABI
      // `main` below adapts its return form to an exit code.
      ir::FunctionMeta renamed = function.meta;
      renamed.name = interner_->intern("alcy_main");
      llvm_function = create_function(renamed);
      entry_function = llvm_function;
      entry_return = function.meta.return_type;
      if (backtrace_) {
        // The optimizer would fold the entry into its wrapper and
        // leave the walk no named frame at all; one missed inlining
        // on a function called once is what keeps `main` standing.
        llvm_function->addFnAttr(llvm::Attribute::NoInline);
      }
    } else {
      llvm_function = create_function(function.meta);
    }
    values_.add_function(function_idx, llvm_function);
    // A binary defines the whole image: nothing outside can call these
    // symbols, so the middle end may drop the instantiations no caller
    // reached. A library keeps its surface external.
    if (emit_entry_) {
      llvm_function->setLinkage(llvm::GlobalValue::InternalLinkage);
    } else if (exported_functions_.has_value() && !exported[function_idx.idx]) {
      // A library's object carries code no dependent reaches by name:
      // only the surface stays external, so a symbol a split part
      // promotes cannot meet another object's copy of it. A
      // request that names no surface - a relocatable module, a test -
      // keeps every symbol as it is.
      llvm_function->setLinkage(llvm::GlobalValue::InternalLinkage);
    }
    if (backtrace_) {
      // The renamed entry keeps its source spelling: a frame reads
      // `main`, not `alcy_main`.
      visible_functions_.push_back(
          BacktraceName{llvm_function, display_name(function.meta)});
    }
  }
  for (const ir::FunctionIdx function_idx : storage_->functions().idx_range()) {
    if (parametric[function_idx.idx] || !kept(function_idx)) {
      continue;
    }
    const ir::Function& function = storage_->functions()[function_idx];
    // Bodies emit independently: no cross-function state moves between
    // iterations.
    // The name is the source spelling: two instantiations of one generic
    // read as repeat reports of the same name rather than as distinct
    // work, so an instantiation-specific cost stays attributable rather
    // than split across rows.
    {
      PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(
          profiler_, interner_->get(function.meta.name), "emit-fn");
      emit_function(values_.function(function_idx), function);
    }
  }
  if (entry_function != nullptr) {
    emit_entry(entry_function, entry_return);
  }

#if BUILD_FLAG(IS_DEBUG)
  // Storage arrives verified, so only the generated module is rechecked.
  if (llvm::verifyModule(*module_, &llvm::errs())) [[unlikely]] {
    DLOG("LLVM verify module failed");
    module_->print(llvm::errs(), nullptr);
    UNREACHABLE();
  }
#endif
  return {std::move(visible_functions_), std::move(call_sites_)};
}

void LlvmIrEmitter::emit_function(llvm::Function* llvm_function,
                                  const ir::Function& function) {
  check_state();
  // A function with no blocks is a declaration: the artifact the link
  // brings defines it.
  if (function.blocks.empty()) {
    return;
  }

  for (const ir::BlockIdx block_idx : function.blocks) {
    values_.add_block(block_idx, llvm::BasicBlock::Create(module_->getContext(),
                                                          "", llvm_function));
  }

  // Pre-generate phi nodes. The entry block (first in function
  // order) receives LLVM function arguments directly instead of
  // PHIs: lowering places one block parameter per declared parameter.
  //
  // A large aggregate carries no value through either path. The caller
  // names a slot for the result ahead of the parameters, and passes each
  // large argument as its slot, so the entry block copies what it was
  // handed into a slot of its own and every later block reaches a join
  // parameter through the slot the branch fills.
  const bool indirect_ret = in_memory_aggregate(function.meta.return_type);
  return_slot_ = indirect_ret ? llvm_function->getArg(0) : nullptr;
  u32 arg_no = indirect_ret ? 1 : 0;
  for (const ir::BlockIdx block_idx : function.blocks) {
    const ir::Block& block = storage_->blocks()[block_idx];
    llvm::BasicBlock* llvm_block = values_.block(block_idx);
    const bool is_entry = block_idx.idx == function.blocks.head().idx;

    builder_->SetInsertPoint(llvm_block);
    for (const ir::BlockParamIdx param_id : block.block_params) {
      const ir::BlockParam& param = storage_->block_params()[param_id];

      if (is_entry) {
        llvm::Value* arg = llvm_function->getArg(arg_no++);
        if (in_memory_aggregate(param.type)) {
          llvm::Type* param_type = type(param.type);
          llvm::AllocaInst* slot = frame_slot(param_type);
          builder_->CreateMemCpy(slot, llvm::MaybeAlign(), arg,
                                 llvm::MaybeAlign(),
                                 builder_->getInt64(memory_size(param_type)));
          values_.add_register(param.reg, slot);
          values_.add_slot_type(param.reg, param_type);
          continue;
        }
        values_.add_register(param.reg, arg);
        continue;
      }
      if (in_memory_aggregate(param.type)) {
        llvm::Type* param_type = type(param.type);
        values_.add_register(param.reg, frame_slot(param_type));
        values_.add_slot_type(param.reg, param_type);
        continue;
      }
      llvm::PHINode* phi = builder_->CreatePHI(type(param.type), 0);
      values_.add_register(param.reg, phi);
    }
  }

  // Emit in control-flow order from the entry, not index order: a
  // register's definition must be emitted before the block that reads
  // it, and a pass that inserts blocks after lowering leaves some
  // definitions in blocks with higher indexes than their uses.
  std::vector<ir::BlockIdx> pending{function.blocks.head()};
  std::vector<bool> emitted(storage_->blocks().size(), false);
  while (!pending.empty()) {
    const ir::BlockIdx block_idx = pending.back();
    pending.pop_back();
    if (emitted[block_idx.idx]) {
      continue;
    }
    emitted[block_idx.idx] = true;
    const ir::Block& block = storage_->blocks()[block_idx];
    builder_->SetInsertPoint(values_.block(block_idx));
    emit_block(block);
    std::vector<ir::BlockIdx> succs;
    const base::IdxRange<ir::InstructionIdx> range = block.instrs;
    if (!range.empty()) {
      const ir::Instruction& last = storage_->instrs()[range[range.size() - 1]];
      for (ir::OperandIdx o : last.operands) {
        const ir::Operand& operand = storage_->operands()[o.idx];
        if (operand.is<ir::BlockIdx>()) {
          succs.push_back(operand.as_block());
        }
      }
    }
    // Reversed so the first branch target is emitted first.
    for (usize i = succs.size(); i > 0; --i) {
      pending.push_back(succs[i - 1]);
    }
  }
  // A block nothing branches to still needs its instructions emitted.
  for (const ir::BlockIdx block_idx : function.blocks) {
    if (emitted[block_idx.idx]) {
      continue;
    }
    const ir::Block& block = storage_->blocks()[block_idx];
    builder_->SetInsertPoint(values_.block(block_idx));
    emit_block(block);
  }
  // A split block keeps its old block's outgoing edge only through the
  // continuation, so successor PHIs still naming the old block would
  // read a predecessor that no longer reaches them.
  for (const auto& [from, to] : split_blocks_) {
    for (llvm::BasicBlock& block : *llvm_function) {
      for (llvm::Instruction& inst : block) {
        llvm::PHINode* phi = llvm::dyn_cast<llvm::PHINode>(&inst);
        if (phi == nullptr) {
          break;
        }
        for (u32 i = 0; i < phi->getNumIncomingValues(); ++i) {
          if (phi->getIncomingBlock(i) == from) {
            phi->setIncomingBlock(i, to);
          }
        }
      }
    }
  }
  split_blocks_.clear();

#if BUILD_FLAG(IS_DEBUG)
  if (llvm::verifyFunction(*llvm_function, &llvm::errs())) [[unlikely]] {
    DLOG("LLVM verify function failed");
    llvm_function->viewCFG();
    llvm_function->print(llvm::errs());
    UNREACHABLE();
  }
#endif
}

void LlvmIrEmitter::emit_block(const ir::Block& block) {
  check_state();

  for (const ir::InstructionIdx instr_idx : block.instrs) {
    const ir::Instruction& instr = storage_->instrs()[instr_idx];
    emit_instruction(instr_idx, instr);
  }
}

bool LlvmIrEmitter::has_call_site(ir::InstructionIdx instr_idx) const {
  if (!backtrace_) {
    return false;
  }
  if (instr_idx.idx >= instr_spans_.size() ||
      instr_idx.idx >= site_names_.size()) {
    return false;
  }
  if (instr_spans_[instr_idx.idx].file == source::UNKNOWN_FILE) {
    return false;
  }
  return site_names_[instr_idx.idx] != str::EMPTY_STRING_ID;
}

void LlvmIrEmitter::emit_sited_call(ir::InstructionIdx instr_idx,
                                    const ir::Instruction& instr) {
  llvm::LLVMContext& context = module_->getContext();
  llvm::BasicBlock* here = builder_->GetInsertBlock();
  llvm::Function* function = here->getParent();
  llvm::BasicBlock* call_block =
      llvm::BasicBlock::Create(context, "", function);
  llvm::BasicBlock* cont_block =
      llvm::BasicBlock::Create(context, "", function);
  builder_->CreateBr(call_block);
  builder_->SetInsertPoint(call_block);
  emit_control(instr);
  builder_->CreateBr(cont_block);
  builder_->SetInsertPoint(cont_block);
  call_sites_.push_back(CallSiteName{
      call_block, std::string(interner_->get(site_names_[instr_idx.idx]))});
  split_blocks_.emplace_back(here, cont_block);
}

const overlay::Entry* LlvmIrEmitter::overlay_entry(
    ir::InstructionIdx instr_idx) const {
  if (overlay_ == nullptr || instr_idx.idx >= overlay_->entries.size()) {
    return nullptr;
  }
  const overlay::Entry& entry = overlay_->entries[instr_idx.idx];
  return entry.kind == overlay::Check::None ? nullptr : &entry;
}

void LlvmIrEmitter::emit_check(const overlay::Entry& entry,
                               llvm::Value* condition,
                               std::string_view helper) {
  const std::string_view where = entry.site == str::EMPTY_STRING_ID
                                     ? std::string_view{}
                                     : interner_->get(entry.site);
  llvm::Value* text =
      builder_->CreateGlobalString(where, ".alcy.where", 0, module_);
  llvm::Type* usize = width_ == ir::PointerWidth::W64 ? builder_->getInt64Ty()
                                                      : builder_->getInt32Ty();
  builder_->CreateCall(
      check_function(helper),
      {condition, text,
       llvm::ConstantInt::get(usize, static_cast<u64>(where.size()))});
}

void LlvmIrEmitter::emit_check_prelude(const ir::Instruction& instr,
                                       const overlay::Entry& entry) {
  const ir::Operand& lhs = storage_->operands()[instr.operands.head()];
  llvm::Value* left = resolve_operand_value(lhs);
  llvm::Type* type = left->getType();
  switch (entry.kind) {
    case overlay::Check::ShiftRange: {
      llvm::Value* count =
          resolve_operand_value(storage_->operands()[entry.operand]);
      llvm::Value* width =
          llvm::ConstantInt::get(type, type->getIntegerBitWidth());
      emit_check(entry, builder_->CreateICmpUGE(count, width),
                 "alcy_check_shift");
      return;
    }
    case overlay::Check::MinOverOne: {
      llvm::Value* divisor =
          resolve_operand_value(storage_->operands()[entry.operand]);
      const u32 bits = type->getIntegerBitWidth();
      llvm::Value* minimum =
          llvm::ConstantInt::get(type, llvm::APInt::getSignedMinValue(bits));
      llvm::Value* minus_one =
          llvm::ConstantInt::get(type, llvm::APInt::getAllOnes(bits));
      llvm::Value* condition =
          builder_->CreateAnd(builder_->CreateICmpEQ(left, minimum),
                              builder_->CreateICmpEQ(divisor, minus_one));
      emit_check(entry, condition, "alcy_check_overflow");
      return;
    }
    default: return;
  }
}

llvm::Function* LlvmIrEmitter::check_function(std::string_view name) {
  const llvm::StringRef llvm_name(name.data(), name.size());
  if (llvm::Function* existing = module_->getFunction(llvm_name)) {
    return existing;
  }
  llvm::Type* usize = width_ == ir::PointerWidth::W64 ? builder_->getInt64Ty()
                                                      : builder_->getInt32Ty();
  llvm::FunctionType* type = llvm::FunctionType::get(
      builder_->getVoidTy(),
      {builder_->getInt1Ty(), builder_->getPtrTy(), usize}, false);
  return llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage,
                                llvm_name, module_);
}

void LlvmIrEmitter::emit_instruction(ir::InstructionIdx instr_idx,
                                     const ir::Instruction& instr) {
  check_state();

  using Op = ir::Opcode;

  if (backtrace_ && instr.op == Op::Call && has_call_site(instr_idx)) {
    emit_sited_call(instr_idx, instr);
    return;
  }
  // The build's checks: an overflow entry replaces the
  // operation's own emission with the with-overflow form, and the other
  // kinds emit their condition and the runtime call before it runs.
  if (const overlay::Entry* entry = overlay_entry(instr_idx);
      entry != nullptr) {
    if (entry->kind == overlay::Check::Overflow) {
      emit_checked_arith(instr, *entry);
      return;
    }
    emit_check_prelude(instr, *entry);
  }
  switch (instr.op) {
    case Op::Noop: break;

    case Op::IntAdd:
    case Op::IntSub:
    case Op::IntMul:
    case Op::IntAddWrap:
    case Op::IntSubWrap:
    case Op::IntMulWrap:
    case Op::IntDiv:
    case Op::UintDiv:
    case Op::IntRem:
    case Op::UintRem:
    case Op::FAdd:
    case Op::FSub:
    case Op::FMul:
    case Op::FDiv:
    case Op::FNeg:
    case Op::And:
    case Op::Or:
    case Op::Xor:
    case Op::ShiftLeft:
    case Op::ArithmeticShiftRight:
    case Op::LogicalShiftRight:
    case Op::Not:
    case Op::BitReverse:
    case Op::Eq:
    case Op::Ne:
    case Op::Le:
    case Op::Lt:
    case Op::Ge:
    case Op::Gt:
    case Op::TypeCast:
    case Op::TypeSizeOf:
    case Op::TypeAlignOf:
    case Op::Select:
    case Op::Move:
    case Op::Borrow:
    case Op::Drop: emit_compute(instr); break;

    case Op::Alloca:
    case Op::Load:
    case Op::Store:
    case Op::Memcopy:
    case Op::Memset:
    case Op::GetElementPtr:
    case Op::ElemOffset:
    case Op::EnumTagLoad:
    case Op::EnumTagSet:
    case Op::EnumFieldAddr:
    case Op::ParametricDrop:
    case Op::ExtractValue:
    case Op::InsertValue:
    case Op::AtomicLoad:
    case Op::AtomicStore:
    case Op::AtomicRmw:
    case Op::AtomicCompareExchange:
    case Op::Fence: emit_memory(instr); break;

    case Op::Br:
    case Op::CondBr:
    case Op::Switch:
    case Op::Call:
    case Op::Ret:
    case Op::Unreachable: emit_control(instr); break;

    default: {
      DLOG("Unknown opcode found: {}", instr.op);
      UNREACHABLE();
    }
  }
}

void LlvmIrEmitter::emit_control(const ir::Instruction& instr) {
  check_state();

  const ir::Instruction& i = instr;
  const ir::OperandIdxRange& ops = i.operands;

  using Op = ir::Opcode;

  switch (i.op) {
    case Op::Br: {
      // operands[0] = Target block
      // operands[1..N] = parameters
      DCHECK(!ops.empty());
      const ir::Operand& target_op = storage_->operands()[ops.head()];
      const ir::BlockIdx target_block_idx = target_op.as_block();
      llvm::BasicBlock* target_llvm_block = values_.block(target_block_idx);
      llvm::BasicBlock* current_llvm_block = builder_->GetInsertBlock();

      const ir::Block& target_block = storage_->blocks()[target_block_idx];
      DCHECK(ops.size() == 1 + target_block.block_params.size());
      // Branch arguments fill the target's parameters positionally:
      // operands[1..N] are the incoming values in parameter order. A
      // large aggregate parameter has no phi: its register names a slot
      // the branch fills, so the value reaches every successor that
      // joins through this block.
      u32 position = 0;
      for (const ir::BlockParamIdx param_idx : target_block.block_params) {
        const ir::BlockParam& param = storage_->block_params()[param_idx];
        const ir::Operand& arg_op =
            storage_->operands()[ops.head() + 1 + position];
        ++position;

        if (in_memory_aggregate(param.type)) {
          copy_to_slot(values_.register_value(param.reg), arg_op,
                       type(param.type));
          continue;
        }
        auto* phi =
            llvm::cast<llvm::PHINode>(values_.register_value(param.reg));
        phi->addIncoming(resolve_operand_value(arg_op), current_llvm_block);
      }
      builder_->CreateBr(target_llvm_block);
      break;
    }
    case Op::CondBr: {
      // operands = [cond, true_block, false_block]. Targets must not take
      // block parameters.
      DCHECK(ops.size() == 3);
      llvm::Value* cond =
          resolve_operand_value(storage_->operands()[ops.head()]);
      llvm::BasicBlock* true_block =
          values_.block(storage_->operands()[ops.head() + 1].as_block());
      llvm::BasicBlock* false_block =
          values_.block(storage_->operands()[ops.head() + 2].as_block());
      builder_->CreateCondBr(cond, true_block, false_block);
      break;
    }
    case Op::Switch: {
      // operands = [value, default_block, (case_imm, case_block)...].
      // Targets must not take block parameters.
      DCHECK(ops.size() >= 2);
      llvm::Value* value =
          resolve_operand_value(storage_->operands()[ops.head()]);
      llvm::BasicBlock* default_block =
          values_.block(storage_->operands()[ops.head() + 1].as_block());
      llvm::SwitchInst* switch_inst =
          builder_->CreateSwitch(value, default_block);
      for (u32 idx = 2; idx + 1 < ops.size(); idx += 2) {
        auto* case_value = llvm::cast<llvm::ConstantInt>(
            resolve_operand_value(storage_->operands()[ops.head() + idx]));
        llvm::BasicBlock* case_block = values_.block(
            storage_->operands()[ops.head() + idx + 1].as_block());
        switch_inst->addCase(case_value, case_block);
      }
      break;
    }
    case Op::Call: {
      DCHECK(ops.size() >= 1);

      const ir::Operand& callee_op = storage_->operands()[ops.head()];
      if (!callee_op.is<ir::FunctionIdx>() &&
          !callee_op.is<ir::ExternalFunctionIdx>()) {
        // A call through a function value: destructure code and
        // environment, then call with the environment first, the
        // way synthetic closure functions declare it.
        DCHECK(callee_op.is<ir::RegisterIdx>());
        llvm::Value* closure = resolve_operand_value(callee_op);
        const ir::TypeIdx closure_type =
            storage_->registers()[callee_op.as_register()].type;
        DCHECK(storage_->types()[closure_type.idx].tag == ir::TypeTag::Func);
        const ir::FuncType& sig =
            storage_
                ->func_types()[storage_->types()[closure_type.idx].as_func()];
        llvm::Value* code = builder_->CreateExtractValue(closure, {0u});
        llvm::Value* env = builder_->CreateExtractValue(closure, {1u});
        // The signature the call site spells must match the one the
        // synthetic function declares, result slot and argument slots
        // included.
        llvm::SmallVector<llvm::Type*, FUNCTION_ARGS_SOO_SIZE> param_types;
        const bool indirect_ret = in_memory_aggregate(sig.ret);
        param_types.reserve(sig.params.size() + 2);
        if (indirect_ret) {
          param_types.push_back(builder_->getPtrTy());
        }
        param_types.push_back(builder_->getPtrTy());
        for (const ir::TypeIdx param : sig.params) {
          param_types.push_back(parameter_type(param));
        }
        llvm::FunctionType* fn_type = llvm::FunctionType::get(
            indirect_ret ? builder_->getVoidTy() : return_type(sig.ret),
            param_types, false);
        u32 param_base = 0;
        llvm::SmallVector<llvm::Value*, FUNCTION_ARGS_SOO_SIZE> args;
        args.reserve(ops.size() + 1);
        if (llvm::AllocaInst* result_slot = call_result_slot(sig.ret, i.dst);
            result_slot != nullptr) {
          args.push_back(result_slot);
          ++param_base;
        }
        args.push_back(env);
        ++param_base;
        for (u32 idx = 1; idx < ops.size(); ++idx) {
          const ir::Operand& arg_op = storage_->operands()[ops.head() + idx];
          args.push_back(call_argument(arg_op, param_types[param_base]));
          ++param_base;
        }
        llvm::CallInst* call_inst = builder_->CreateCall(fn_type, code, args);
        if (i.dst.is_valid() && !indirect_ret) {
          values_.add_register(i.dst, call_inst);
        }
        break;
      }
      llvm::Function* callee_func = resolve_operand_function(callee_op);
      // An external declaration keeps the ABI it names: it takes scalars
      // and pointers only, and answers no slot.
      const bool own_abi = callee_op.is<ir::FunctionIdx>();

      llvm::SmallVector<llvm::Value*, FUNCTION_ARGS_SOO_SIZE> args;
      args.reserve(ops.size());
      llvm::AllocaInst* result_slot = nullptr;
      if (own_abi) {
        result_slot = call_result_slot(
            storage_->functions()[callee_op.as_function()].meta.return_type,
            i.dst);
      }
      u32 param_base = 0;
      if (result_slot != nullptr) {
        args.push_back(result_slot);
        ++param_base;
      }

      for (u32 idx = 1; idx < ops.size(); ++idx) {
        const ir::Operand& arg_op = storage_->operands()[ops.head() + idx];
        llvm::Type* wanted =
            callee_func->getFunctionType()->getParamType(param_base);
        ++param_base;
        args.push_back(own_abi ? call_argument(arg_op, wanted)
                               : argument_value(arg_op, wanted));
      }

      llvm::CallInst* call_inst = builder_->CreateCall(callee_func, args);

      if (i.dst.is_valid() && result_slot == nullptr) {
        values_.add_register(i.dst, call_inst);
      }
      break;
    }
    case Op::Ret: {
      DCHECK(ops.size() <= 1);
      // An explicit `()` comes back as an operand of a type that holds
      // no bytes, and the function still returns void.
      const ir::TypeTag returned =
          ops.empty()
              ? ir::TypeTag::Void
              : storage_->types()[storage_->operands()[ops.head()].type].tag;
      const bool void_return =
          returned == ir::TypeTag::Void || returned == ir::TypeTag::Never;
      if (void_return) {
        builder_->CreateRetVoid();
      } else if (return_slot_ != nullptr) {
        // The caller's slot stands where the value would return.
        const ir::Operand& lhs = storage_->operands()[ops.head()];
        copy_to_slot(return_slot_, lhs, type(lhs.type));
        builder_->CreateRetVoid();
      } else {
        const ir::Operand& lhs = storage_->operands()[ops.head()];
        builder_->CreateRet(resolve_operand_value(lhs));
      }
      break;
    }
    case Op::Unreachable: {
      DCHECK(ops.empty());
      builder_->CreateUnreachable();
      break;
    }
    default: {
      DLOG("Unknown control opcode found: {}", i.op);
      UNREACHABLE();
    }
  }
}

// A signature encodes to a symbol; a foreign one keeps the name it was
// declared with.
std::string LlvmIrEmitter::linkable_name(
    const ir::FunctionMeta& function_meta) const {
  if (function_meta.kind == ir::SymbolKind::Foreign) {
    return std::string(interner_->get(function_meta.name));
  }
  symbol::Signature signature;
  signature.path = std::string(interner_->get(function_meta.path));
  signature.name = std::string(interner_->get(function_meta.name));
  signature.owner = function_meta.owner;
  switch (function_meta.kind) {
    case ir::SymbolKind::Free:
      signature.kind = symbol::Signature::Kind::Free;
      break;
    case ir::SymbolKind::Assoc:
      signature.kind = symbol::Signature::Kind::Assoc;
      break;
    case ir::SymbolKind::Method:
      signature.kind = symbol::Signature::Kind::Method;
      break;
    case ir::SymbolKind::Foreign: break;
  }
  const ir::TypeIdxRange generics = function_meta.generics;
  signature.generics.reserve(generics.size());
  for (u32 i = 0; i < generics.size(); ++i) {
    signature.generics.emplace_back(generics.head().idx + i);
  }
  return symbol::mangle(signature, *storage_, *interner_);
}

std::string LlvmIrEmitter::display_name(
    const ir::FunctionMeta& function_meta) const {
  const std::string_view path = interner_->get(function_meta.path);
  std::string out;
  if (!path.empty()) {
    out += path;
    out += "::";
  }
  out += interner_->get(function_meta.name);
  return out;
}

namespace {

// One address the panic walker can name, with the spelling a frame
// shows. An empty name marks a frame the trace does not show (the
// runtime's own).
struct TableEntry {
  llvm::Constant* address = nullptr;
  std::string name;
};

// Defines `{address, name, len}` plus a null terminator under
// `array_name`, and points `slot_name` (created when missing) at it.
// Called after the runtime exists and before optimization.
void define_table(llvm::Module& module,
                  std::string_view array_name,
                  std::string_view slot_name,
                  std::string_view name_prefix,
                  const std::vector<TableEntry>& entries) {
  llvm::LLVMContext& context = module.getContext();
  llvm::PointerType* ptr = llvm::PointerType::getUnqual(context);
  llvm::Type* usize = module.getDataLayout().getIntPtrType(context);
  llvm::StructType* entry_type =
      llvm::StructType::get(context, {ptr, ptr, usize});

  std::vector<llvm::Constant*> constants;
  constants.reserve(entries.size() + 1);
  u32 made = 0;
  for (const TableEntry& entry : entries) {
    llvm::Constant* name = llvm::ConstantPointerNull::get(ptr);
    llvm::Constant* length = llvm::ConstantInt::get(usize, 0);
    if (!entry.name.empty()) {
      llvm::Constant* bytes = llvm::ConstantDataArray::getString(
          context, entry.name, /*AddNull=*/false);
      llvm::GlobalVariable* text = module.getOrInsertGlobal(
          std::string(name_prefix) + std::to_string(made++), bytes->getType());
      text->setInitializer(bytes);
      text->setConstant(true);
      text->setLinkage(llvm::GlobalValue::PrivateLinkage);
      name = llvm::ConstantExpr::getBitCast(text, ptr);
      length = llvm::ConstantInt::get(usize, entry.name.size());
    }
    constants.push_back(
        llvm::ConstantStruct::get(entry_type, {entry.address, name, length}));
  }
  // The walker stops at an entry with no start address.
  constants.push_back(llvm::ConstantStruct::get(
      entry_type,
      {llvm::ConstantPointerNull::get(ptr), llvm::ConstantPointerNull::get(ptr),
       llvm::ConstantInt::get(usize, 0)}));
  llvm::ArrayType* array_type =
      llvm::ArrayType::get(entry_type, constants.size());
  llvm::GlobalVariable* array =
      module.getOrInsertGlobal(std::string(array_name), array_type);
  array->setInitializer(llvm::ConstantArray::get(array_type, constants));
  array->setConstant(true);
  array->setLinkage(llvm::GlobalValue::PrivateLinkage);
  llvm::GlobalVariable* slot = module.getNamedGlobal(slot_name);
  if (slot == nullptr) {
    slot = module.getOrInsertGlobal(std::string(slot_name), ptr);
    slot->setLinkage(llvm::GlobalValue::PrivateLinkage);
    slot->setInitializer(llvm::ConstantPointerNull::get(ptr));
  }
  slot->setInitializer(llvm::ConstantExpr::getBitCast(array, ptr));
  slot->setConstant(true);
}

}  // namespace

void define_backtrace_table(llvm::Module& module,
                            const std::vector<BacktraceName>& visible) {
  std::vector<TableEntry> entries;
  entries.reserve(visible.size() + 1);
  for (llvm::Function& function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    std::string name;
    for (const BacktraceName& shown : visible) {
      if (shown.function == &function) {
        name = shown.name;
        break;
      }
    }
    entries.push_back(TableEntry{&function, std::move(name)});
  }
  define_table(module, "alcy_backtrace_array", "alcy_backtrace_entries",
               "alcy.backtrace.name.", entries);
}

void define_callsite_table(llvm::Module& module,
                           const std::vector<CallSiteName>& sites) {
  std::vector<TableEntry> entries;
  entries.reserve(sites.size() + 1);
  for (const CallSiteName& site : sites) {
    entries.push_back(
        TableEntry{llvm::BlockAddress::get(site.block->getParent(), site.block),
                   site.site});
  }
  define_table(module, "alcy_callsite_array", "alcy_callsite_entries",
               "alcy.callsite.name.", entries);
}

llvm::Function* LlvmIrEmitter::create_function(
    const ir::FunctionMeta& function_meta) const {
  llvm::SmallVector<llvm::Type*, FUNCTION_ARGS_SOO_SIZE> parameter_types;
  parameter_types.reserve(function_meta.param_types.size());
  // A result past the direct limit crosses in a slot the caller names
  // first, and the signature returns void there.
  const bool indirect_ret = in_memory_aggregate(function_meta.return_type);
  if (indirect_ret) {
    parameter_types.emplace_back(builder_->getPtrTy());
  }
  for (const ir::TypeIdx id : function_meta.param_types) {
    parameter_types.emplace_back(parameter_type(id));
  }

  llvm::FunctionType* func_type = llvm::FunctionType::get(
      indirect_ret ? builder_->getVoidTy()
                   : return_type(function_meta.return_type),
      llvm::ArrayRef<llvm::Type*>(parameter_types), false);

  const std::string func_name = linkable_name(function_meta);

  llvm::Function* llvm_function = llvm::Function::Create(
      func_type, llvm::Function::ExternalLinkage, func_name, module_);
  DCHECK_MSG(llvm_function, "Failed to create LLVM function");
  // The parameter type already says how the reference behaves, so the
  // ownership rules translate to one attribute per reference: an
  // exclusive borrow is unaliased, a shared one read-only, and a
  // reference is never null. A reference to a slice crosses as the fat
  // pointer itself, where the attributes do not apply.
  u32 index = indirect_ret ? 1 : 0;
  for (const ir::TypeIdx id : function_meta.param_types) {
    llvm::Type* param_type = parameter_types[index];
    if (!param_type->isPointerTy()) {
      ++index;
      continue;
    }
    const ir::TypeTag tag = storage_->types()[id.idx].tag;
    if (tag == ir::TypeTag::MutRef) {
      llvm_function->addParamAttr(index, llvm::Attribute::NoAlias);
      llvm_function->addParamAttr(index, llvm::Attribute::NonNull);
    } else if (tag == ir::TypeTag::Ref) {
      llvm_function->addParamAttr(index, llvm::Attribute::ReadOnly);
      llvm_function->addParamAttr(index, llvm::Attribute::NonNull);
    }
    ++index;
  }
  if (func_type->getReturnType()->isPointerTy()) {
    const ir::TypeTag return_tag =
        storage_->types()[function_meta.return_type.idx].tag;
    if (return_tag == ir::TypeTag::Ref || return_tag == ir::TypeTag::MutRef) {
      llvm_function->addRetAttr(llvm::Attribute::NonNull);
    }
  }
  // A source-level inline hint rides on the function, so LLVM's inliner
  // and ThinLTO see the same policy the declaration asked for.
  switch (function_meta.inline_kind) {
    case ir::InlineKind::Always:
      llvm_function->addFnAttr(llvm::Attribute::AlwaysInline);
      break;
    case ir::InlineKind::Hint:
      llvm_function->addFnAttr(llvm::Attribute::InlineHint);
      break;
    case ir::InlineKind::Never:
      llvm_function->addFnAttr(llvm::Attribute::NoInline);
      break;
    case ir::InlineKind::None: break;
  }
  // There is no unwinding in alcy; `panic` aborts.
  llvm_function->setDoesNotThrow();
  return llvm_function;
}

u64 LlvmIrEmitter::memory_size(llvm::Type* llvm_type) const {
  return module_->getDataLayout().getTypeStoreSize(llvm_type).getFixedValue();
}

bool LlvmIrEmitter::in_memory_aggregate(ir::TypeIdx idx) const {
  using T = ir::TypeTag;
  const T tag = storage_->types()[idx.idx].tag;
  if (tag != T::Struct && tag != T::Array && tag != T::Enum &&
      tag != T::Tuple) {
    return false;
  }
  llvm::Type* aggregate = type(idx);
  if (!aggregate->isSized()) {
    return false;
  }
  return module_->getDataLayout().getTypeAllocSize(aggregate) >=
         IN_MEMORY_AGGREGATE_BYTES;
}

llvm::Type* LlvmIrEmitter::parameter_type(ir::TypeIdx idx) const {
  return in_memory_aggregate(idx) ? builder_->getPtrTy() : type(idx);
}

llvm::AllocaInst* LlvmIrEmitter::frame_slot(llvm::Type* llvm_type) const {
  llvm::BasicBlock& entry =
      builder_->GetInsertBlock()->getParent()->getEntryBlock();
  llvm::IRBuilder<> at_entry(&entry, entry.begin());
  return at_entry.CreateAlloca(llvm_type);
}

llvm::Value* LlvmIrEmitter::operand_slot(const ir::Operand& operand,
                                         llvm::Type* value_type) {
  if (operand.is<ir::RegisterIdx>()) {
    const ir::RegisterIdx reg = operand.as_register();
    if (values_.slot_type(reg) != nullptr) {
      return values_.register_value(reg);
    }
  }
  llvm::AllocaInst* slot = frame_slot(value_type);
  builder_->CreateStore(resolve_operand_value(operand), slot);
  return slot;
}

void LlvmIrEmitter::copy_to_slot(llvm::Value* slot,
                                 const ir::Operand& operand,
                                 llvm::Type* value_type) {
  llvm::Value* source = operand_slot(operand, value_type);
  if (source == slot) {
    return;
  }
  builder_->CreateMemCpy(slot, llvm::MaybeAlign(), source, llvm::MaybeAlign(),
                         builder_->getInt64(memory_size(value_type)));
}

llvm::AllocaInst* LlvmIrEmitter::call_result_slot(ir::TypeIdx ret,
                                                  ir::RegisterIdx dst) {
  if (!in_memory_aggregate(ret)) {
    return nullptr;
  }
  llvm::Type* slot_type = type(ret);
  llvm::AllocaInst* slot = frame_slot(slot_type);
  if (dst.is_valid()) {
    values_.add_register(dst, slot);
    values_.add_slot_type(dst, slot_type);
  }
  return slot;
}

llvm::Value* LlvmIrEmitter::call_argument(const ir::Operand& operand,
                                          llvm::Type* wanted) {
  if (!in_memory_aggregate(operand.type)) {
    return argument_value(operand, wanted);
  }
  DCHECK_MSG(wanted->isPointerTy(), "A large aggregate crosses as a slot");
  return operand_slot(operand, type(operand.type));
}

llvm::Value* LlvmIrEmitter::resolve_operand_value(const ir::Operand& op) {
  using Payload = ir::Operand::Payload;
  switch (op.tag()) {
    case Payload::TagOf<ir::RegisterIdx>: {
      const ir::RegisterIdx reg = op.as_register();
      // A register that names a slot holds bytes, not a value: reading it
      // is the load the register's type spells. The selector
      // bounds the width of the values it takes, so a consumer of a shape
      // past IN_MEMORY_AGGREGATE_BYTES goes through operand_slot for the
      // address rather than reading the register.
      if (llvm::Type* slot_type = values_.slot_type(reg)) {
        return builder_->CreateLoad(slot_type, values_.register_value(reg));
      }
      return values_.register_value(reg);
    }
    case Payload::TagOf<ir::BlockIdx>: return values_.block(op.as_block());
    case Payload::TagOf<ir::ImmutableIdx>:
      return values_.immutable(op.as_immutable());
    case Payload::TagOf<ir::FunctionIdx>:
      return values_.function(op.as_function());

    default:
      DLOG("Unknown operand tag found while resolving operand value.");
      UNREACHABLE();
  }
}

llvm::Function* LlvmIrEmitter::resolve_operand_function(
    const ir::Operand& op) const {
  using Payload = ir::Operand::Payload;
  switch (op.tag()) {
    case Payload::TagOf<ir::FunctionIdx>:
      return values_.function(op.as_function());
    case Payload::TagOf<ir::ExternalFunctionIdx>:
      return values_.external_function(op.as_external_function());
    default:
      DLOG("Unknown operand tag found while resolving operand function.");
      UNREACHABLE();
  }
}

void LlvmIrEmitter::setup_immutables() {
  for (const ir::ImmutableIdx immutable_idx :
       storage_->immutables().idx_range()) {
    const ir::Immutable& immutable = storage_->immutables()[immutable_idx];

    const ir::TypeTag tag = storage_->types()[immutable.type.idx].tag;

    if (tag == ir::TypeTag::Param) {
      // A parameter widens where the substitution names the type, so a
      // parametric body is the only holder of one, and no body the
      // emitter walks reaches it.
      continue;
    }
    if (ir::is_integer_type(tag)) {
      // TODO: Add i128, u128, i256, u256, and arbitrary bit support with
      // llvm::APInt
      llvm::Constant* c = llvm::ConstantInt::get(
          type(immutable.type), immutable.as_u64_integer(tag),
          ir::is_signed_integer_type(tag));
      DCHECK_MSG(c, "Failed to get integer constant from LLVM");

      values_.add_immutable(immutable_idx, c);
    } else if (ir::is_float_type(tag)) {
      llvm::Constant* c =
          llvm::ConstantFP::get(type(immutable.type), immutable.as_f64_fp(tag));
      DCHECK_MSG(c, "Failed to get fp constant from LLVM");

      values_.add_immutable(immutable_idx, c);
    } else if (tag == ir::TypeTag::Str) {
      const std::string_view str_val =
          interner_->get(immutable.data.str_id_value);
      llvm::GlobalVariable* global = builder_->CreateGlobalString(
          llvm::StringRef(str_val), "", 0, module_);
      llvm::Constant* zero = llvm::ConstantInt::get(builder_->getInt32Ty(), 0);
      llvm::SmallVector<llvm::Value*, 2> indices{zero, zero};
      llvm::Constant* ptr = llvm::ConstantExpr::getGetElementPtr(
          global->getValueType(), global, indices);
      llvm::Type* len_ty = width_ == ir::PointerWidth::W64
                               ? builder_->getInt64Ty()
                               : builder_->getInt32Ty();
      llvm::Constant* len = llvm::ConstantInt::get(len_ty, str_val.size());
      llvm::StructType* str_ty =
          llvm::cast<llvm::StructType>(type(immutable.type));
      values_.add_immutable(immutable_idx,
                            llvm::ConstantStruct::get(str_ty, {ptr, len}));
    } else {
      DCHECK_MSG(false, "Currently unsupported type found");
      UNREACHABLE();
    }
  }
}

void LlvmIrEmitter::setup_external_functions() {
  for (ir::ExternalFunctionIdx function_idx(0);
       function_idx.idx < storage_->external_functions().size();
       ++function_idx) {
    const ir::ExternalFunction& function =
        storage_->external_functions()[function_idx];

    values_.add_external_function(function_idx, create_function(function.meta));
  }
}

bool LlvmIrEmitter::is_entry_candidate(const ir::Function& function) const {
  // An associated function that happens to be named `main` is not the
  // entry: only a free function is, matching what the analyzer checks.
  if (function.meta.kind != ir::SymbolKind::Free) {
    return false;
  }
  if (interner_->get(function.meta.name) != "main") {
    return false;
  }
  if (!function.meta.param_types.empty()) {
    return false;
  }
  const ir::TypeTag ret = storage_->types()[function.meta.return_type.idx].tag;
  return ret == ir::TypeTag::Void || ret == ir::TypeTag::I32 ||
         ret == ir::TypeTag::Enum;
}

std::vector<bool> LlvmIrEmitter::reachable_functions(
    const std::vector<bool>& parametric) const {
  std::vector<bool> reachable(storage_->functions().size(), false);
  std::vector<u32> stack;
  const auto reach = [&](const ir::FunctionIdx function_idx) {
    if (function_idx.is_valid() && function_idx.idx < reachable.size() &&
        !parametric[function_idx.idx] && !reachable[function_idx.idx]) {
      reachable[function_idx.idx] = true;
      stack.push_back(function_idx.idx);
    }
  };
  // The roots: the entry a binary renames - the first candidate, the
  // same one the declaration loop would pick - and the surface a
  // library exports.
  if (emit_entry_) {
    for (const ir::FunctionIdx function_idx :
         storage_->functions().idx_range()) {
      if (!parametric[function_idx.idx] &&
          is_entry_candidate(storage_->functions()[function_idx])) {
        reach(function_idx);
        break;
      }
    }
  }
  if (exported_functions_.has_value()) {
    for (const ir::FunctionIdx function_idx : *exported_functions_) {
      reach(function_idx);
    }
  }
  if (stack.empty()) {
    // No root the image is entered through: a module this emitter is
    // not the right gate for keeps every function.
    return {};
  }
  // Calls and function values are both operands naming a function, so
  // one walk over the operands is the whole call graph.
  while (!stack.empty()) {
    const ir::Function& function =
        storage_->functions()[ir::FunctionIdx(stack.back())];
    stack.pop_back();
    for (const ir::BlockIdx block_idx : function.blocks) {
      const ir::Block& block = storage_->blocks()[block_idx];
      for (const ir::InstructionIdx instr_idx : block.instrs) {
        const ir::Instruction& instruction = storage_->instrs()[instr_idx];
        for (const ir::OperandIdx operand_idx : instruction.operands) {
          const ir::Operand& operand = storage_->operands()[operand_idx];
          if (operand.is<ir::FunctionIdx>()) {
            reach(operand.as_function());
          }
        }
      }
    }
  }
  return reachable;
}

void LlvmIrEmitter::emit_entry(llvm::Function* entry_function,
                               ir::TypeIdx entry_return) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler_, "main", "emit-fn");
  check_state();
  llvm::LLVMContext& context = module_->getContext();
  // A freestanding entry is the ELF entry symbol itself. At the ELF
  // entry, `argv` sits above `argc` on the stack; the wrapper reads
  // both from there, so a freestanding program sees the same `args()`
  // as a hosted one.
  const std::string_view triple_view = triple_;
  const bool elf_entry =
      freestanding_ && triple_view.find("windows") == std::string_view::npos &&
      triple_view.find("darwin") == std::string_view::npos &&
      triple_view.find("wasm") == std::string_view::npos;
  const bool windows_msvc =
      triple_view.find("windows") != std::string_view::npos;
  if (windows_msvc) {
    // The kernel calls the entry with no arguments and the static C
    // library is not there to gather them; the runtime does it from
    // the command line before the program starts.
    llvm::FunctionType* start_type =
        llvm::FunctionType::get(builder_->getVoidTy(), false);
    llvm::Function* wrapper = llvm::Function::Create(
        start_type, llvm::Function::ExternalLinkage, "_start", module_);
    llvm::BasicBlock* entry_block =
        llvm::BasicBlock::Create(context, "", wrapper);
    builder_->SetInsertPoint(entry_block);
    llvm::FunctionCallee init_args = module_->getOrInsertFunction(
        "alcy_init_args",
        llvm::FunctionType::get(builder_->getVoidTy(), false));
    builder_->CreateCall(init_args);
    emit_entry_body(wrapper, entry_function, entry_return);
    return;
  }
  if (elf_entry) {
    // A naked trampoline: read argc and argv off the initial stack
    // into the registers main's ABI expects, then call the outer
    // function. The outer function's address rides an ordinary
    // operand, so the partitioner and the LTO backend see the
    // trampoline depends on it rather than a symbol hidden in the
    // assembly text.
    llvm::FunctionType* outer_type = llvm::FunctionType::get(
        builder_->getVoidTy(),
        {builder_->getInt32Ty(), builder_->getPtrTy(), builder_->getPtrTy()},
        false);
    llvm::Function* outer =
        llvm::Function::Create(outer_type, llvm::Function::InternalLinkage,
                               "_alcy_start_outer", module_);
    llvm::Type* i64 = builder_->getInt64Ty();
    llvm::Value* outer_address =
        builder_->CreatePtrToInt(outer, i64, "outer_address");
    const char* trampoline = nullptr;
    const char* constraints = nullptr;
    if (triple_view.rfind("x86_64", 0) == 0) {
      trampoline =
          "mov (%rsp), %edi\n"
          "lea 8(%rsp), %rsi\n"
          "lea 16(%rsp,%rdi,8), %rdx\n"
          "call *$0";
      constraints = "{rax},~{memory},~{rdi},~{rsi},~{rdx}";
    } else if (triple_view.rfind("aarch64", 0) == 0) {
      trampoline =
          "ldr x0, [sp]\n"
          "add x1, sp, #8\n"
          "add x2, sp, #16\n"
          "add x2, x2, x0, lsl #3\n"
          "blr $0";
      constraints = "{x16},~{memory},~{x0},~{x1},~{x2},~{lr}";
    } else {
      trampoline =
          "ld a0, 0(sp)\n"
          "addi a1, sp, 8\n"
          "addi a2, sp, 16\n"
          "slli t1, a0, 3\n"
          "add a2, a2, t1\n"
          "jalr x0, $0, 0";
      constraints = "{t0},~{memory},~{a0},~{a1},~{a2},~{t1}";
    }
    llvm::FunctionType* void_type =
        llvm::FunctionType::get(builder_->getVoidTy(), {i64}, false);
    llvm::InlineAsm* trampoline_call = llvm::InlineAsm::get(
        void_type, trampoline, constraints, /*hasSideEffects=*/true);
    llvm::Function* start = llvm::Function::Create(
        void_type, llvm::Function::ExternalLinkage, "_start", module_);
    start->addFnAttr(llvm::Attribute::Naked);
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(context, "", start);
    builder_->SetInsertPoint(entry);
    builder_->CreateCall(trampoline_call, {outer_address});
    builder_->CreateRetVoid();

    llvm::BasicBlock* outer_entry =
        llvm::BasicBlock::Create(context, "", outer);
    builder_->SetInsertPoint(outer_entry);
    builder_->CreateStore(
        outer->getArg(0),
        module_->getOrInsertGlobal("alcy_argc", builder_->getInt32Ty()));
    builder_->CreateStore(
        outer->getArg(1),
        module_->getOrInsertGlobal("alcy_argv", builder_->getPtrTy()));
    builder_->CreateStore(
        outer->getArg(2),
        module_->getOrInsertGlobal("alcy_environ", builder_->getPtrTy()));
    emit_entry_body(outer, entry_function, entry_return);
    return;
  }
  llvm::FunctionType* wrapper_type =
      freestanding_
          ? llvm::FunctionType::get(builder_->getInt32Ty(), false)
          : llvm::FunctionType::get(
                builder_->getInt32Ty(),
                {builder_->getInt32Ty(), builder_->getPtrTy()}, false);
  llvm::Function* wrapper =
      llvm::Function::Create(wrapper_type, llvm::Function::ExternalLinkage,
                             freestanding_ ? "_start" : "main", module_);
  llvm::BasicBlock* entry_block =
      llvm::BasicBlock::Create(context, "", wrapper);
  builder_->SetInsertPoint(entry_block);
  if (!freestanding_) {
    builder_->CreateStore(
        wrapper->getArg(0),
        module_->getOrInsertGlobal("alcy_argc", builder_->getInt32Ty()));
    builder_->CreateStore(
        wrapper->getArg(1),
        module_->getOrInsertGlobal("alcy_argv", builder_->getPtrTy()));
  }
  emit_entry_body(wrapper, entry_function, entry_return);
  return;
}

void LlvmIrEmitter::emit_entry_body(llvm::Function* wrapper,
                                    llvm::Function* entry_function,
                                    ir::TypeIdx entry_return) {
  const ir::TypeTag ret = storage_->types()[entry_return.idx].tag;
  llvm::LLVMContext& context = module_->getContext();
  const std::string_view triple_view = triple_;
  const bool windows_msvc =
      triple_view.find("windows") != std::string_view::npos;
  builder_->SetInsertPoint(&wrapper->getEntryBlock());
  // A result past the direct limit crosses in a slot the wrapper owns,
  // so the exit code reads it out of memory.
  llvm::AllocaInst* result_slot = nullptr;
  llvm::Value* result = nullptr;
  if (in_memory_aggregate(entry_return)) {
    result_slot = builder_->CreateAlloca(type(entry_return));
    builder_->CreateCall(entry_function, {result_slot});
  } else {
    result = builder_->CreateCall(entry_function);
  }
  const auto result_tag = [&]() -> llvm::Value* {
    if (storage_->state().types[entry_return.idx].tag == ir::TypeTag::Enum &&
        ir::enum_is_zst(storage_->state(), entry_return)) {
      return builder_->getInt32(0);
    }
    llvm::Value* tag = nullptr;
    if (result_slot == nullptr) {
      tag = builder_->CreateExtractValue(result, 0);
    } else {
      llvm::Type* const slot_ty =
          llvm::cast<llvm::StructType>(type(entry_return))->getElementType(0);
      tag = builder_->CreateLoad(
          slot_ty,
          builder_->CreateStructGEP(type(entry_return), result_slot, 0));
    }
    return builder_->CreateZExtOrTrunc(tag, builder_->getInt32Ty());
  };
  // A windows target always exits through ExitProcess: the freestanding
  // flag carries no other meaning there, and the syscall exit below is
  // the Linux sequence.
  if (windows_msvc) {
    llvm::Value* code = llvm::ConstantInt::get(builder_->getInt32Ty(), 0);
    if (ret == ir::TypeTag::I32) {
      code = result;
    } else if (ret == ir::TypeTag::Enum) {
      llvm::Value* tag = result_tag();
      llvm::Value* is_ok = builder_->CreateICmpEQ(
          tag, llvm::ConstantInt::get(builder_->getInt32Ty(), 0));
      code = builder_->CreateSelect(
          is_ok, llvm::ConstantInt::get(builder_->getInt32Ty(), 0),
          llvm::ConstantInt::get(builder_->getInt32Ty(), 1));
      llvm::BasicBlock* ok_block =
          llvm::BasicBlock::Create(context, "", wrapper);
      llvm::BasicBlock* err_block =
          llvm::BasicBlock::Create(context, "", wrapper);
      builder_->CreateCondBr(is_ok, ok_block, err_block);
      builder_->SetInsertPoint(err_block);
      if (!emit_stderr_message("main returned Err\n")) {
        DCHECK_MSG(false, "windows stderr on an unsupported target");
      }
      builder_->CreateBr(ok_block);
      builder_->SetInsertPoint(ok_block);
    }
    if (!emit_exit_windows(code)) {
      DCHECK_MSG(false, "windows exit on an unsupported target");
    }
    return;
  }
  if (freestanding_) {
    // The exit status is the i32 main returned; an enum main exits
    // nonzero on a variant other than the first, and `()` exits zero.
    if (ret == ir::TypeTag::I32) {
      if (!emit_exit(result)) {
        DCHECK_MSG(false, "freestanding exit on an unsupported target");
      }
      return;
    }
    if (ret != ir::TypeTag::Enum) {
      if (!emit_exit(llvm::ConstantInt::get(builder_->getInt32Ty(), 0))) {
        DCHECK_MSG(false, "freestanding exit on an unsupported target");
      }
      return;
    }
    llvm::Value* tag = result_tag();
    llvm::Value* is_ok = builder_->CreateICmpEQ(
        tag, llvm::ConstantInt::get(builder_->getInt32Ty(), 0));
    llvm::BasicBlock* ok_block = llvm::BasicBlock::Create(context, "", wrapper);
    llvm::BasicBlock* err_block =
        llvm::BasicBlock::Create(context, "", wrapper);
    builder_->CreateCondBr(is_ok, ok_block, err_block);
    builder_->SetInsertPoint(ok_block);
    if (!emit_exit(llvm::ConstantInt::get(builder_->getInt32Ty(), 0))) {
      DCHECK_MSG(false, "freestanding exit on an unsupported target");
    }
    builder_->SetInsertPoint(err_block);
    // The error variant is an exit, not a panic: the message goes to
    // stderr and the status is 1, the same as a hosted build.
    if (!emit_stderr_message("main returned Err\n")) {
      DCHECK_MSG(false, "freestanding stderr on an unsupported target");
    }
    if (!emit_exit(llvm::ConstantInt::get(builder_->getInt32Ty(), 1))) {
      DCHECK_MSG(false, "freestanding exit on an unsupported target");
    }
    return;
  }
  if (ret == ir::TypeTag::I32) {
    builder_->CreateRet(result);
    return;
  }
  if (ret == ir::TypeTag::Enum) {
    // A two-variant enum whose first variant holds `()` returns by
    // value; a nonzero discriminant aborts through the panic path with
    // a generic message.
    llvm::Value* tag = result_tag();
    llvm::Value* is_ok = builder_->CreateICmpEQ(
        tag, llvm::ConstantInt::get(builder_->getInt32Ty(), 0));
    llvm::BasicBlock* ok_block = llvm::BasicBlock::Create(context, "", wrapper);
    llvm::BasicBlock* err_block =
        llvm::BasicBlock::Create(context, "", wrapper);
    builder_->CreateCondBr(is_ok, ok_block, err_block);
    builder_->SetInsertPoint(ok_block);
    builder_->CreateRet(llvm::ConstantInt::get(builder_->getInt32Ty(), 0));
    builder_->SetInsertPoint(err_block);
    // The error variant is an exit, not a panic: the message goes to
    // stderr and the status is 1. The direct backend reports the same.
    llvm::Type* len_ty = width_ == ir::PointerWidth::W64
                             ? builder_->getInt64Ty()
                             : builder_->getInt32Ty();
    llvm::FunctionCallee write = module_->getOrInsertFunction(
        "write",
        llvm::FunctionType::get(
            len_ty, {builder_->getInt32Ty(), builder_->getPtrTy(), len_ty},
            false));
    llvm::GlobalVariable* message =
        builder_->CreateGlobalString("main returned Err\n", "", 0, module_);
    llvm::Value* zero = llvm::ConstantInt::get(builder_->getInt32Ty(), 0);
    llvm::SmallVector<llvm::Value*, 2> indices{zero, zero};
    llvm::Value* bytes =
        builder_->CreateInBoundsGEP(message->getValueType(), message, indices);
    llvm::Value* len = llvm::ConstantInt::get(
        len_ty, llvm::StringRef("main returned Err\n").size());
    llvm::Value* stderr_fd = llvm::ConstantInt::get(builder_->getInt32Ty(), 2);
    builder_->CreateCall(write, {stderr_fd, bytes, len});
    llvm::FunctionCallee quit = module_->getOrInsertFunction(
        "exit", llvm::FunctionType::get(builder_->getVoidTy(),
                                        {builder_->getInt32Ty()}, false));
    builder_->CreateCall(quit,
                         {llvm::ConstantInt::get(builder_->getInt32Ty(), 1)});
    builder_->CreateUnreachable();
    return;
  }
  builder_->CreateRet(llvm::ConstantInt::get(builder_->getInt32Ty(), 0));
}

// Ends the program through the target's exit syscall. The number is
// loaded in the assembly text and the status rides an input operand,
// so one shape covers every output kind: a syscall never returns, so
// the call is immediately unreachable.
bool LlvmIrEmitter::emit_stderr_message(llvm::StringRef message) {
  const std::string_view triple_check = triple_;
  if (triple_check.find("windows") != std::string_view::npos) {
    // GetStdHandle(-12) is stderr; the kernel writes the bytes and
    // nothing sees a C library.
    llvm::FunctionType* get_std_type = llvm::FunctionType::get(
        builder_->getPtrTy(), {builder_->getInt32Ty()}, false);
    llvm::FunctionCallee get_std =
        module_->getOrInsertFunction("GetStdHandle", get_std_type);
    llvm::Value* handle = builder_->CreateCall(
        get_std, {llvm::ConstantInt::get(builder_->getInt32Ty(), -12, true)},
        "stderr");
    llvm::AllocaInst* slot =
        builder_->CreateAlloca(builder_->getInt32Ty(), nullptr, "written");
    builder_->CreateStore(llvm::ConstantInt::get(builder_->getInt32Ty(), 0),
                          slot);
    llvm::FunctionType* write_file_type = llvm::FunctionType::get(
        builder_->getInt32Ty(),
        {builder_->getPtrTy(), builder_->getPtrTy(), builder_->getInt32Ty(),
         builder_->getPtrTy(), builder_->getPtrTy()},
        false);
    llvm::FunctionCallee write_file =
        module_->getOrInsertFunction("WriteFile", write_file_type);
    llvm::GlobalVariable* global =
        builder_->CreateGlobalString(message, "", 0, module_);
    llvm::Value* zero = llvm::ConstantInt::get(builder_->getInt32Ty(), 0);
    llvm::SmallVector<llvm::Value*, 2> indices{zero, zero};
    llvm::Value* bytes =
        builder_->CreateInBoundsGEP(global->getValueType(), global, indices);
    builder_->CreateCall(
        write_file,
        {handle, bytes,
         llvm::ConstantInt::get(builder_->getInt32Ty(),
                                static_cast<u32>(message.size())),
         slot, llvm::ConstantPointerNull::get(builder_->getPtrTy())});
    return true;
  }
  const std::string arch = triple_.substr(0, triple_.find('-'));
  const char* text = nullptr;
  const char* constraints = nullptr;
  if (arch == "x86_64") {
    text = "movq $$1, %rax\nsyscall";
    constraints = "{rdi},{rsi},{rdx},~{rax},~{rcx},~{r11},~{memory}";
  } else if (arch == "aarch64") {
    text = "mov x8, #64\nsvc #0";
    constraints = "{x0},{x1},{x2},~{x8},~{memory}";
  } else if (arch == "riscv64") {
    text = "li a7, 64\necall";
    constraints = "{a0},{a1},{a2},~{a7},~{memory}";
  } else {
    return false;
  }
  llvm::GlobalVariable* global =
      builder_->CreateGlobalString(message, "", 0, module_);
  llvm::Type* len_ty = width_ == ir::PointerWidth::W64 ? builder_->getInt64Ty()
                                                       : builder_->getInt32Ty();
  llvm::Value* len = llvm::ConstantInt::get(len_ty, message.size());
  llvm::Value* fd = llvm::ConstantInt::get(builder_->getInt32Ty(), 2);
  llvm::Value* zero = llvm::ConstantInt::get(builder_->getInt32Ty(), 0);
  llvm::SmallVector<llvm::Value*, 2> indices{zero, zero};
  llvm::Value* bytes =
      builder_->CreateInBoundsGEP(global->getValueType(), global, indices);
  llvm::Type* word = builder_->getInt64Ty();
  llvm::FunctionType* type = llvm::FunctionType::get(
      builder_->getVoidTy(), {word, builder_->getPtrTy(), word}, false);
  llvm::InlineAsm* call = llvm::InlineAsm::get(type, text, constraints,
                                               /*hasSideEffects=*/true);
  llvm::Value* wide_fd = builder_->CreateSExt(fd, word);
  llvm::Value* wide_len = builder_->CreateSExt(len, word);
  builder_->CreateCall(call, {wide_fd, bytes, wide_len});
  return true;
}

bool LlvmIrEmitter::emit_exit_windows(llvm::Value* code32) {
  llvm::FunctionCallee quit = module_->getOrInsertFunction(
      "ExitProcess", llvm::FunctionType::get(builder_->getVoidTy(),
                                             {builder_->getInt32Ty()}, false));
  builder_->CreateCall(quit, {code32});
  builder_->CreateUnreachable();
  return true;
}

bool LlvmIrEmitter::emit_exit(llvm::Value* code32) {
  const std::string arch = triple_.substr(0, triple_.find('-'));
  const char* text = nullptr;
  const char* constraints = nullptr;
  if (arch == "x86_64") {
    // AT&T syntax is the assembler's default dialect, and `$$` is a
    // literal `$` in inline assembly.
    text = "movq $$60, %rax\nsyscall";
    constraints = "{rdi},~{rax},~{rcx},~{r11},~{memory}";
  } else if (arch == "aarch64") {
    text = "mov x8, #93\nsvc #0";
    constraints = "{x0},~{x8},~{memory}";
  } else if (arch == "riscv64") {
    text = "li a7, 93\necall";
    constraints = "{a0},~{a7},~{memory}";
  } else {
    return false;
  }
  llvm::Type* word = builder_->getInt64Ty();
  // One input: the status register. The syscall number is loaded in
  // the assembly text, so nothing else crosses.
  llvm::FunctionType* type =
      llvm::FunctionType::get(builder_->getVoidTy(), {word}, false);
  llvm::InlineAsm* exit_call =
      llvm::InlineAsm::get(type, text, constraints, /*hasSideEffects=*/true);
  llvm::Value* code = builder_->CreateSExt(code32, word);
  builder_->CreateCall(exit_call, {code});
  builder_->CreateUnreachable();
  return true;
}

}  // namespace codegen_llvm
