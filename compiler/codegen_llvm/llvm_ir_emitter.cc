// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/llvm_ir_emitter.h"

#include <memory>
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
#include "source/source.h"
#include "symbol/mangle.h"

#if BUILD_FLAG(IS_DEBUG)
#include "ir/formatter.h"  // IWYU pragma: keep
#endif

namespace codegen_llvm {

LlvmIrEmitter::LlvmIrEmitter(llvm::Module* module,
                             ir::VerifiedStorage storage,
                             ir::SymbolTable* interner,
                             const Target& target,
                             bool emit_entry,
                             bool freestanding,
                             bool overflow_checks,
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
      freestanding_(freestanding),
      overflow_checks_(overflow_checks),
      backtrace_(backtrace),
      instr_spans_(instr_spans),
      site_names_(site_names) {}

void LlvmIrEmitter::check_state() {
  // DCHECK_MSG(storage_, "IR Storage is null");
  DCHECK_MSG(module_, "LLVM Module is null");
  DCHECK_MSG(builder_, "LLVM IR Builder is null");
  DCHECK_MSG(interner_, "String Interner is null");
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

llvm::Type* LlvmIrEmitter::build_type(ir::TypeIdx idx) const {
  const ir::TypeNode& node = storage_->types()[idx];
  const ir::TypeTag tag = node.tag;
  using T = ir::TypeTag;

  switch (tag) {
    case T::Void: return builder_->getVoidTy();
    case T::Never:
      // Uninhabited; only function return positions lower through
      // here (noreturn externals), where LLVM expects void.
      return builder_->getVoidTy();
    case T::I1: return builder_->getInt1Ty();
    case T::I8:
    case T::U8: return builder_->getInt8Ty();
    case T::I16:
    case T::U16: return builder_->getInt16Ty();
    case T::I32:
    case T::U32: return builder_->getInt32Ty();
    case T::I64:
    case T::U64: return builder_->getInt64Ty();
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
      for (const ir::TypeIdx field : struct_type.fields) {
        field_types.emplace_back(type(field));
      }
      // Anonymous structural type; LLVM deduplicates identical shapes.
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
      // A discriminant, then the payload in the slot itself. Keeping the
      // payload here rather than behind a pointer is what lets an enum
      // value outlive the frame that built it.
      llvm::SmallVector<llvm::Type*, 2> slot_types;
      slot_types.emplace_back(builder_->getInt32Ty());
      slot_types.emplace_back(enum_payload_area_type(idx));
      llvm::StructType* slot =
          llvm::StructType::get(module_->getContext(), slot_types);
      // The layout ir::type_layout publishes is what field offsets are
      // computed against, so the type built here has to match it. The
      // module carries the target's layout before the emitter runs, so
      // this checks the two against each other on every build rather
      // than waiting for a layout to appear.
      const ir::TypeLayout expected =
          ir::type_layout(storage_->state(), idx, width_);
      const llvm::DataLayout& dl = module_->getDataLayout();
      DCHECK(!dl.isDefault());
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

  // Declare every function before defining any body so forward
  // calls resolve regardless of declaration order.
  llvm::Function* entry_function = nullptr;
  ir::TypeTag entry_return = ir::TypeTag::Void;
  for (const ir::FunctionIdx function_idx : storage_->functions().idx_range()) {
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
      entry_return = storage_->types()[function.meta.return_type.idx].tag;
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
    }
    if (backtrace_) {
      // The renamed entry keeps its source spelling: a frame reads
      // `main`, not `alcy_main`.
      visible_functions_.push_back(
          BacktraceName{llvm_function, display_name(function.meta)});
    }
  }
  // PERF: consider running this process concurrently.
  for (const ir::FunctionIdx function_idx : storage_->functions().idx_range()) {
    const ir::Function& function = storage_->functions()[function_idx];
    // Bodies emit independently: no cross-function state moves between
    // iterations, which is also what the comment above is waiting on.
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

  for (const ir::BlockIdx block_idx : function.blocks) {
    values_.add_block(block_idx, llvm::BasicBlock::Create(module_->getContext(),
                                                          "", llvm_function));
  }

  // Pre-generate phi nodes. The entry block (first in function
  // order) receives LLVM function arguments directly instead of
  // PHIs: lowering places one block parameter per declared parameter.
  u32 arg_no = 0;
  for (const ir::BlockIdx block_idx : function.blocks) {
    const ir::Block& block = storage_->blocks()[block_idx];
    llvm::BasicBlock* llvm_block = values_.block(block_idx);
    const bool is_entry = block_idx.idx == function.blocks.head().idx;

    builder_->SetInsertPoint(llvm_block);
    for (const ir::BlockParamIdx param_id : block.block_params) {
      const ir::BlockParam& param = storage_->block_params()[param_id];

      if (is_entry) {
        values_.add_register(param.reg, llvm_function->getArg(arg_no++));
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

void LlvmIrEmitter::emit_instruction(ir::InstructionIdx instr_idx,
                                     const ir::Instruction& instr) {
  check_state();

  using Op = ir::Opcode;

  if (backtrace_ && instr.op == Op::Call && has_call_site(instr_idx)) {
    emit_sited_call(instr_idx, instr);
    return;
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
    case Op::GetElementPtr:
    case Op::ElemOffset:
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

      builder_->CreateBr(target_llvm_block);

      const ir::Block& target_block = storage_->blocks()[target_block_idx];
      DCHECK(ops.size() == 1 + target_block.block_params.size());
      // Branch arguments fill the target's parameters positionally:
      // operands[1..N] are the incoming values in parameter order.
      u32 position = 0;
      for (const ir::BlockParamIdx param_idx : target_block.block_params) {
        const ir::BlockParam& param = storage_->block_params()[param_idx];

        auto* phi =
            llvm::cast<llvm::PHINode>(values_.register_value(param.reg));

        const ir::Operand& arg_op =
            storage_->operands()[ops.head() + 1 + position];
        ++position;
        llvm::Value* arg_val = resolve_operand_value(arg_op);

        phi->addIncoming(arg_val, current_llvm_block);
      }
      break;
    }
    case Op::CondBr: {
      // operands = [cond, true_block, false_block]. Targets must not take
      // block parameters in MVP.
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
      // Targets must not take block parameters in MVP.
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
        llvm::SmallVector<llvm::Type*, FUNCTION_ARGS_SOO_SIZE> param_types;
        param_types.reserve(sig.params.size() + 1);
        param_types.push_back(builder_->getPtrTy());
        for (ir::TypeIdx param : sig.params) {
          param_types.push_back(type(param));
        }
        llvm::FunctionType* fn_type =
            llvm::FunctionType::get(type(sig.ret), param_types, false);
        llvm::SmallVector<llvm::Value*, FUNCTION_ARGS_SOO_SIZE> args;
        args.reserve(ops.size());
        args.push_back(env);
        for (u32 idx = 1; idx < ops.size(); ++idx) {
          const ir::Operand& arg_op = storage_->operands()[ops.head() + idx];
          args.push_back(resolve_operand_value(arg_op));
        }
        llvm::CallInst* call_inst = builder_->CreateCall(fn_type, code, args);
        if (i.dst.is_valid()) {
          values_.add_register(i.dst, call_inst);
        }
        break;
      }
      llvm::Function* callee_func = resolve_operand_function(callee_op);

      llvm::SmallVector<llvm::Value*, FUNCTION_ARGS_SOO_SIZE> args;
      args.reserve(ops.size() - 1);

      for (u32 idx = 1; idx < ops.size(); ++idx) {
        const ir::Operand& arg_op = storage_->operands()[ops.head() + idx];
        args.push_back(resolve_operand_value(arg_op));
      }

      llvm::CallInst* call_inst = builder_->CreateCall(callee_func, args);

      if (i.dst.is_valid()) {
        values_.add_register(i.dst, call_inst);
      }
      break;
    }
    case Op::Ret: {
      DCHECK(ops.size() <= 1);
      if (ops.empty()) {
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
  // DLOG("Function Name: {}, Param Count in Slice: {}",
  //      interner_->get(function_meta.name), function_meta.param_types.size());

  for (const ir::TypeIdx id : function_meta.param_types) {
    parameter_types.emplace_back(type(id));
  }

  llvm::FunctionType* func_type = llvm::FunctionType::get(
      type(function_meta.return_type),
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
  u32 index = 0;
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
  // There is no unwinding in alcy; `panic` aborts.
  llvm_function->setDoesNotThrow();
  return llvm_function;
}

llvm::Value* LlvmIrEmitter::resolve_operand_value(const ir::Operand& op) const {
  using Payload = ir::Operand::Payload;
  switch (op.tag()) {
    case Payload::TagOf<ir::RegisterIdx>:
      return values_.register_value(op.as_register());
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

void LlvmIrEmitter::emit_entry(llvm::Function* entry_function,
                               ir::TypeTag ret) {
  PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(profiler_, "main", "emit-fn");
  check_state();
  llvm::LLVMContext& context = module_->getContext();
  // A freestanding entry is the ELF entry symbol itself and never
  // returns to a runtime; a hosted one is the C `main` a crt calls.
  llvm::Function* wrapper = llvm::Function::Create(
      llvm::FunctionType::get(builder_->getInt32Ty(), false),
      llvm::Function::ExternalLinkage, freestanding_ ? "_start" : "main",
      module_);
  llvm::BasicBlock* entry_block =
      llvm::BasicBlock::Create(context, "", wrapper);
  builder_->SetInsertPoint(entry_block);
  llvm::Value* result = builder_->CreateCall(entry_function);
  if (freestanding_) {
    // The exit status is the i32 main returned; an enum main exits
    // nonzero on a variant other than the first, and `()` exits zero.
    llvm::Value* code = llvm::ConstantInt::get(builder_->getInt32Ty(), 0);
    if (ret == ir::TypeTag::I32) {
      code = result;
    } else if (ret == ir::TypeTag::Enum) {
      llvm::Value* tag = builder_->CreateExtractValue(result, 0);
      llvm::Value* is_ok = builder_->CreateICmpEQ(
          tag, llvm::ConstantInt::get(builder_->getInt32Ty(), 0));
      code = builder_->CreateSelect(
          is_ok, llvm::ConstantInt::get(builder_->getInt32Ty(), 0),
          llvm::ConstantInt::get(builder_->getInt32Ty(), 1));
    }
    if (!emit_exit(code)) {
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
    llvm::Value* tag = builder_->CreateExtractValue(result, 0);
    llvm::Value* is_ok = builder_->CreateICmpEQ(
        tag, llvm::ConstantInt::get(builder_->getInt32Ty(), 0));
    llvm::BasicBlock* ok_block = llvm::BasicBlock::Create(context, "", wrapper);
    llvm::BasicBlock* err_block =
        llvm::BasicBlock::Create(context, "", wrapper);
    builder_->CreateCondBr(is_ok, ok_block, err_block);
    builder_->SetInsertPoint(ok_block);
    builder_->CreateRet(llvm::ConstantInt::get(builder_->getInt32Ty(), 0));
    builder_->SetInsertPoint(err_block);
    llvm::Type* len_ty = width_ == ir::PointerWidth::W64
                             ? builder_->getInt64Ty()
                             : builder_->getInt32Ty();
    llvm::FunctionCallee panic = module_->getOrInsertFunction(
        "alcy_panic", llvm::FunctionType::get(builder_->getVoidTy(),
                                              {builder_->getPtrTy(), len_ty,
                                               builder_->getPtrTy(), len_ty},
                                              false));
    llvm::GlobalVariable* message =
        builder_->CreateGlobalString("main returned Err", "", 0, module_);
    llvm::Value* zero = llvm::ConstantInt::get(builder_->getInt32Ty(), 0);
    llvm::SmallVector<llvm::Value*, 2> indices{zero, zero};
    llvm::Value* bytes =
        builder_->CreateInBoundsGEP(message->getValueType(), message, indices);
    llvm::Value* len = llvm::ConstantInt::get(
        len_ty, llvm::StringRef("main returned Err").size());
    // The entry thunk has no source position to name, so the panic prints
    // its message without one.
    llvm::GlobalVariable* where =
        builder_->CreateGlobalString("", "", 0, module_);
    llvm::Value* where_bytes =
        builder_->CreateInBoundsGEP(where->getValueType(), where, indices);
    llvm::Value* zero_len = llvm::ConstantInt::get(len_ty, 0);
    builder_->CreateCall(panic, {bytes, len, where_bytes, zero_len});
    builder_->CreateUnreachable();
    return;
  }
  builder_->CreateRet(llvm::ConstantInt::get(builder_->getInt32Ty(), 0));
}

// Ends the program through the target's exit syscall. The number is
// loaded in the assembly text and the status rides an input operand,
// so one shape covers every output kind: a syscall never returns, so
// the call is immediately unreachable.
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
