// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <limits>
#include <optional>

#include "codegen/backend.h"
#include "codegen/wasm/emit.h"
#include "codegen/wasm/writer.h"
#include "debug/dcheck.h"
#include "diag/span.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "ir/common.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/type_util.h"
#include "overlay/overlay.h"

namespace codegen::wasm {
namespace {

// Binary arithmetic and bitwise opcodes, one row per IR opcode with the
// wasm opcode for each machine type. Signedness is the IR opcode's to
// pick: `IntDiv` is the signed row, `UintDiv` the unsigned one.
struct BinaryOp {
  ir::Opcode op;
  u8 i32;
  u8 i64;
  u8 f32;
  u8 f64;
};

constexpr BinaryOp BINARY_OPS[] = {
    {ir::Opcode::IntAdd, 0x6A, 0x7C, 0x92, 0xA0},
    {ir::Opcode::IntSub, 0x6B, 0x7D, 0x93, 0xA1},
    {ir::Opcode::IntMul, 0x6C, 0x7E, 0x94, 0xA2},
    {ir::Opcode::IntAddWrap, 0x6A, 0x7C, 0x92, 0xA0},
    {ir::Opcode::IntSubWrap, 0x6B, 0x7D, 0x93, 0xA1},
    {ir::Opcode::IntMulWrap, 0x6C, 0x7E, 0x94, 0xA2},
    {ir::Opcode::IntDiv, 0x6D, 0x7F, 0x95, 0xA3},
    {ir::Opcode::UintDiv, 0x6E, 0x80, 0x95, 0xA3},
    {ir::Opcode::IntRem, 0x6F, 0x81, 0, 0},
    {ir::Opcode::UintRem, 0x70, 0x82, 0, 0},
    {ir::Opcode::FAdd, 0, 0, 0x92, 0xA0},
    {ir::Opcode::FSub, 0, 0, 0x93, 0xA1},
    {ir::Opcode::FMul, 0, 0, 0x94, 0xA2},
    {ir::Opcode::FDiv, 0, 0, 0x95, 0xA3},
    {ir::Opcode::And, 0x71, 0x83, 0, 0},
    {ir::Opcode::Or, 0x72, 0x84, 0, 0},
    {ir::Opcode::Xor, 0x73, 0x85, 0, 0},
    {ir::Opcode::ShiftLeft, 0x74, 0x86, 0, 0},
    {ir::Opcode::ArithmeticShiftRight, 0x75, 0x87, 0, 0},
    {ir::Opcode::LogicalShiftRight, 0x76, 0x88, 0, 0},
};

bool is_wide_int(ir::TypeTag tag) {
  return tag == ir::TypeTag::I64 || tag == ir::TypeTag::U64;
}

// Whether a tag rides a 32-bit word, pointers included: casts between
// them and integers are the identity the LLVM emitter also treats them as.
bool is_word_int(ir::TypeTag tag) {
  return ir::is_integer_type(tag) && !is_wide_int(tag);
}

u8 compare_opcode(ir::Opcode op, bool is_signed, bool wide) {
  using O = ir::Opcode;
  switch (op) {
    case O::Eq: return wide ? 0x51 : 0x46;
    case O::Ne: return wide ? 0x52 : 0x47;
    case O::Lt:
      return wide ? (is_signed ? 0x53 : 0x54) : (is_signed ? 0x48 : 0x49);
    case O::Gt:
      return wide ? (is_signed ? 0x55 : 0x56) : (is_signed ? 0x4A : 0x4B);
    case O::Le:
      return wide ? (is_signed ? 0x57 : 0x58) : (is_signed ? 0x4C : 0x4D);
    case O::Ge:
      return wide ? (is_signed ? 0x59 : 0x5A) : (is_signed ? 0x4E : 0x4F);
    default: return 0;
  }
}

u8 float_compare_opcode(ir::Opcode op, bool wide) {
  using O = ir::Opcode;
  switch (op) {
    case O::Eq: return wide ? 0x61 : 0x5B;
    case O::Ne: return wide ? 0x62 : 0x5C;
    case O::Lt: return wide ? 0x63 : 0x5D;
    case O::Gt: return wide ? 0x64 : 0x5E;
    case O::Le: return wide ? 0x65 : 0x5F;
    case O::Ge: return wide ? 0x66 : 0x60;
    default: return 0;
  }
}

// Canonicalizes a narrowed integer in a 32-bit container: signed types
// sign-extend so later signed compares read the same value LLVM would,
// unsigned types zero-extend.
void canonicalize(BinaryWriter& out, ir::TypeTag tag) {
  switch (tag) {
    case ir::TypeTag::I1:
      out.byte(0x41);
      out.i32_leb(1);
      out.byte(0x71);  // i32.and
      break;
    case ir::TypeTag::I8: out.byte(0xC0); break;  // i32.extend8_s
    case ir::TypeTag::U8:
      out.byte(0x41);
      out.i32_leb(0xFF);
      out.byte(0x71);
      break;
    case ir::TypeTag::I16: out.byte(0xC1); break;  // i32.extend16_s
    case ir::TypeTag::U16:
      out.byte(0x41);
      out.i32_leb(0xFFFF);
      out.byte(0x71);
      break;
    default: break;
  }
}

}  // namespace

Emitter::EmitResult Emitter::emit_compute(ir::InstructionIdx instr_idx,
                                          const ir::Instruction& instr) {
  using O = ir::Opcode;
  const ir::OperandIdxRange ops = instr.operands;
  const auto store_dst = [this, &instr]() -> EmitResult {
    if (instr.dst.is_valid() && reg_shape_[instr.dst.idx].words > 0) {
      pop_words(reg_base_[instr.dst.idx], reg_shape_[instr.dst.idx].words);
    }
    return base::make_ok();
  };

  // The build's checks (ADR-0068): an overflow entry takes the checked
  // form of the operation, and the other kinds run their condition and
  // the runtime call before the operation does.
  if (const overlay::Entry* entry = overlay_entry(instr_idx);
      entry != nullptr) {
    if (entry->kind == overlay::Check::Overflow) {
      return emit_checked_overflow(instr, *entry);
    }
    if (emit_check_prelude(instr, *entry).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
  }

  for (const BinaryOp& entry : BINARY_OPS) {
    if (entry.op != instr.op) {
      continue;
    }
    DCHECK_EQ(ops.size(), 2u);
    const ir::Operand& lhs = storage_.operands()[ops.head()];
    const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
    const ir::TypeTag tag = storage_.types()[lhs.type].tag;
    u8 opcode = 0;
    if (ir::is_integer_type(tag)) {
      opcode = is_wide_int(tag) ? entry.i64 : entry.i32;
    } else if (ir::is_float_type(tag)) {
      opcode = tag == ir::TypeTag::F32 ? entry.f32 : entry.f64;
    }
    if (opcode == 0) {
      return unsupported(current_span_, ir::opcode_to_str(instr.op));
    }
    if (push_operand(lhs).is_err() || push_operand(rhs).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
    op(opcode);
    return store_dst();
  }

  switch (instr.op) {
    case O::Not: {
      DCHECK_EQ(ops.size(), 1u);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::TypeTag tag = storage_.types()[lhs.type].tag;
      if (!ir::is_integer_type(tag)) {
        return unsupported(current_span_, "this complement");
      }
      if (push_operand(lhs).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      if (tag == ir::TypeTag::I1) {
        // A bool is one canonical 0/1 word. A complement would turn
        // `true` into 0xFFFFFFFF, still true to every branch; `i32.eqz`
        // is the logical not and stays canonical.
        op(0x45);  // i32.eqz
      } else if (is_wide_int(tag)) {
        i64_const(-1);
        op(0x85);  // i64.xor
      } else {
        i32_const(-1);
        op(0x73);  // i32.xor
      }
      return store_dst();
    }
    case O::BitReverse: return unsupported(current_span_, "a bit reversal");
    case O::Eq:
    case O::Ne:
    case O::Lt:
    case O::Le:
    case O::Gt:
    case O::Ge: {
      DCHECK_EQ(ops.size(), 2u);
      const ir::Operand& lhs = storage_.operands()[ops.head()];
      const ir::Operand& rhs = storage_.operands()[ops.head() + 1];
      const ir::TypeTag tag = storage_.types()[lhs.type].tag;
      u8 opcode = 0;
      if (ir::is_integer_type(tag)) {
        opcode = compare_opcode(instr.op, ir::is_signed_integer_type(tag),
                                is_wide_int(tag));
      } else if (ir::is_float_type(tag)) {
        opcode = float_compare_opcode(instr.op, tag == ir::TypeTag::F64);
      } else if (is_thin_pointer(tag)) {
        if (instr.op != O::Eq && instr.op != O::Ne) {
          return unsupported(current_span_, "this pointer comparison");
        }
        opcode = instr.op == O::Eq ? 0x46 : 0x47;
      } else {
        return unsupported(current_span_,
                           ir::type_to_str(storage_.types()[lhs.type].tag));
      }
      if (push_operand(lhs).is_err() || push_operand(rhs).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      op(opcode);
      return store_dst();
    }
    case O::TypeCast: {
      DCHECK_EQ(ops.size(), 1u);
      if (!instr.dst.is_valid()) {
        return base::make_ok();
      }
      const ir::Operand& src = storage_.operands()[ops.head()];
      const ir::TypeTag src_tag = storage_.types()[src.type].tag;
      const ir::TypeTag dst_tag =
          storage_.types()[storage_.registers()[instr.dst].type].tag;
      if (src_tag == dst_tag) {
        if (copy_operand(src, reg_base_[instr.dst.idx],
                         reg_shape_[instr.dst.idx])
                .is_err()) {
          return base::make_err(codegen::EmitError::Unsupported);
        }
        return base::make_ok();
      }
      if (push_operand(src).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      if (is_word_int(src_tag) && is_word_int(dst_tag)) {
        canonicalize(body_, dst_tag);
      } else if (is_word_int(src_tag) && is_wide_int(dst_tag)) {
        op(ir::is_signed_integer_type(src_tag) ? 0xAC : 0xAD);  // extend_i32
      } else if (is_wide_int(src_tag) && is_wide_int(dst_tag)) {
        // Same width; nothing to encode.
      } else if (is_wide_int(src_tag) && is_word_int(dst_tag)) {
        op(0xA7);  // i32.wrap_i64
        canonicalize(body_, dst_tag);
      } else if (ir::is_integer_type(src_tag) && ir::is_float_type(dst_tag)) {
        const bool wide = is_wide_int(src_tag);
        if (dst_tag == ir::TypeTag::F32) {
          op(ir::is_signed_integer_type(src_tag) ? (wide ? 0xB4 : 0xB2)
                                                 : (wide ? 0xB5 : 0xB3));
        } else {
          op(ir::is_signed_integer_type(src_tag) ? (wide ? 0xB9 : 0xB7)
                                                 : (wide ? 0xBA : 0xB8));
        }
      } else if (ir::is_float_type(src_tag) && ir::is_integer_type(dst_tag)) {
        const bool wide_src = src_tag == ir::TypeTag::F64;
        const bool wide_dst = is_wide_int(dst_tag);
        if (wide_dst) {
          op(wide_src ? (ir::is_signed_integer_type(dst_tag) ? 0xB0 : 0xB1)
                      : (ir::is_signed_integer_type(dst_tag) ? 0xAE : 0xAF));
        } else {
          op(wide_src ? (ir::is_signed_integer_type(dst_tag) ? 0xAA : 0xAB)
                      : (ir::is_signed_integer_type(dst_tag) ? 0xA8 : 0xA9));
        }
        canonicalize(body_, dst_tag);
      } else if (ir::is_float_type(src_tag) && ir::is_float_type(dst_tag)) {
        if (src_tag == dst_tag) {
          // Nothing to encode.
        } else if (src_tag == ir::TypeTag::F64) {
          op(0xB6);  // f32.demote_f64
        } else {
          op(0xBB);  // f64.promote_f32
        }
      } else if (is_word_int(src_tag) && is_thin_pointer(dst_tag)) {
        canonicalize(body_, dst_tag);
      } else if (is_thin_pointer(src_tag) && is_thin_pointer(dst_tag)) {
        // Relabelling: the address is unchanged, and the destination
        // names what it points at.
      } else if (is_thin_pointer(src_tag) && ir::is_integer_type(dst_tag)) {
        if (is_wide_int(dst_tag)) {
          op(0xAD);  // i64.extend_i32_u: the address zero-extends.
        }
        canonicalize(body_, dst_tag);
      } else if (is_wide_int(src_tag) && is_thin_pointer(dst_tag)) {
        op(0xA7);  // i32.wrap_i64
      } else {
        return unsupported(current_span_,
                           ir::type_to_str(storage_.types()[src.type].tag));
      }
      return store_dst();
    }
    case O::TypeSizeOf:
    case O::TypeAlignOf: {
      DCHECK(instr.measure.is_valid());
      if (!instr.dst.is_valid()) {
        return base::make_ok();
      }
      const ir::TypeLayout layout =
          storage_.layout_of(instr.measure, target_.width);
      const u64 amount = instr.op == O::TypeSizeOf ? layout.size : layout.align;
      if (reg_shape_[instr.dst.idx].type == ValType::I64) {
        i64_const(static_cast<i64>(amount));
      } else {
        i32_const(static_cast<i32>(amount));
      }
      return store_dst();
    }
    case O::Select: {
      DCHECK_EQ(ops.size(), 3u);
      const ir::Operand& cond = storage_.operands()[ops.head()];
      const ir::Operand& on_true = storage_.operands()[ops.head() + 1];
      const ir::Operand& on_false = storage_.operands()[ops.head() + 2];
      if (instr.dst.is_valid() && reg_shape_[instr.dst.idx].words > 1) {
        return unsupported(current_span_, "a multi-word select");
      }
      if (push_operand(on_true).is_err() || push_operand(on_false).is_err() ||
          push_word(cond, 0, ValType::I32).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      op(0x1B);  // select
      return store_dst();
    }
    case O::Move:
    case O::Borrow: {
      DCHECK_EQ(ops.size(), 1u);
      if (!instr.dst.is_valid()) {
        return base::make_ok();
      }
      return copy_operand(storage_.operands()[ops.head()],
                          reg_base_[instr.dst.idx], reg_shape_[instr.dst.idx]);
    }
    case O::Drop: return base::make_ok();
    default: return unsupported(current_span_, ir::opcode_to_str(instr.op));
  }
}

namespace {

// The opcode `BINARY_OPS` holds for a tag's container; zero when the
// operation is not one of the checked ones.
u8 binary_opcode(ir::Opcode op, bool wide) {
  for (const BinaryOp& row : BINARY_OPS) {
    if (row.op == op) {
      return wide ? row.i64 : row.i32;
    }
  }
  return 0;
}

// The width a shift count is checked against.
u32 integer_bits(ir::TypeTag tag) {
  switch (tag) {
    case ir::TypeTag::I8:
    case ir::TypeTag::U8: return 8;
    case ir::TypeTag::I16:
    case ir::TypeTag::U16: return 16;
    case ir::TypeTag::I32:
    case ir::TypeTag::U32: return 32;
    default: return 64;
  }
}

// The signed minimum an i32 container holds for a narrowed tag.
i32 signed_minimum(ir::TypeTag tag) {
  switch (tag) {
    case ir::TypeTag::I8: return -128;
    case ir::TypeTag::I16: return -32768;
    default: return std::numeric_limits<i32>::min();
  }
}

}  // namespace

Emitter::EmitResult Emitter::emit_checked_overflow(
    const ir::Instruction& instr,
    const overlay::Entry& entry) {
  using O = ir::Opcode;
  DCHECK_EQ(instr.operands.size(), 2u);
  const ir::Operand& lhs = storage_.operands()[instr.operands.head()];
  const ir::Operand& rhs = storage_.operands()[instr.operands.head() + 1];
  const ir::TypeTag tag = storage_.types()[lhs.type].tag;
  const bool wide = is_wide_int(tag);
  const bool word32 =
      !wide && (tag == ir::TypeTag::I32 || tag == ir::TypeTag::U32);
  const bool narrow = !wide && !word32;
  const bool signed_op = ir::is_signed_integer_type(tag);
  const u8 opcode = binary_opcode(instr.op, wide);
  if (opcode == 0) {
    return unsupported(current_span_, ir::opcode_to_str(instr.op));
  }

  if (word32) {
    // A 32-bit operation widens into i64, where it cannot overflow, and
    // wraps back: the value fits exactly when wrapping changes nothing.
    const u8 extend = signed_op ? 0xAC : 0xAD;  // i64.extend_i32_s/_u
    if (push_operand(lhs).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
    op(extend);
    if (push_operand(rhs).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
    op(extend);
    op(binary_opcode(instr.op, true));
    local_set(scratch_wide_);
    local_get(scratch_wide_);
    op(0xA7);  // i32.wrap_i64
    op(extend);
    local_get(scratch_wide_);
    op(0x52);  // i64.ne
  } else if (narrow) {
    // A narrowed value fits its container exactly when canonicalizing
    // the result does not change it.
    if (push_operand(lhs).is_err() || push_operand(rhs).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
    op(opcode);
    local_set(scratch0_);
    local_get(scratch0_);
    canonicalize(body_, tag);
    local_get(scratch0_);
    op(0x47);  // i32.ne
  } else {
    if (push_operand(lhs).is_err() || push_operand(rhs).is_err()) {
      return base::make_err(codegen::EmitError::Unsupported);
    }
    op(opcode);
    local_set(scratch_wide_);
    switch (instr.op) {
      case O::IntAdd:
        if (signed_op) {
          // ((a ^ s) & (b ^ s)) < 0
          push_operand(lhs);
          local_get(scratch_wide_);
          op(0x85);  // i64.xor
          push_operand(rhs);
          local_get(scratch_wide_);
          op(0x85);
          op(0x83);  // i64.and
          i64_const(0);
          op(0x53);  // i64.lt_s
        } else {
          local_get(scratch_wide_);
          push_operand(lhs);
          op(0x54);  // i64.lt_u
        }
        break;
      case O::IntSub:
        if (signed_op) {
          // ((a ^ b) & (a ^ s)) < 0
          push_operand(lhs);
          push_operand(rhs);
          op(0x85);
          push_operand(lhs);
          local_get(scratch_wide_);
          op(0x85);
          op(0x83);
          i64_const(0);
          op(0x53);
        } else {
          push_operand(lhs);
          push_operand(rhs);
          op(0x54);  // i64.lt_u
        }
        break;
      default: {  // IntMul
        if (signed_op) {
          // special = (a == -1); the overflow is `b == MIN` there, and
          // `a != 0 && sdiv(s, a) != b` elsewhere with the divisor kept
          // off zero and minus one.
          push_operand(lhs);
          i64_const(-1);
          op(0x51);  // i64.eq
          local_set(scratch0_);
          local_get(scratch0_);
          push_operand(rhs);
          i64_const(std::numeric_limits<i64>::min());
          op(0x51);
          op(0x71);  // ovf1
          i64_const(1);
          push_operand(lhs);
          local_get(scratch0_);
          push_operand(lhs);
          op(0x50);  // i64.eqz
          op(0x72);  // i32.or
          op(0x1B);
          local_set(scratch_wide2_);  // safe
          local_get(scratch0_);
          op(0x45);  // !special
          push_operand(lhs);
          i64_const(0);
          op(0x52);  // i64.ne
          op(0x71);
          local_get(scratch_wide_);
          local_get(scratch_wide2_);
          op(0x7F);  // i64.div_s
          push_operand(rhs);
          op(0x52);
          op(0x71);
          op(0x72);  // i32.or
        } else {
          // a != 0, and the quotient by the divisor kept off zero.
          push_operand(lhs);
          i64_const(0);
          op(0x52);  // i64.ne
          i64_const(1);
          push_operand(lhs);
          push_operand(lhs);
          op(0x50);
          op(0x1B);
          local_set(scratch_wide2_);  // safe
          local_get(scratch_wide_);
          local_get(scratch_wide2_);
          op(0x80);  // i64.div_u
          push_operand(rhs);
          op(0x52);
          op(0x71);
        }
        break;
      }
    }
  }

  emit_check_call(CHECK_OVERFLOW, entry.site);
  if (word32) {
    local_get(scratch_wide_);
    op(0xA7);  // i32.wrap_i64
  } else {
    local_get(wide ? scratch_wide_ : scratch0_);
    if (narrow) {
      canonicalize(body_, tag);
    }
  }
  if (instr.dst.is_valid() && reg_shape_[instr.dst.idx].words > 0) {
    pop_words(reg_base_[instr.dst.idx], reg_shape_[instr.dst.idx].words);
  }
  return base::make_ok();
}

Emitter::EmitResult Emitter::emit_check_prelude(const ir::Instruction& instr,
                                                const overlay::Entry& entry) {
  const ir::Operand& lhs = storage_.operands()[instr.operands.head()];
  const ir::TypeTag tag = storage_.types()[lhs.type].tag;
  const bool wide = is_wide_int(tag);
  switch (entry.kind) {
    case overlay::Check::ShiftRange: {
      if (push_operand(storage_.operands()[entry.operand]).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      if (wide) {
        i64_const(64);
        op(0x5A);  // i64.ge_u
      } else {
        i32_const(static_cast<i32>(integer_bits(tag)));
        op(0x4F);  // i32.ge_u
      }
      emit_check_call(CHECK_SHIFT, entry.site);
      return base::make_ok();
    }
    case overlay::Check::MinOverOne: {
      // (the dividend is the minimum) & (the divisor is minus one)
      if (push_operand(lhs).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      if (wide) {
        i64_const(std::numeric_limits<i64>::min());
        op(0x51);  // i64.eq
      } else {
        i32_const(signed_minimum(tag));
        op(0x46);  // i32.eq
      }
      if (push_operand(storage_.operands()[entry.operand]).is_err()) {
        return base::make_err(codegen::EmitError::Unsupported);
      }
      if (wide) {
        i64_const(-1);
        op(0x51);
      } else {
        i32_const(-1);
        op(0x46);
      }
      op(0x71);
      emit_check_call(CHECK_OVERFLOW, entry.site);
      return base::make_ok();
    }
    default: return base::make_ok();
  }
}

}  // namespace codegen::wasm
