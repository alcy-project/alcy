// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "codegen_llvm/llvm_ir_emitter.h"

#include <memory>
#include <string_view>
#include <utility>

#include "build/build_config.h"
#include "codegen_llvm/common.h"
#include "debug/dcheck.h"
#include "debug/dlog.h"
#include "debug/fatal.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_interner.h"
#include "ir/block.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/type_util.h"

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

inline void LlvmIrEmitter::init_value_map() {
  functions_.resize(storage_.functions().size(), nullptr);
  registers_.resize(storage_.registers().size(), nullptr);
  blocks_.resize(storage_.blocks().size(), nullptr);
  immutables_.resize(storage_.immutables().size(), nullptr);
  external_functions_.resize(storage_.external_functions().size(), nullptr);

  DCHECK_EQ(functions_.size(), storage_.functions().size());
  DCHECK_EQ(registers_.size(), storage_.registers().size());
  DCHECK_EQ(blocks_.size(), storage_.blocks().size());
  DCHECK_EQ(immutables_.size(), storage_.immutables().size());
  DCHECK_EQ(external_functions_.size(), storage_.external_functions().size());
}

inline void LlvmIrEmitter::check_state() {
  // DCHECK_MSG(storage_, "IR Storage is null");
  DCHECK_MSG(module_, "LLVM Module is null");
  DCHECK_MSG(builder_, "LLVM IR Builder is null");
  DCHECK_MSG(interner_, "String Interner is null");
}

inline llvm::Type* LlvmIrEmitter::type(ir::Type type) const {
  using T = ir::Type;

  switch (type) {
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
    default: {
      DLOG("unsupported type: {}", type);
      DCHECK(false);
      UNREACHABLE();
    }
  }
}

void LlvmIrEmitter::emit() && noexcept {
  check_state();

  init_value_map();
  setup_immutables();
  setup_external_functions();

  // PERF: Consider run this process concurrently.
  for (const ir::FunctionIdx function_idx : storage_.functions().idx_range()) {
    const ir::Function& function = storage_.functions()[function_idx];

    llvm::Function* llvm_function = create_function(function.meta);
    add_function(function_idx, llvm_function);
    emit_function(llvm_function, function);
  }

#if BUILD_FLAG(IS_DEBUG)
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
    add_block(block_idx, llvm::BasicBlock::Create(module_->getContext(), "",
                                                  llvm_function));
  }

  // Pre-generate phi nodes
  for (const ir::BlockIdx block_idx : function.blocks) {
    const ir::Block& block = storage_.blocks()[block_idx];
    llvm::BasicBlock* llvm_block = blocks_[block_idx];

    builder_->SetInsertPoint(llvm_block);
    for (const ir::BlockParamIdx param_id : block.block_params) {
      const ir::BlockParam& param = storage_.block_params()[param_id];

      llvm::PHINode* phi = builder_->CreatePHI(type(param.type), 0);
      add_register(param.reg, phi);
    }
  }

  for (const ir::BlockIdx block_idx : function.blocks) {
    const ir::Block& block = storage_.blocks()[block_idx];
    builder_->SetInsertPoint(blocks_[block_idx]);
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

  const ir::Instruction& i = instr;
  const ir::OperandIdxRange& ops = i.operands;

  using Op = ir::Opcode;

  switch (i.op) {
    case Op::Noop: {
      // no-operation
      break;
    }
    case Op::Alloca: {
      DCHECK(ops.size() == 1);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      add_register(i.dst, builder_->CreateAlloca(type(lhs.type),
                                                 resolve_operand_value(lhs)));
      break;
    }
    case Op::Load: {
      DCHECK(ops.size() == 1);
      const ir::Operand& ptr_op = storage_.operands()[ops.head()];

      const ir::Register& dst_reg = storage_.registers()[i.dst];
      llvm::Type* load_ty = type(dst_reg.type);

      add_register(
          i.dst, builder_->CreateLoad(load_ty, resolve_operand_value(ptr_op)));
      break;
    }
    case Op::Store: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      add_register(i.dst, builder_->CreateStore(resolve_operand_value(lhs),
                                                resolve_operand_value(rhs)));
      break;
    }
      // GetElementPtr
      // ExtractValue,
      // InsertValue,

    case Op::IntAdd: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      add_register(i.dst, builder_->CreateAdd(resolve_operand_value(lhs),
                                              resolve_operand_value(rhs)));
      break;
    }
    case Op::IntSub: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      add_register(i.dst, builder_->CreateSub(resolve_operand_value(lhs),
                                              resolve_operand_value(rhs)));
      break;
    }
    case Op::IntMul: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      add_register(i.dst, builder_->CreateMul(resolve_operand_value(lhs),
                                              resolve_operand_value(rhs)));
      break;
    }
      // IntDiv,   // Signed division
      // UintDiv,  // Unsigned division
      // IntRem,   // Signed remainder
      // UintRem,  // Unsigned remainder

    case Op::And: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      add_register(i.dst, builder_->CreateAnd(resolve_operand_value(lhs),
                                              resolve_operand_value(rhs)));
      break;
    }
    case Op::Or: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      add_register(i.dst, builder_->CreateOr(resolve_operand_value(lhs),
                                             resolve_operand_value(rhs)));
      break;
    }
    case Op::Xor: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      add_register(i.dst, builder_->CreateXor(resolve_operand_value(lhs),
                                              resolve_operand_value(rhs)));
      break;
    }
    case Op::ShiftLeft: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      add_register(i.dst, builder_->CreateShl(resolve_operand_value(lhs),
                                              resolve_operand_value(rhs)));
      break;
    }
    case Op::ArithmeticShiftRight: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      add_register(i.dst, builder_->CreateAShr(resolve_operand_value(lhs),
                                               resolve_operand_value(rhs)));
      break;
    }
    case Op::LogicalShiftRight: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      add_register(i.dst, builder_->CreateLShr(resolve_operand_value(lhs),
                                               resolve_operand_value(rhs)));
      break;
    }
    case Op::Not: {
      DCHECK(ops.size() == 1);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      add_register(i.dst, builder_->CreateNot(resolve_operand_value(lhs)));
      break;
    }
      // BitReverse,

      // Eq,
      // Ne,
      // Le,
      // Lt,
      // Ge,
      // Gt,

      // TypeCast,

      // Select,

    case Op::Br: {
      // operands[0] = Target block
      // operands[1..N] = parameters
      const ir::Operand& target_op = storage_.operands()[ops.head()];
      const ir::BlockIdx target_block_idx = target_op.as_block();
      llvm::BasicBlock* target_llvm_block = blocks_[target_block_idx];
      llvm::BasicBlock* current_llvm_block = builder_->GetInsertBlock();

      builder_->CreateBr(target_llvm_block);

      // Add incoming values to target block phi nodes.
      const ir::Block& target_block = storage_.blocks()[target_block_idx];
      for (const ir::BlockParamIdx param_idx : target_block.block_params) {
        const ir::BlockParam& param = storage_.block_params()[param_idx];

        auto* phi = llvm::cast<llvm::PHINode>(registers_[param.reg]);

        const ir::Operand& arg_op =
            storage_.operands()[ops.head() + 1 + param_idx.idx];
        llvm::Value* arg_val = resolve_operand_value(arg_op);

        phi->addIncoming(arg_val, current_llvm_block);
      }
      break;
    }
    // CondBr,
    // Switch,
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
        add_register(i.dst, call_inst);
      }
      break;
    }
    case Op::Ret: {
      DCHECK(ops.size() == 1);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      builder_->CreateRet(resolve_operand_value(lhs));
      break;
    }
    case Op::Unreachable: {
      DCHECK(ops.empty());
      builder_->CreateUnreachable();
      break;
    }

    // AtomicLoad,
    // AtomicStore,
    // AtomicRmw,
    // AtomicCompareExchange,
    // Fence,

    // Move,
    // Drop,
    // BorrowBegin,
    // BorrowEnd,

    // VecSplat,
    // VecExtract,
    // VecInsert,
    // VecReduce,
    default: {
      DLOG("Unknown opcode found: {}", i.op);
      UNREACHABLE();
      break;
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
    parameter_types.emplace_back(type(storage_.types()[id]));
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
  switch (op.tag) {
    case ir::OperandTag::Register: return registers_[op.as_register()];
    case ir::OperandTag::Block: return blocks_[op.as_block()];
    case ir::OperandTag::Immutable: return immutables_[op.as_immutable()];

    default:
      DLOG("Unknown operand tag found while resolving operand value.");
      UNREACHABLE();
  }
}

llvm::Function* LlvmIrEmitter::resolve_operand_function(
    const ir::Operand& op) const {
  switch (op.tag) {
    case ir::OperandTag::Function: return functions_[(op.as_function())];
    case ir::OperandTag::ExternalFunction:
      return external_functions_[(op.as_external_function())];
    default:
      DLOG("Unknown operand tag found while resolving operand function.");
      UNREACHABLE();
  }
}

inline void LlvmIrEmitter::add_function(ir::FunctionIdx id,
                                        llvm::Function* function) {
  DCHECK_MSG(function, "Function is null");
  functions_[id] = function;
}
inline void LlvmIrEmitter::add_register(ir::RegisterIdx id,
                                        llvm::Value* value) {
  DCHECK_MSG(value, "Value is null");
  registers_[id] = value;
}
inline void LlvmIrEmitter::add_block(ir::BlockIdx id, llvm::BasicBlock* block) {
  DCHECK_MSG(block, "Block is null");
  blocks_[id] = block;
}
inline void LlvmIrEmitter::add_immutable(ir::ImmutableIdx id,
                                         llvm::Constant* immutable) {
  DCHECK_MSG(immutable, "Immutable is null");
  immutables_[id] = immutable;
}
inline void LlvmIrEmitter::add_external_function(ir::ExternalFunctionIdx id,
                                                 llvm::Function* ex_function) {
  DCHECK_MSG(ex_function, "External function is null");
  external_functions_[id] = ex_function;
}

void LlvmIrEmitter::setup_immutables() {
  for (const ir::ImmutableIdx immutable_idx :
       storage_.immutables().idx_range()) {
    const ir::Immutable& immutable = storage_.immutables()[immutable_idx];

    if (ir::is_integer_type(immutable.type)) {
      // TODO: Add i128, u128, i256, u256, and arbitrary bit support with
      // llvm::APInt
      llvm::Constant* c = llvm::ConstantInt::get(
          type(immutable.type), immutable.as_u64_integer(),
          ir::is_signed_integer_type(immutable.type));
      DCHECK_MSG(c, "Failed to get integer constant from LLVM");

      add_immutable(immutable_idx, c);
    } else if (ir::is_float_type(immutable.type)) {
      llvm::Constant* c =
          llvm::ConstantFP::get(type(immutable.type), immutable.as_f64_fp());
      DCHECK_MSG(c, "Failed to get fp constant from LLVM");

      add_immutable(immutable_idx, c);
    } else if (immutable.type == ir::Type::Str) {
      const std::string_view str_val =
          interner_->get(immutable.data.str_id_value);
      DLOG("str_val: {}", str_val);
      llvm::Constant* str_const = builder_->CreateGlobalString(
          llvm::StringRef(str_val), "", 0, module_);
      add_immutable(immutable_idx, str_const);
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

    add_external_function(function_idx, create_function(function.meta));
  }
}

}  // namespace codegen_llvm
