// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/common.h"
#include "codegen_llvm/llvm_ir_emitter.h"
#include "config/build_config.h"
#include "debug/dcheck.h"
#include "debug/dlog.h"
#include "debug/fatal.h"
#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/instruction.h"
#include "ir/instruction_flags.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"
#include "ir/type_util.h"

#if BUILD_FLAG(IS_DEBUG)
#include "ir/formatter.h"  // IWYU pragma: keep
#endif

#if BUILD_FLAG(IS_OS_WIN)
#include <malloc.h>
#endif

namespace codegen_llvm {

namespace {

llvm::AtomicRMWInst::BinOp rmw_op(ir::AtomicRmwOp op) {
  using Op = ir::AtomicRmwOp;
  using BinOp = llvm::AtomicRMWInst::BinOp;
  switch (op) {
    case Op::Add: return BinOp::Add;
    case Op::Sub: return BinOp::Sub;
    case Op::And: return BinOp::And;
    case Op::Or: return BinOp::Or;
    case Op::Xor: return BinOp::Xor;
    case Op::Exchange: return BinOp::Xchg;
    default: return BinOp::BAD_BINOP;
  }
}

}  // namespace

void LlvmIrEmitter::emit_memory(const ir::Instruction& instr) {
  check_state();

  const ir::Instruction& i = instr;
  const ir::OperandIdxRange& ops = i.operands;

  using Op = ir::Opcode;

  switch (i.op) {
    case Op::Alloca: {
      // The allocated element type comes from the destination register;
      // the operand only provides the array size.
      DCHECK(ops.size() == 1);
      if (!i.dst.is_valid()) {
        break;
      }
      const ir::Operand& size_op = storage_.operands()[ops.head()];
      llvm::Type* elem_ty = type(storage_.registers()[i.dst].type);
      llvm::Value* size = resolve_operand_value(size_op);
      llvm::AllocaInst* alloca = builder_->CreateAlloca(elem_ty, size);
      values_.add_register(i.dst, alloca);
      values_.add_alloca_type(i.dst, elem_ty);
      break;
    }
    case Op::Load: {
      DCHECK(ops.size() == 1);
      if (!i.dst.is_valid()) {
        break;
      }
      const ir::Operand& ptr_op = storage_.operands()[ops.head()];

      const ir::Register& dst_reg = storage_.registers()[i.dst];
      llvm::Type* load_ty = type(dst_reg.type);

      values_.add_register(
          i.dst, builder_->CreateLoad(load_ty, resolve_operand_value(ptr_op)));
      break;
    }
    case Op::Store: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      llvm::StoreInst* store = builder_->CreateStore(
          resolve_operand_value(lhs), resolve_operand_value(rhs));
      if (i.dst.is_valid()) {
        values_.add_register(i.dst, store);
      }
      break;
    }
    case Op::Memcopy: {
      DCHECK(ops.size() == 3);
      DCHECK(!i.dst.is_valid());
      builder_->CreateMemCpy(
          resolve_operand_value(storage_.operands()[ops.head()]),
          llvm::MaybeAlign(1),
          resolve_operand_value(storage_.operands()[ops.head() + 1]),
          llvm::MaybeAlign(1),
          resolve_operand_value(storage_.operands()[ops.head() + 2]));
      break;
    }
    case Op::GetElementPtr: {
      // operands = [base_ptr, index...]. The element type is recovered from
      // the base pointer's tracked site; nested projections (which have
      // no site of their own) carry it as their register type.
      DCHECK(ops.size() >= 2);
      const ir::Operand& base_op = storage_.operands()[ops.head()];
      DCHECK(base_op.is<ir::RegisterIdx>());
      const ir::RegisterIdx base_reg = base_op.as_register();
      llvm::Type* elem_ty = values_.alloca_type(base_reg);
      if (elem_ty == nullptr) {
        // Nested projections carry the pointee as their register
        // type; dereference reference tags once to reach it.
        ir::TypeIdx base_ty = storage_.registers()[base_reg].type;
        ir::TypeTag tag = storage_.types()[base_ty.idx].tag;
        if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
          base_ty = storage_.ref_types()[storage_.types()[base_ty.idx].as_ref()]
                        .pointee;
        }
        elem_ty = type(base_ty);
      }
      DCHECK_MSG(elem_ty, "GetElementPtr of untracked pointer");
      llvm::SmallVector<llvm::Value*, kFunctionArgsSooSize> indices;
      for (u32 idx = 1; idx < ops.size(); ++idx) {
        indices.push_back(
            resolve_operand_value(storage_.operands()[ops.head() + idx]));
      }
      llvm::Value* gep =
          builder_->CreateGEP(elem_ty, resolve_operand_value(base_op), indices);
      if (i.dst.is_valid()) {
        values_.add_register(i.dst, gep);
      }
      break;
    }
    case Op::ElemOffset: {
      // operands = [base_ptr, index(integer)]. The destination register
      // carries the element reference type, so the pointee is the
      // element type regardless of the base pointer's provenance.
      DCHECK(ops.size() == 2);
      const ir::Operand& base_op = storage_.operands()[ops.head()];
      DCHECK(base_op.is<ir::RegisterIdx>());
      DCHECK(i.dst.is_valid());
      const ir::TypeIdx dst_ty = storage_.registers()[i.dst].type;
      const ir::TypeTag dst_tag = storage_.types()[dst_ty.idx].tag;
      DCHECK(dst_tag == ir::TypeTag::Ref || dst_tag == ir::TypeTag::MutRef);
      llvm::Type* elem_ty = type(
          storage_.ref_types()[storage_.types()[dst_ty.idx].as_ref()].pointee);
      llvm::SmallVector<llvm::Value*, 1> indices{
          resolve_operand_value(storage_.operands()[ops.head() + 1])};
      values_.add_register(
          i.dst, builder_->CreateGEP(elem_ty, resolve_operand_value(base_op),
                                     indices));
      break;
    }
    case Op::ExtractValue: {
      // operands = [aggregate, index...]; indexes are integer immediates.
      DCHECK(ops.size() >= 2);
      llvm::Value* agg = resolve_operand_value(storage_.operands()[ops.head()]);
      llvm::SmallVector<u32, kFunctionArgsSooSize> indices;
      for (u32 idx = 1; idx < ops.size(); ++idx) {
        const ir::Operand& index_op = storage_.operands()[ops.head() + idx];
        DCHECK(index_op.is<ir::ImmutableIdx>());
        const ir::Immutable& imm =
            storage_.immutables()[index_op.as_immutable()];
        const ir::TypeTag tag = storage_.types()[imm.type.idx].tag;
        DCHECK(ir::is_integer_type(tag));
        indices.push_back(static_cast<u32>(imm.as_u64_integer(tag)));
      }
      if (i.dst.is_valid()) {
        values_.add_register(i.dst, builder_->CreateExtractValue(agg, indices));
      }
      break;
    }
    case Op::InsertValue: {
      // operands = [aggregate, field_value, index...].
      DCHECK(ops.size() >= 3);
      llvm::Value* agg = resolve_operand_value(storage_.operands()[ops.head()]);
      llvm::Value* field_value =
          resolve_operand_value(storage_.operands()[ops.head() + 1]);
      llvm::SmallVector<u32, kFunctionArgsSooSize> indices;
      for (u32 idx = 2; idx < ops.size(); ++idx) {
        const ir::Operand& index_op = storage_.operands()[ops.head() + idx];
        DCHECK(index_op.is<ir::ImmutableIdx>());
        const ir::Immutable& imm =
            storage_.immutables()[index_op.as_immutable()];
        const ir::TypeTag tag = storage_.types()[imm.type.idx].tag;
        DCHECK(ir::is_integer_type(tag));
        indices.push_back(static_cast<u32>(imm.as_u64_integer(tag)));
      }
      if (i.dst.is_valid()) {
        values_.add_register(
            i.dst, builder_->CreateInsertValue(agg, field_value, indices));
      }
      break;
    }
    case Op::AtomicLoad: {
      DCHECK(ops.size() == 1);
      if (!i.dst.is_valid()) {
        break;
      }
      const ir::Operand& ptr_op = storage_.operands()[ops.head()];
      const ir::Register& dst_reg = storage_.registers()[i.dst];
      llvm::LoadInst* load = builder_->CreateLoad(
          type(dst_reg.type), resolve_operand_value(ptr_op));
      load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
      values_.add_register(i.dst, load);
      break;
    }
    case Op::AtomicStore: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      llvm::StoreInst* store = builder_->CreateStore(
          resolve_operand_value(lhs), resolve_operand_value(rhs));
      store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
      if (i.dst.is_valid()) {
        values_.add_register(i.dst, store);
      }
      break;
    }
    case Op::AtomicRmw: {
      DCHECK(ops.size() == 2);
      const ir::Operand& ptr_op = storage_.operands()[ops.head()];
      const ir::Operand& val_op = storage_.operands()[ops.head() + 1];
      llvm::AtomicRMWInst* rmw = builder_->CreateAtomicRMW(
          rmw_op(i.flags.rmw_op), resolve_operand_value(ptr_op),
          resolve_operand_value(val_op), llvm::MaybeAlign(),
          llvm::AtomicOrdering::SequentiallyConsistent);
      if (i.dst.is_valid()) {
        values_.add_register(i.dst, rmw);
      }
      break;
    }
    case Op::AtomicCompareExchange: {
      // operands = [ptr, cmp, new].
      DCHECK(ops.size() == 3);
      llvm::AtomicCmpXchgInst* cmpxchg = builder_->CreateAtomicCmpXchg(
          resolve_operand_value(storage_.operands()[ops.head()]),
          resolve_operand_value(storage_.operands()[ops.head() + 1]),
          resolve_operand_value(storage_.operands()[ops.head() + 2]),
          llvm::MaybeAlign(), llvm::AtomicOrdering::SequentiallyConsistent,
          llvm::AtomicOrdering::SequentiallyConsistent);
      if (i.dst.is_valid()) {
        values_.add_register(i.dst, cmpxchg);
      }
      break;
    }
    case Op::Fence: {
      DCHECK(ops.empty());
      builder_->CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
      break;
    }
    default: {
      DLOG("Unknown memory opcode found: {}", i.op);
      UNREACHABLE();
    }
  }
}

}  // namespace codegen_llvm
