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
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/register.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/type_util.h"

#if BUILD_FLAG(IS_DEBUG)
#include "ir/formatter.h"  // IWYU pragma: keep
#endif

namespace codegen_llvm {

namespace {

llvm::CmpInst::Predicate int_predicate(ir::Opcode op, bool is_signed) {
  using Op = ir::Opcode;
  using Pred = llvm::CmpInst::Predicate;
  switch (op) {
    case Op::Eq: return Pred::ICMP_EQ;
    case Op::Ne: return Pred::ICMP_NE;
    case Op::Lt: return is_signed ? Pred::ICMP_SLT : Pred::ICMP_ULT;
    case Op::Le: return is_signed ? Pred::ICMP_SLE : Pred::ICMP_ULE;
    case Op::Gt: return is_signed ? Pred::ICMP_SGT : Pred::ICMP_UGT;
    case Op::Ge: return is_signed ? Pred::ICMP_SGE : Pred::ICMP_UGE;
    default: return Pred::BAD_ICMP_PREDICATE;
  }
}

llvm::CmpInst::Predicate float_predicate(ir::Opcode op) {
  using Op = ir::Opcode;
  using Pred = llvm::CmpInst::Predicate;
  switch (op) {
    case Op::Eq: return Pred::FCMP_OEQ;
    case Op::Ne: return Pred::FCMP_ONE;
    case Op::Lt: return Pred::FCMP_OLT;
    case Op::Le: return Pred::FCMP_OLE;
    case Op::Gt: return Pred::FCMP_OGT;
    case Op::Ge: return Pred::FCMP_OGE;
    default: return Pred::BAD_FCMP_PREDICATE;
  }
}

// Binary arithmetic and bitwise opcodes, each paired with a thunk that
// builds the LLVM instruction. Every one of these takes exactly two
// operands and writes `dst`, so a table replaces the switch: a new
// opcode is one row plus one one-line thunk rather than a case block.
// The builder methods disagree on their trailing fast-math and
// metadata parameters, and a member pointer cannot carry their
// defaults, so each thunk supplies what its method needs.
using Builder = LlvmIrEmitter::IRBuilder;
using BinaryEmit = llvm::Value* (*)(Builder&, llvm::Value*, llvm::Value*);

#define ALCY_BINARY(Name, Call)                                   \
  llvm::Value* Name(Builder& b, llvm::Value* l, llvm::Value* r) { \
    return Call;                                                  \
  }

ALCY_BINARY(emit_add, b.CreateAdd(l, r))
ALCY_BINARY(emit_sub, b.CreateSub(l, r))
ALCY_BINARY(emit_mul, b.CreateMul(l, r))
ALCY_BINARY(emit_sdiv, b.CreateSDiv(l, r))
ALCY_BINARY(emit_udiv, b.CreateUDiv(l, r))
ALCY_BINARY(emit_srem, b.CreateSRem(l, r))
ALCY_BINARY(emit_urem, b.CreateURem(l, r))
ALCY_BINARY(emit_fadd, b.CreateFAdd(l, r))
ALCY_BINARY(emit_fsub, b.CreateFSub(l, r))
ALCY_BINARY(emit_fmul, b.CreateFMul(l, r))
ALCY_BINARY(emit_fdiv, b.CreateFDiv(l, r))
ALCY_BINARY(emit_and, b.CreateAnd(l, r))
ALCY_BINARY(emit_or, b.CreateOr(l, r))
ALCY_BINARY(emit_xor, b.CreateXor(l, r))
ALCY_BINARY(emit_shl, b.CreateShl(l, r))
ALCY_BINARY(emit_ashr, b.CreateAShr(l, r))
ALCY_BINARY(emit_lshr, b.CreateLShr(l, r))

#undef ALCY_BINARY

struct BinaryOp {
  ir::Opcode op;
  BinaryEmit emit;
};

constexpr BinaryOp BINARY_OPS[] = {
    {ir::Opcode::IntAdd, &emit_add},
    {ir::Opcode::IntSub, &emit_sub},
    {ir::Opcode::IntMul, &emit_mul},
    {ir::Opcode::IntDiv, &emit_sdiv},
    {ir::Opcode::UintDiv, &emit_udiv},
    {ir::Opcode::IntRem, &emit_srem},
    {ir::Opcode::UintRem, &emit_urem},
    {ir::Opcode::FAdd, &emit_fadd},
    {ir::Opcode::FSub, &emit_fsub},
    {ir::Opcode::FMul, &emit_fmul},
    {ir::Opcode::FDiv, &emit_fdiv},
    {ir::Opcode::And, &emit_and},
    {ir::Opcode::Or, &emit_or},
    {ir::Opcode::Xor, &emit_xor},
    {ir::Opcode::ShiftLeft, &emit_shl},
    {ir::Opcode::ArithmeticShiftRight, &emit_ashr},
    {ir::Opcode::LogicalShiftRight, &emit_lshr},
};

}  // namespace

void LlvmIrEmitter::emit_compute(const ir::Instruction& instr) {
  check_state();

  const ir::Instruction& i = instr;
  const ir::OperandIdxRange& ops = i.operands;

  using Op = ir::Opcode;

  for (const BinaryOp& entry : BINARY_OPS) {
    if (entry.op != i.op) {
      continue;
    }
    DCHECK(ops.size() == 2);
    const ir::Operand& lhs = storage_->operands()[ops.head()];
    const ir::Operand& rhs = storage_->operands()[ops.head() + 1];
    if (i.dst.is_valid()) {
      values_.add_register(i.dst,
                           entry.emit(*builder_, resolve_operand_value(lhs),
                                      resolve_operand_value(rhs)));
    }
    return;
  }

  switch (i.op) {
    case Op::Not: {
      DCHECK(ops.size() == 1);
      const ir::Operand& lhs = storage_->operands()[ops.head()];
      if (i.dst.is_valid()) {
        values_.add_register(i.dst,
                             builder_->CreateNot(resolve_operand_value(lhs)));
      }
      break;
    }
    case Op::BitReverse: {
      DCHECK(ops.size() == 1);
      const ir::Operand& lhs = storage_->operands()[ops.head()];
      llvm::Value* value = resolve_operand_value(lhs);
      if (i.dst.is_valid()) {
        values_.add_register(
            i.dst, builder_->CreateIntrinsic(llvm::Intrinsic::bitreverse,
                                             value->getType(), value));
      }
      break;
    }
    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge: {
      DCHECK(ops.size() == 2);
      const ir::Operand& lhs = storage_->operands()[ops.head()];
      const ir::Operand& rhs = storage_->operands()[ops.head() + 1];
      const ir::TypeTag tag = storage_->types()[lhs.type.idx].tag;
      llvm::Value* lhs_val = resolve_operand_value(lhs);
      llvm::Value* rhs_val = resolve_operand_value(rhs);
      llvm::Value* result = nullptr;
      if (ir::is_integer_type(tag)) {
        result = builder_->CreateICmp(
            int_predicate(i.op, ir::is_signed_integer_type(tag)), lhs_val,
            rhs_val);
      } else if (ir::is_float_type(tag)) {
        result = builder_->CreateFCmp(float_predicate(i.op), lhs_val, rhs_val);
      } else if ((tag == ir::TypeTag::Ptr || tag == ir::TypeTag::Ref ||
                  tag == ir::TypeTag::MutRef) &&
                 (i.op == Op::Eq || i.op == Op::Ne)) {
        // References and raw pointers compare by address.
        result =
            builder_->CreateICmp(int_predicate(i.op, false), lhs_val, rhs_val);
      } else {
        DLOG("Unsupported comparison type: {}", tag);
        DCHECK(false);
        UNREACHABLE();
      }
      if (i.dst.is_valid()) {
        values_.add_register(i.dst, result);
      }
      break;
    }
    case Op::TypeCast: {
      DCHECK(ops.size() == 1);
      if (!i.dst.is_valid()) {
        break;
      }
      const ir::Operand& src = storage_->operands()[ops.head()];
      const ir::TypeTag src_tag = storage_->types()[src.type.idx].tag;
      const ir::Register& dst_reg = storage_->registers()[i.dst];
      const ir::TypeTag dst_tag = storage_->types()[dst_reg.type.idx].tag;
      llvm::Value* value = resolve_operand_value(src);
      llvm::Type* dst_ty = type(dst_reg.type);
      llvm::Value* result = nullptr;
      if (src_tag == dst_tag) {
        result = value;
      } else if (ir::is_integer_type(src_tag) && ir::is_integer_type(dst_tag)) {
        const u32 src_bits = value->getType()->getIntegerBitWidth();
        const u32 dst_bits = dst_ty->getIntegerBitWidth();
        if (dst_bits < src_bits) {
          result = builder_->CreateTrunc(value, dst_ty);
        } else if (ir::is_signed_integer_type(src_tag)) {
          result = builder_->CreateSExt(value, dst_ty);
        } else {
          result = builder_->CreateZExt(value, dst_ty);
        }
      } else if (ir::is_integer_type(src_tag) && ir::is_float_type(dst_tag)) {
        if (ir::is_signed_integer_type(src_tag)) {
          result = builder_->CreateSIToFP(value, dst_ty);
        } else {
          result = builder_->CreateUIToFP(value, dst_ty);
        }
      } else if (ir::is_float_type(src_tag) && ir::is_integer_type(dst_tag)) {
        if (ir::is_signed_integer_type(dst_tag)) {
          result = builder_->CreateFPToSI(value, dst_ty);
        } else {
          result = builder_->CreateFPToUI(value, dst_ty);
        }
      } else if (ir::is_float_type(src_tag) && ir::is_float_type(dst_tag)) {
        result = builder_->CreateFPCast(value, dst_ty);
      } else if (ir::is_integer_type(src_tag) && dst_tag == ir::TypeTag::Ptr) {
        result = builder_->CreateIntToPtr(value, dst_ty);
      } else if ((src_tag == ir::TypeTag::Ptr || src_tag == ir::TypeTag::Ref ||
                  src_tag == ir::TypeTag::MutRef) &&
                 ir::is_integer_type(dst_tag)) {
        // References ride as pointers; casts read the address.
        result = builder_->CreatePtrToInt(value, dst_ty);
      } else if (value->getType()->isPointerTy() &&
                 (dst_tag == ir::TypeTag::Struct ||
                  dst_tag == ir::TypeTag::Array ||
                  dst_tag == ir::TypeTag::Tuple ||
                  dst_tag == ir::TypeTag::Enum)) {
        // Pointer reinterpretation (type-erased payloads): the value
        // stays identical, but the destination labels the pointee
        // type so later element access resolves it.
        result = value;
        values_.add_alloca_type(i.dst, dst_ty);
      } else if (value->getType()->isPointerTy() &&
                 (dst_tag == ir::TypeTag::Ref ||
                  dst_tag == ir::TypeTag::MutRef)) {
        // Relabelling a raw pointer as a reference: the address is
        // unchanged, and the destination carries the pointee type.
        result = value;
      } else {
        DLOG("Unsupported cast");
        DCHECK(false);
        UNREACHABLE();
      }
      values_.add_register(i.dst, result);
      break;
    }
    case Op::TypeSizeOf:
    case Op::TypeAlignOf: {
      DCHECK(i.measure.is_valid());
      llvm::Type* measured = type(i.measure);
      const llvm::DataLayout& layout = module_->getDataLayout();
      const u64 amount = i.op == Op::TypeSizeOf
                             ? layout.getTypeAllocSize(measured).getFixedValue()
                             : layout.getABITypeAlign(measured).value();
      llvm::Type* dst_ty = type(storage_->registers()[i.dst].type);
      values_.add_register(
          i.dst, llvm::ConstantInt::get(llvm::cast<llvm::IntegerType>(dst_ty),
                                        amount));
      break;
    }
    case Op::Select: {
      DCHECK(ops.size() == 3);
      llvm::Value* cond =
          resolve_operand_value(storage_->operands()[ops.head()]);
      llvm::Value* true_val =
          resolve_operand_value(storage_->operands()[ops.head() + 1]);
      llvm::Value* false_val =
          resolve_operand_value(storage_->operands()[ops.head() + 2]);
      if (i.dst.is_valid()) {
        values_.add_register(i.dst,
                             builder_->CreateSelect(cond, true_val, false_val));
      }
      break;
    }
    case Op::Move: {
      // Register alias; no code emitted.
      DCHECK(ops.size() == 1);
      if (i.dst.is_valid()) {
        values_.add_register(
            i.dst, resolve_operand_value(storage_->operands()[ops.head()]));
      }
      break;
    }
    case Op::Borrow: {
      // The address operand already is the reference value.
      DCHECK(ops.size() == 1);
      if (i.dst.is_valid()) {
        values_.add_register(
            i.dst, resolve_operand_value(storage_->operands()[ops.head()]));
      }
      break;
    }
    case Op::Drop: {
      // Ownership marker only; no code emitted.
      DCHECK(ops.size() <= 1);
      break;
    }
    default: {
      DLOG("Unknown compute opcode found: {}", i.op);
      UNREACHABLE();
    }
  }
}

}  // namespace codegen_llvm
