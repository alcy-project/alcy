// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/llvm_ir_emitter.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "codegen_llvm/common.h"
#include "config/build_config.h"
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
#include "symbol/mangle.h"

#if BUILD_FLAG(IS_DEBUG)
#include "ir/formatter.h"  // IWYU pragma: keep
#endif

namespace codegen_llvm {

LlvmIrEmitter::LlvmIrEmitter(llvm::Module* module,
                             ir::VerifiedStorage storage,
                             str::StringInterner* interner,
                             ir::PointerWidth width)
    : module_(module),
      storage_(std::move(storage)),
      builder_(std::make_unique<IRBuilder>(module_->getContext())),
      interner_(interner),
      width_(width) {}

void LlvmIrEmitter::check_state() {
  // DCHECK_MSG(storage_, "IR Storage is null");
  DCHECK_MSG(module_, "LLVM Module is null");
  DCHECK_MSG(builder_, "LLVM IR Builder is null");
  DCHECK_MSG(interner_, "String Interner is null");
}

llvm::Type* LlvmIrEmitter::type(ir::TypeIdx idx) const {
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
    case T::Ptr: return builder_->getPtrTy();
    case T::Ref: return builder_->getPtrTy();
    case T::MutRef: return builder_->getPtrTy();
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
    case T::Enum: {
      // A discriminant, then the payload in the slot itself. Keeping the
      // payload here rather than behind a pointer is what lets an enum
      // value outlive the frame that built it. The area is sized from
      // ir::type_layout, which the lowerer reads the same numbers from,
      // so a field's address resolves identically on both sides.
      llvm::SmallVector<llvm::Type*, 2> slot_types;
      slot_types.emplace_back(builder_->getInt32Ty());
      slot_types.emplace_back(enum_payload_area_type(idx));
      llvm::StructType* slot =
          llvm::StructType::get(module_->getContext(), slot_types);
      // The layout ir::type_layout publishes is what field offsets are
      // computed against, so the type built here has to match it. The
      // module has no DataLayout this early, so this can only be checked
      // once one is installed.
      const ir::TypeLayout expected =
          ir::type_layout(storage_->state(), idx, width_);
      if (!module_->getDataLayout().isDefault()) {
        const llvm::DataLayout& dl = module_->getDataLayout();
        DCHECK_EQ(dl.getTypeAllocSize(slot).getFixedValue(), expected.size);
        DCHECK_EQ(dl.getABITypeAlign(slot).value(), expected.align);
      }
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
// on x86 and is still wrong on a strict-alignment target.
//
// The carrier is chosen from the published alignment rather than from the
// module's DataLayout, because a type is built before the module has one.
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

void LlvmIrEmitter::emit() && noexcept {
  check_state();

  values_.resize_functions(storage_->functions().size());
  values_.resize_registers(storage_->registers().size());
  values_.resize_blocks(storage_->blocks().size());
  values_.resize_immutables(storage_->immutables().size());
  values_.resize_external_functions(storage_->external_functions().size());
  values_.resize_alloca_types(storage_->registers().size());

  setup_immutables();
  setup_external_functions();

  // Declare every function before defining any body so forward
  // calls resolve regardless of declaration order.
  llvm::Function* entry_function = nullptr;
  ir::TypeTag entry_return = ir::TypeTag::Void;
  for (const ir::FunctionIdx function_idx : storage_->functions().idx_range()) {
    const ir::Function& function = storage_->functions()[function_idx];

    llvm::Function* llvm_function = nullptr;
    if (entry_function == nullptr && is_entry_candidate(function)) {
      // The user entry becomes an implementation detail; a C-ABI
      // `main` below adapts its return form to an exit code.
      ir::FunctionMeta renamed = function.meta;
      renamed.name = interner_->intern("alcy_main");
      llvm_function = create_function(renamed);
      entry_function = llvm_function;
      entry_return = storage_->types()[function.meta.return_type.idx].tag;
    } else {
      llvm_function = create_function(function.meta);
    }
    values_.add_function(function_idx, llvm_function);
  }
  // PERF: Consider run this process concurrently.
  for (const ir::FunctionIdx function_idx : storage_->functions().idx_range()) {
    const ir::Function& function = storage_->functions()[function_idx];
    emit_function(values_.function(function_idx), function);
  }
  if (entry_function != nullptr) {
    emit_entry(entry_function, entry_return);
  }

#if BUILD_FLAG(IS_DEBUG)
  // Storage arrives verified (StorageBuilder::build is the only
  // production path), so only the generated LLVM module is rechecked
  // here; re-running the alcy verifier would repeat build-time work.
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

  // Pre-generate phi nodes. The entry block (first in function
  // order) receives LLVM function arguments directly instead of
  // PHIs: lowering places one block parameter per declared parameter.
  unsigned arg_no = 0;
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

  for (const ir::BlockIdx block_idx : function.blocks) {
    const ir::Block& block = storage_->blocks()[block_idx];
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
    const ir::Instruction& instr = storage_->instrs()[instr_idx];
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

      // Add incoming values to target block phi nodes.
      const ir::Block& target_block = storage_->blocks()[target_block_idx];
      for (const ir::BlockParamIdx param_idx : target_block.block_params) {
        const ir::BlockParam& param = storage_->block_params()[param_idx];

        auto* phi =
            llvm::cast<llvm::PHINode>(values_.register_value(param.reg));

        const ir::Operand& arg_op =
            storage_->operands()[ops.head() + 1 + param_idx.idx];
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

      // Head is callee
      const ir::Operand& callee_op = storage_->operands()[ops.head()];
      llvm::Function* callee_func = resolve_operand_function(callee_op);

      llvm::SmallVector<llvm::Value*, FUNCTION_ARGS_SOO_SIZE> args;
      args.reserve(ops.size() - 1);

      // All ops except head are args
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

// The linker-visible name of a function. A signature encodes to a
// symbol; a foreign one keeps the name it was declared with.
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

  // A symbol is derived from the signature, so a source name can never
  // reach the linker. A C entry point keeps the name it was declared
  // with, and the synthesized program entry is named below.
  const std::string func_name = linkable_name(function_meta);

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
  check_state();
  llvm::LLVMContext& context = module_->getContext();
  llvm::Function* main_function = llvm::Function::Create(
      llvm::FunctionType::get(builder_->getInt32Ty(), false),
      llvm::Function::ExternalLinkage, "main", module_);
  llvm::BasicBlock* entry_block =
      llvm::BasicBlock::Create(context, "", main_function);
  builder_->SetInsertPoint(entry_block);
  llvm::Value* result = builder_->CreateCall(entry_function);
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
    llvm::BasicBlock* ok_block =
        llvm::BasicBlock::Create(context, "", main_function);
    llvm::BasicBlock* err_block =
        llvm::BasicBlock::Create(context, "", main_function);
    builder_->CreateCondBr(is_ok, ok_block, err_block);
    builder_->SetInsertPoint(ok_block);
    builder_->CreateRet(llvm::ConstantInt::get(builder_->getInt32Ty(), 0));
    builder_->SetInsertPoint(err_block);
    llvm::Type* len_ty = width_ == ir::PointerWidth::W64
                             ? builder_->getInt64Ty()
                             : builder_->getInt32Ty();
    llvm::FunctionCallee panic = module_->getOrInsertFunction(
        "alcy_panic",
        llvm::FunctionType::get(builder_->getVoidTy(),
                                {builder_->getPtrTy(), len_ty}, false));
    llvm::GlobalVariable* message =
        builder_->CreateGlobalString("main returned Err", "", 0, module_);
    llvm::Value* zero = llvm::ConstantInt::get(builder_->getInt32Ty(), 0);
    llvm::SmallVector<llvm::Value*, 2> indices{zero, zero};
    llvm::Value* bytes =
        builder_->CreateInBoundsGEP(message->getValueType(), message, indices);
    llvm::Value* len = llvm::ConstantInt::get(
        len_ty, llvm::StringRef("main returned Err").size());
    builder_->CreateCall(panic, {bytes, len});
    builder_->CreateUnreachable();
    return;
  }
  builder_->CreateRet(llvm::ConstantInt::get(builder_->getInt32Ty(), 0));
}

}  // namespace codegen_llvm
