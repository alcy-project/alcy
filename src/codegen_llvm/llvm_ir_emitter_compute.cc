// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cfg/build_config.h"
#include "codegen_llvm/common.h"
#include "codegen_llvm/llvm_ir_emitter.h"
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

// Integer comparison helpers keyed by operand type tag.
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

}  // namespace

void LlvmIrEmitter::emit_compute(const ir::Instruction& instr) {
  check_state();

  const ir::Instruction& i = instr;
  const ir::OperandIdxRange& ops = i.operands;

  using Op = ir::Opcode;

  auto binary = [&](auto emit) {
    DCHECK(ops.size() == 2);
    const ir::Operand& lhs = storage_.operands()[ops.head()];
    const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
    if (i.dst.is_valid()) {
      values_.add_register(
          i.dst, emit(resolve_operand_value(lhs), resolve_operand_value(rhs)));
    }
  };

  switch (i.op) {
    case Op::IntAdd: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateAdd(lhs, rhs);
      });
      break;
    }
    case Op::IntSub: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateSub(lhs, rhs);
      });
      break;
    }
    case Op::IntMul: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateMul(lhs, rhs);
      });
      break;
    }
    case Op::IntDiv: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateSDiv(lhs, rhs);
      });
      break;
    }
    case Op::UintDiv: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateUDiv(lhs, rhs);
      });
      break;
    }
    case Op::IntRem: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateSRem(lhs, rhs);
      });
      break;
    }
    case Op::UintRem: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateURem(lhs, rhs);
      });
      break;
    }
    case Op::FAdd: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateFAdd(lhs, rhs);
      });
      break;
    }
    case Op::FSub: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateFSub(lhs, rhs);
      });
      break;
    }
    case Op::FMul: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateFMul(lhs, rhs);
      });
      break;
    }
    case Op::FDiv: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateFDiv(lhs, rhs);
      });
      break;
    }
    case Op::And: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateAnd(lhs, rhs);
      });
      break;
    }
    case Op::Or: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateOr(lhs, rhs);
      });
      break;
    }
    case Op::Xor: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateXor(lhs, rhs);
      });
      break;
    }
    case Op::ShiftLeft: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateShl(lhs, rhs);
      });
      break;
    }
    case Op::ArithmeticShiftRight: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateAShr(lhs, rhs);
      });
      break;
    }
    case Op::LogicalShiftRight: {
      binary([this](llvm::Value* lhs, llvm::Value* rhs) {
        return builder_->CreateLShr(lhs, rhs);
      });
      break;
    }
    case Op::Not: {
      DCHECK(ops.size() == 1);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      if (i.dst.is_valid()) {
        values_.add_register(i.dst,
                             builder_->CreateNot(resolve_operand_value(lhs)));
      }
      break;
    }
    case Op::BitReverse: {
      DCHECK(ops.size() == 1);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
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
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      const ir::TypeTag tag = storage_.types()[lhs.type.idx].tag;
      llvm::Value* lhs_val = resolve_operand_value(lhs);
      llvm::Value* rhs_val = resolve_operand_value(rhs);
      llvm::Value* result = nullptr;
      if (ir::is_integer_type(tag)) {
        result = builder_->CreateICmp(
            int_predicate(i.op, ir::is_signed_integer_type(tag)), lhs_val,
            rhs_val);
      } else if (ir::is_float_type(tag)) {
        result = builder_->CreateFCmp(float_predicate(i.op), lhs_val, rhs_val);
      } else if (tag == ir::TypeTag::Ptr &&
                 (i.op == Op::Eq || i.op == Op::Ne)) {
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
      const ir::Operand& src = storage_.operands()[ops.head()];
      const ir::TypeTag src_tag = storage_.types()[src.type.idx].tag;
      const ir::Register& dst_reg = storage_.registers()[i.dst];
      const ir::TypeTag dst_tag = storage_.types()[dst_reg.type.idx].tag;
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
      } else if (src_tag == ir::TypeTag::Ptr && ir::is_integer_type(dst_tag)) {
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
      } else {
        DLOG("Unsupported cast");
        DCHECK(false);
        UNREACHABLE();
      }
      values_.add_register(i.dst, result);
      break;
    }
    case Op::Select: {
      DCHECK(ops.size() == 3);
      llvm::Value* cond =
          resolve_operand_value(storage_.operands()[ops.head()]);
      llvm::Value* true_val =
          resolve_operand_value(storage_.operands()[ops.head() + 1]);
      llvm::Value* false_val =
          resolve_operand_value(storage_.operands()[ops.head() + 2]);
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
            i.dst, resolve_operand_value(storage_.operands()[ops.head()]));
      }
      break;
    }
    case Op::Borrow: {
      // The address operand already is the reference value.
      DCHECK(ops.size() == 1);
      if (i.dst.is_valid()) {
        values_.add_register(
            i.dst, resolve_operand_value(storage_.operands()[ops.head()]));
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
