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
      const ir::Operand& size_op = storage_->operands()[ops.head()];
      llvm::Type* elem_ty = type(storage_->registers()[i.dst].type);
      llvm::Value* size = resolve_operand_value(size_op);
      // A fixed-size slot belongs to the frame, not to the block that
      // names it. One inside a loop body otherwise takes a fresh slot
      // every iteration, and the stack walks down until it meets the
      // heap - twenty thousand turns on a plain `Vec` were enough. A
      // slot whose size is a value stays where it is: reserving it once
      // would fix a length it does not have.
      llvm::AllocaInst* alloca = nullptr;
      if (llvm::isa<llvm::ConstantInt>(size)) {
        llvm::BasicBlock& entry =
            builder_->GetInsertBlock()->getParent()->getEntryBlock();
        llvm::IRBuilder<> entry_builder(&entry, entry.begin());
        alloca = entry_builder.CreateAlloca(elem_ty, size);
      } else {
        alloca = builder_->CreateAlloca(elem_ty, size);
      }
      values_.add_register(i.dst, alloca);
      values_.add_alloca_type(i.dst, elem_ty);
      break;
    }
    case Op::Load: {
      DCHECK(ops.size() == 1);
      if (!i.dst.is_valid()) {
        break;
      }
      const ir::Operand& ptr_op = storage_->operands()[ops.head()];

      const ir::Register& dst_reg = storage_->registers()[i.dst];
      llvm::Type* load_ty = type(dst_reg.type);
      // A zero-sized value has no storage to read, so the access is a
      // no-op and the placeholder is the result. Emitting it would
      // hand a sanitizer a zero-byte check over an address that is
      // valid for nothing; the address names a value, never bytes.
      if (module_->getDataLayout().getTypeStoreSize(load_ty) == 0) {
        values_.add_register(i.dst, llvm::UndefValue::get(load_ty));
        break;
      }
      values_.add_register(
          i.dst, builder_->CreateLoad(load_ty, resolve_operand_value(ptr_op)));
      break;
    }
    case Op::Store: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_->operands()[ops.head()];
      const ir::Operand& rhs = storage_->operands()[ops.head() + 1];
      // Nothing crosses for a zero-sized value either, so the store
      // is the same no-op; a store's own value is the placeholder its
      // destination asks for.
      llvm::Type* stored_ty = type(lhs.type);
      if (module_->getDataLayout().getTypeStoreSize(stored_ty) == 0) {
        if (i.dst.is_valid()) {
          values_.add_register(i.dst, llvm::UndefValue::get(stored_ty));
        }
        break;
      }
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
      // The IR's alignment is the element's; zero means byte alignment.
      const llvm::Align align(static_cast<u64>(1) << i.flags.mem_align_log2);
      builder_->CreateMemCpy(
          resolve_operand_value(storage_->operands()[ops.head()]),
          llvm::MaybeAlign(align),
          resolve_operand_value(storage_->operands()[ops.head() + 1]),
          llvm::MaybeAlign(align),
          resolve_operand_value(storage_->operands()[ops.head() + 2]));
      break;
    }
    case Op::GetElementPtr: {
      // operands = [base_ptr, index...]. The element type is recovered from
      // the base pointer's tracked site; nested projections (which have
      // no site of their own) carry it as their register type.
      DCHECK(ops.size() >= 2);
      const ir::Operand& base_op = storage_->operands()[ops.head()];
      DCHECK(base_op.is<ir::RegisterIdx>());
      const ir::RegisterIdx base_reg = base_op.as_register();
      llvm::Type* elem_ty = values_.alloca_type(base_reg);
      if (elem_ty == nullptr) {
        // Nested projections carry the pointee as their register
        // type; dereference reference tags once to reach it.
        ir::TypeIdx base_ty = storage_->registers()[base_reg].type;
        ir::TypeTag tag = storage_->types()[base_ty.idx].tag;
        if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef ||
            tag == ir::TypeTag::RawPtr || tag == ir::TypeTag::RawMutPtr) {
          base_ty =
              storage_->ref_types()[storage_->types()[base_ty.idx].as_ref()]
                  .pointee;
        }
        elem_ty = type(base_ty);
      }
      DCHECK_MSG(elem_ty, "GetElementPtr of untracked pointer");
      llvm::SmallVector<llvm::Value*, FUNCTION_ARGS_SOO_SIZE> indices;
      for (u32 idx = 1; idx < ops.size(); ++idx) {
        indices.push_back(
            resolve_operand_value(storage_->operands()[ops.head() + idx]));
      }
      llvm::Value* base = resolve_operand_value(base_op);
      llvm::Value* gep =
          i.flags.inbounds ? builder_->CreateInBoundsGEP(elem_ty, base, indices)
                           : builder_->CreateGEP(elem_ty, base, indices);
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
      const ir::Operand& base_op = storage_->operands()[ops.head()];
      DCHECK(base_op.is<ir::RegisterIdx>());
      DCHECK(i.dst.is_valid());
      const ir::TypeIdx dst_ty = storage_->registers()[i.dst].type;
      const ir::TypeTag dst_tag = storage_->types()[dst_ty.idx].tag;
      DCHECK(dst_tag == ir::TypeTag::Ref || dst_tag == ir::TypeTag::MutRef ||
             dst_tag == ir::TypeTag::RawPtr ||
             dst_tag == ir::TypeTag::RawMutPtr);
      llvm::Type* elem_ty =
          type(storage_->ref_types()[storage_->types()[dst_ty.idx].as_ref()]
                   .pointee);
      llvm::SmallVector<llvm::Value*, 1> indices{
          resolve_operand_value(storage_->operands()[ops.head() + 1])};
      llvm::Value* base = resolve_operand_value(base_op);
      values_.add_register(
          i.dst, i.flags.inbounds
                     ? builder_->CreateInBoundsGEP(elem_ty, base, indices)
                     : builder_->CreateGEP(elem_ty, base, indices));
      break;
    }
    case Op::EnumTagLoad: {
      // operands = [place]. The representation is the enum's: a niche
      // synthesizes the discriminant from the payload's spare pattern
      // (ADR-0087), a plain enum reads its tag slot.
      DCHECK(ops.size() == 1);
      if (!i.dst.is_valid()) {
        break;
      }
      const ir::Operand& place_op = storage_->operands()[ops.head()];
      llvm::Value* place = resolve_operand_value(place_op);
      const ir::TypeIdx enum_ty =
          ir::enum_place_type(storage_->state(), place_op.type);
      const ir::EnumNiche niche = ir::enum_niche(storage_->state(), enum_ty);
      llvm::Value* disc = nullptr;
      llvm::Type* const i32_ty = builder_->getInt32Ty();
      if (!niche.has) {
        llvm::Value* tag_ptr = builder_->CreateInBoundsGEP(
            type(enum_ty), place,
            {builder_->getInt32(0), builder_->getInt32(0)});
        disc = builder_->CreateLoad(i32_ty, tag_ptr);
      } else {
        llvm::Value* payload = builder_->CreateLoad(type(niche.slot), place);
        llvm::Value* fieldless = nullptr;
        if (niche.slot == niche.payload) {
          fieldless = builder_->CreateICmpEQ(
              payload, llvm::ConstantPointerNull::get(builder_->getPtrTy()));
        } else {
          fieldless = builder_->CreateICmpUGT(
              payload,
              llvm::ConstantInt::get(type(niche.slot),
                                     static_cast<u64>(niche.sentinel) - 1));
        }
        llvm::Value* step = builder_->CreateZExt(fieldless, i32_ty);
        llvm::Value* carrying =
            llvm::ConstantInt::get(i32_ty, static_cast<u64>(niche.carrying));
        disc = niche.fieldless < niche.carrying
                   ? builder_->CreateSub(carrying, step)
                   : builder_->CreateAdd(carrying, step);
      }
      values_.add_register(i.dst, disc);
      break;
    }
    case Op::EnumTagSet: {
      // operands = [place, tag]. A niche writes its fieldless spell for
      // the fieldless variant; the carrying variant's payload follows,
      // so a zero stands where that payload's store lands.
      DCHECK(ops.size() == 2);
      const ir::Operand& place_op = storage_->operands()[ops.head()];
      llvm::Value* place = resolve_operand_value(place_op);
      llvm::Value* tag =
          resolve_operand_value(storage_->operands()[ops.head() + 1]);
      const ir::TypeIdx enum_ty =
          ir::enum_place_type(storage_->state(), place_op.type);
      const ir::EnumNiche niche = ir::enum_niche(storage_->state(), enum_ty);
      llvm::Type* const i32_ty = builder_->getInt32Ty();
      if (!niche.has) {
        llvm::Value* tag_ptr = builder_->CreateInBoundsGEP(
            type(enum_ty), place,
            {builder_->getInt32(0), builder_->getInt32(0)});
        builder_->CreateStore(tag, tag_ptr);
        break;
      }
      llvm::Value* fieldless = builder_->CreateICmpEQ(
          tag,
          llvm::ConstantInt::get(i32_ty, static_cast<u64>(niche.fieldless)));
      llvm::Type* const slot_ty = type(niche.slot);
      llvm::Value* spell =
          niche.slot == niche.payload
              ? static_cast<llvm::Value*>(
                    llvm::ConstantPointerNull::get(builder_->getPtrTy()))
              : static_cast<llvm::Value*>(llvm::ConstantInt::get(
                    slot_ty, static_cast<u64>(niche.sentinel)));
      builder_->CreateStore(
          builder_->CreateSelect(fieldless, spell,
                                 llvm::Constant::getNullValue(slot_ty)),
          place);
      break;
    }
    case Op::EnumFieldAddr: {
      // operands = [place, variant, field]. A niche's payload is the
      // slot itself; a plain enum's field lies in the area after the
      // tag, at the offset the variant's fields publish.
      DCHECK(ops.size() == 3);
      if (!i.dst.is_valid()) {
        break;
      }
      const ir::Operand& place_op = storage_->operands()[ops.head()];
      llvm::Value* place = resolve_operand_value(place_op);
      const ir::TypeIdx enum_ty =
          ir::enum_place_type(storage_->state(), place_op.type);
      const ir::EnumNiche niche = ir::enum_niche(storage_->state(), enum_ty);
      if (niche.has) {
        values_.add_register(i.dst, place);
        break;
      }
      const u64 variant =
          llvm::cast<llvm::ConstantInt>(
              resolve_operand_value(storage_->operands()[ops.head() + 1]))
              ->getZExtValue();
      const u64 field =
          llvm::cast<llvm::ConstantInt>(
              resolve_operand_value(storage_->operands()[ops.head() + 2]))
              ->getZExtValue();
      const ir::EnumType& shape =
          storage_->enum_types()[storage_->types()[enum_ty.idx].as_enum()];
      DCHECK(variant < shape.variants.size());
      const ir::TypeIdxRange fields =
          storage_->enum_variant_types()[shape.variants[variant]].fields;
      DCHECK(field < fields.size());
      // The area member sits at the alignment the payload needs, so
      // the field's offset within the area is all that is left to add:
      // the struct's own padding is the discriminant's room.
      const u64 at = ir::field_offset(storage_->state(), fields,
                                      static_cast<u32>(field), width_);
      llvm::Value* area_ptr = builder_->CreateInBoundsGEP(
          type(enum_ty), place, {builder_->getInt32(0), builder_->getInt32(1)});
      values_.add_register(
          i.dst, builder_->CreateInBoundsGEP(builder_->getInt8Ty(), area_ptr,
                                             {builder_->getInt64(at)}));
      break;
    }
    case Op::ExtractValue: {
      // operands = [aggregate, index...]; indexes are integer immediates.
      DCHECK(ops.size() >= 2);
      llvm::Value* agg =
          resolve_operand_value(storage_->operands()[ops.head()]);
      llvm::SmallVector<u32, FUNCTION_ARGS_SOO_SIZE> indices;
      for (u32 idx = 1; idx < ops.size(); ++idx) {
        const ir::Operand& index_op = storage_->operands()[ops.head() + idx];
        DCHECK(index_op.is<ir::ImmutableIdx>());
        const ir::Immutable& imm =
            storage_->immutables()[index_op.as_immutable()];
        const ir::TypeTag tag = storage_->types()[imm.type.idx].tag;
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
      llvm::Value* agg =
          resolve_operand_value(storage_->operands()[ops.head()]);
      llvm::Value* field_value =
          resolve_operand_value(storage_->operands()[ops.head() + 1]);
      llvm::SmallVector<u32, FUNCTION_ARGS_SOO_SIZE> indices;
      for (u32 idx = 2; idx < ops.size(); ++idx) {
        const ir::Operand& index_op = storage_->operands()[ops.head() + idx];
        DCHECK(index_op.is<ir::ImmutableIdx>());
        const ir::Immutable& imm =
            storage_->immutables()[index_op.as_immutable()];
        const ir::TypeTag tag = storage_->types()[imm.type.idx].tag;
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
      const ir::Operand& ptr_op = storage_->operands()[ops.head()];
      const ir::Register& dst_reg = storage_->registers()[i.dst];
      llvm::LoadInst* load = builder_->CreateLoad(
          type(dst_reg.type), resolve_operand_value(ptr_op));
      load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
      values_.add_register(i.dst, load);
      break;
    }
    case Op::AtomicStore: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_->operands()[ops.head()];
      const ir::Operand& rhs = storage_->operands()[ops.head() + 1];
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
      const ir::Operand& ptr_op = storage_->operands()[ops.head()];
      const ir::Operand& val_op = storage_->operands()[ops.head() + 1];
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
          resolve_operand_value(storage_->operands()[ops.head()]),
          resolve_operand_value(storage_->operands()[ops.head() + 1]),
          resolve_operand_value(storage_->operands()[ops.head() + 2]),
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
