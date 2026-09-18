// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "codegen_llvm/llvm_ir_emitter.h"

#include <memory>
#include <string_view>
#include <utility>

#include "cfg/build_config.h"
#include "codegen_llvm/common.h"
#include "debug/dcheck.h"
#include "debug/dlog.h"
#include "debug/fatal.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "ir/block.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"
#include "ir/type_util.h"
#include "ir/verifier.h"

#if BUILD_FLAG(IS_DEBUG)
#include "ir/formatter.h"  // IWYU pragma: keep
#endif

namespace codegen_llvm {

LlvmIrEmitter::LlvmIrEmitter(llvm::Module* module,
                             ir::Storage&& storage,
                             str::StringInterner* interner)
    : module_(module),
      storage_(std::move(storage)),
      builder_(std::make_unique<IRBuilder>(module_->getContext())),
      interner_(interner) {}

void LlvmIrEmitter::check_state() {
  // DCHECK_MSG(storage_, "IR Storage is null");
  DCHECK_MSG(module_, "LLVM Module is null");
  DCHECK_MSG(builder_, "LLVM IR Builder is null");
  DCHECK_MSG(interner_, "String Interner is null");
}

llvm::Type* LlvmIrEmitter::type(ir::TypeIdx idx) const {
  const ir::TypeNode& node = storage_.types()[idx];
  const ir::TypeTag tag = node.tag;
  using T = ir::TypeTag;

  switch (tag) {
    case T::Void: return builder_->getVoidTy();
    case T::I1: return builder_->getInt1Ty();
    case T::I8: return builder_->getInt8Ty();
    case T::I16: return builder_->getInt16Ty();
    case T::I32: return builder_->getInt32Ty();
    case T::I64: return builder_->getInt64Ty();
    // case T::I128: return builder_->getInt128Ty();
    case T::F32: return builder_->getFloatTy();
    case T::F64: return builder_->getDoubleTy();
    case T::Ptr: return builder_->getPtrTy();
    case T::Ref: return builder_->getPtrTy();
    case T::MutRef: return builder_->getPtrTy();
    case T::Struct: {
      const ir::StructType& struct_type =
          storage_.struct_types()[node.as_struct()];
      llvm::SmallVector<llvm::Type*, kFunctionArgsSooSize> field_types;
      field_types.reserve(struct_type.fields.size());
      for (const ir::TypeIdx field : struct_type.fields) {
        field_types.emplace_back(type(field));
      }
      // Anonymous structural type; LLVM deduplicates identical shapes.
      return llvm::StructType::get(module_->getContext(), field_types);
    }
    case T::Array: {
      const ir::ArrayType& array_type = storage_.array_types()[node.as_array()];
      return llvm::ArrayType::get(type(array_type.element), array_type.count);
    }
    default: {
      DLOG("Unsupported type: {}", tag);
      DCHECK(false);
      UNREACHABLE();
    }
  }
}

void LlvmIrEmitter::emit() && noexcept {
  check_state();

  values_.resize_functions(storage_.functions().size());
  values_.resize_registers(storage_.registers().size());
  values_.resize_blocks(storage_.blocks().size());
  values_.resize_immutables(storage_.immutables().size());
  values_.resize_external_functions(storage_.external_functions().size());
  values_.resize_alloca_types(storage_.registers().size());

  setup_immutables();
  setup_external_functions();

  // PERF: Consider run this process concurrently.
  for (const ir::FunctionIdx function_idx : storage_.functions().idx_range()) {
    const ir::Function& function = storage_.functions()[function_idx];

    llvm::Function* llvm_function = create_function(function.meta);
    values_.add_function(function_idx, llvm_function);
    emit_function(llvm_function, function);
  }

#if BUILD_FLAG(IS_DEBUG)
  if (base::Result<void, ir::VerifyError> result = ir::verify_storage(storage_);
      result.is_err()) [[unlikely]] {
    const ir::VerifyError error = std::move(result).unwrap_err();
    const diag::Diagnostic diag = ir::to_diagnostic(error);
    fmt::memory_buffer rendered;
    diag::render(diag, rendered);
    DLOG("{}", std::string_view(rendered.data(), rendered.size()));
    UNREACHABLE();
  }
  if (llvm::verifyModule(*module_, &llvm::errs())) [[unlikely]] {
    DLOG("LLVM verify module failed");
    module_->print(llvm::errs(), nullptr);
    UNREACHABLE();
  }
#endif
}

void LlvmIrEmitter::emit_function(llvm::Function* llvm_function,
                                  const ir::Function& function) {
  check_state();

  // 3-pass block emission(block declare -> generate phi nodes -> block define)
  for (const ir::BlockIdx block_idx : function.blocks) {
    values_.add_block(block_idx, llvm::BasicBlock::Create(module_->getContext(),
                                                          "", llvm_function));
  }

  // Pre-generate phi nodes
  for (const ir::BlockIdx block_idx : function.blocks) {
    const ir::Block& block = storage_.blocks()[block_idx];
    llvm::BasicBlock* llvm_block = values_.block(block_idx);

    builder_->SetInsertPoint(llvm_block);
    for (const ir::BlockParamIdx param_id : block.block_params) {
      const ir::BlockParam& param = storage_.block_params()[param_id];

      llvm::PHINode* phi = builder_->CreatePHI(type(param.type), 0);
      values_.add_register(param.reg, phi);
    }
  }

  for (const ir::BlockIdx block_idx : function.blocks) {
    const ir::Block& block = storage_.blocks()[block_idx];
    builder_->SetInsertPoint(values_.block(block_idx));
    emit_block(block);
  }

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
    const ir::Instruction& instr = storage_.instrs()[instr_idx];
    emit_instruction(instr);
  }
}

void LlvmIrEmitter::emit_instruction(const ir::Instruction& instr) {
  check_state();

  using Op = ir::Opcode;

  switch (instr.op) {
    case Op::Noop: break;

    case Op::IntAdd:
    case Op::IntSub:
    case Op::IntMul:
    case Op::IntDiv:
    case Op::UintDiv:
    case Op::IntRem:
    case Op::UintRem:
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
    case Op::Select:
    case Op::Move:
    case Op::Drop: emit_compute(instr); break;

    case Op::Alloca:
    case Op::Load:
    case Op::Store:
    case Op::GetElementPtr:
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
      const ir::Operand& target_op = storage_.operands()[ops.head()];
      const ir::BlockIdx target_block_idx = target_op.as_block();
      llvm::BasicBlock* target_llvm_block = values_.block(target_block_idx);
      llvm::BasicBlock* current_llvm_block = builder_->GetInsertBlock();

      builder_->CreateBr(target_llvm_block);

      // Add incoming values to target block phi nodes.
      const ir::Block& target_block = storage_.blocks()[target_block_idx];
      for (const ir::BlockParamIdx param_idx : target_block.block_params) {
        const ir::BlockParam& param = storage_.block_params()[param_idx];

        auto* phi =
            llvm::cast<llvm::PHINode>(values_.register_value(param.reg));

        const ir::Operand& arg_op =
            storage_.operands()[ops.head() + 1 + param_idx.idx];
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
          resolve_operand_value(storage_.operands()[ops.head()]);
      llvm::BasicBlock* true_block =
          values_.block(storage_.operands()[ops.head() + 1].as_block());
      llvm::BasicBlock* false_block =
          values_.block(storage_.operands()[ops.head() + 2].as_block());
      builder_->CreateCondBr(cond, true_block, false_block);
      break;
    }
    case Op::Switch: {
      // operands = [value, default_block, (case_imm, case_block)...].
      // Targets must not take block parameters in MVP.
      DCHECK(ops.size() >= 2);
      llvm::Value* value =
          resolve_operand_value(storage_.operands()[ops.head()]);
      llvm::BasicBlock* default_block =
          values_.block(storage_.operands()[ops.head() + 1].as_block());
      llvm::SwitchInst* switch_inst =
          builder_->CreateSwitch(value, default_block);
      for (u32 idx = 2; idx + 1 < ops.size(); idx += 2) {
        auto* case_value = llvm::cast<llvm::ConstantInt>(
            resolve_operand_value(storage_.operands()[ops.head() + idx]));
        llvm::BasicBlock* case_block =
            values_.block(storage_.operands()[ops.head() + idx + 1].as_block());
        switch_inst->addCase(case_value, case_block);
      }
      break;
    }
    case Op::Call: {
      DCHECK(ops.size() >= 1);

      // Head is callee
      const ir::Operand& callee_op = storage_.operands()[ops.head()];
      llvm::Function* callee_func = resolve_operand_function(callee_op);

      llvm::SmallVector<llvm::Value*, kFunctionArgsSooSize> args;
      args.reserve(ops.size() - 1);

      // All ops except head are args
      for (u32 idx = 1; idx < ops.size(); ++idx) {
        const ir::Operand& arg_op = storage_.operands()[ops.head() + idx];
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
        const ir::Operand& lhs = storage_.operands()[ops.head()];
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

llvm::Function* LlvmIrEmitter::create_function(
    const ir::FunctionMeta& function_meta) const {
  llvm::SmallVector<llvm::Type*, kFunctionArgsSooSize> parameter_types;
  parameter_types.reserve(function_meta.param_types.size());
  // DLOG("Function Name: {}, Param Count in Slice: {}",
  //      interner_->get(function_meta.name), function_meta.param_types.size());

  for (const ir::TypeIdx id : function_meta.param_types) {
    parameter_types.emplace_back(type(id));
  }

  llvm::FunctionType* func_type = llvm::FunctionType::get(
      type(function_meta.return_type),
      llvm::ArrayRef<llvm::Type*>(parameter_types), false);

  const std::string_view func_name = interner_->get(function_meta.name);

  // DLOG("Generated LLVM FTy NumParams: {}", func_type->getNumParams());

  llvm::Function* llvm_function = llvm::Function::Create(
      func_type, llvm::Function::ExternalLinkage, func_name, module_);
  DCHECK_MSG(llvm_function, "Failed to create LLVM function");
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
       storage_.immutables().idx_range()) {
    const ir::Immutable& immutable = storage_.immutables()[immutable_idx];

    const ir::TypeTag tag = storage_.types()[immutable.type.idx].tag;

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
      // DLOG("str_val: {}", str_val);
      llvm::Constant* str_const = builder_->CreateGlobalString(
          llvm::StringRef(str_val), "", 0, module_);
      values_.add_immutable(immutable_idx, str_const);
    } else {
      DCHECK_MSG(false, "Currently unsupported type found");
      UNREACHABLE();
    }
  }
}

void LlvmIrEmitter::setup_external_functions() {
  for (ir::ExternalFunctionIdx function_idx(0);
       function_idx.idx < storage_.external_functions().size();
       ++function_idx) {
    const ir::ExternalFunction& function =
        storage_.external_functions()[function_idx];

    values_.add_external_function(function_idx, create_function(function.meta));
  }
}

}  // namespace codegen_llvm
