// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/opt.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include "fpag/base/numeric.h"
#include "ir/common.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/type_util.h"
#include "ir/verifier.h"

namespace opt {
namespace {

using ir::Opcode;
using ir::Operand;
using ir::StorageState;
using ir::TypeTag;

u32 bit_width(TypeTag tag) {
  switch (tag) {
    case TypeTag::I1: return 1;
    case TypeTag::I8:
    case TypeTag::U8: return 8;
    case TypeTag::I16:
    case TypeTag::U16: return 16;
    case TypeTag::I32:
    case TypeTag::U32: return 32;
    default: return 64;
  }
}

u64 type_mask(TypeTag tag) {
  const u32 bits = bit_width(tag);
  return bits >= 64 ? ~static_cast<u64>(0) : (static_cast<u64>(1) << bits) - 1;
}

i64 sign_extend(u64 value, TypeTag tag) {
  const u32 bits = bit_width(tag);
  if (bits >= 64) {
    return static_cast<i64>(value);
  }
  const u64 sign = static_cast<u64>(1) << (bits - 1);
  const u64 masked = value & ((static_cast<u64>(1) << bits) - 1);
  return static_cast<i64>((masked ^ sign) - sign);
}

ir::Immutable integer_immutable(ir::TypeIdx type, TypeTag tag, u64 bits) {
  ir::Immutable imm{.type = type, .data = {}};
  switch (tag) {
    case TypeTag::I1: imm.data.i1_value = bits != 0; break;
    case TypeTag::I8: imm.data.i8_value = static_cast<i8>(bits); break;
    case TypeTag::I16: imm.data.i16_value = static_cast<i16>(bits); break;
    case TypeTag::I32: imm.data.i32_value = static_cast<i32>(bits); break;
    case TypeTag::I64: imm.data.i64_value = static_cast<i64>(bits); break;
    case TypeTag::U8: imm.data.u8_value = static_cast<u8>(bits); break;
    case TypeTag::U16: imm.data.u16_value = static_cast<u16>(bits); break;
    case TypeTag::U32: imm.data.u32_value = static_cast<u32>(bits); break;
    default: imm.data.u64_value = bits; break;
  }
  return imm;
}

// The result of one instruction, as far as the fold walk knows it. A
// register's value is an operand because a folded select is its chosen
// operand, not a new immutable; an operand with an invalid type is the
// walk's "not known yet".
Operand unknown_operand() {
  return Operand::from_register(ir::RegisterIdx(base::INVALID_IDX),
                                ir::TypeIdx::invalid());
}

// The integer constant an operand names, directly or through a register
// the walk already resolved.
bool constant_of(const StorageState& state,
                 const std::vector<Operand>& known,
                 const Operand& op,
                 u64& bits,
                 TypeTag& tag) {
  const Operand* resolved = &op;
  Operand via_register = unknown_operand();
  if (op.is<ir::RegisterIdx>()) {
    const ir::RegisterIdx reg = op.as_register();
    if (reg.idx >= known.size() ||
        known[reg.idx].type.idx == base::INVALID_IDX) {
      return false;
    }
    via_register = known[reg.idx];
    resolved = &via_register;
  }
  if (!resolved->is<ir::ImmutableIdx>()) {
    return false;
  }
  tag = state.types[resolved->type.idx].tag;
  if (!ir::is_integer_type(tag)) {
    return false;
  }
  bits = state.immutables[resolved->as_immutable()].as_u64_integer(tag);
  return true;
}

bool fold_binary(Opcode op, TypeTag tag, u64 a, u64 b, u64& out) {
  const u64 mask = type_mask(tag);
  const bool signed_type = ir::is_signed_integer_type(tag);
  switch (op) {
    case Opcode::IntAdd: out = (a + b) & mask; return true;
    case Opcode::IntSub: out = (a - b) & mask; return true;
    case Opcode::IntMul: out = (a * b) & mask; return true;
    case Opcode::And: out = (a & b) & mask; return true;
    case Opcode::Or: out = (a | b) & mask; return true;
    case Opcode::Xor: out = (a ^ b) & mask; return true;
    case Opcode::ShiftLeft:
      out = (a << (b & (bit_width(tag) - 1))) & mask;
      return true;
    case Opcode::LogicalShiftRight:
      out = (a & mask) >> (b & (bit_width(tag) - 1));
      return true;
    case Opcode::ArithmeticShiftRight:
      out = signed_type ? static_cast<u64>(sign_extend(a, tag) >>
                                           (b & (bit_width(tag) - 1))) &
                              mask
                        : (a & mask) >> (b & (bit_width(tag) - 1));
      return true;
    case Opcode::IntDiv:
    case Opcode::IntRem: {
      if (b == 0) {
        // The language panics; folding would hide the panic site.
        return false;
      }
      const i64 sa = sign_extend(a, tag);
      const i64 sb = sign_extend(b, tag);
      // `MIN / -1` wraps and `MIN % -1` is zero, the language's rule.
      if (sb == -1 && sa == std::numeric_limits<i64>::min()) {
        out = op == Opcode::IntDiv ? static_cast<u64>(sa) : 0;
      } else {
        out = op == Opcode::IntDiv ? static_cast<u64>(sa / sb)
                                   : static_cast<u64>(sa % sb);
      }
      out &= mask;
      return true;
    }
    case Opcode::UintDiv:
      if (b == 0) {
        return false;
      }
      out = (a / b) & mask;
      return true;
    case Opcode::UintRem:
      if (b == 0) {
        return false;
      }
      out = (a % b) & mask;
      return true;
    case Opcode::Eq: out = a == b; return true;
    case Opcode::Ne: out = a != b; return true;
    case Opcode::Lt:
      out = signed_type ? sign_extend(a, tag) < sign_extend(b, tag) : a < b;
      return true;
    case Opcode::Le:
      out = signed_type ? sign_extend(a, tag) <= sign_extend(b, tag) : a <= b;
      return true;
    case Opcode::Gt:
      out = signed_type ? sign_extend(a, tag) > sign_extend(b, tag) : a > b;
      return true;
    case Opcode::Ge:
      out = signed_type ? sign_extend(a, tag) >= sign_extend(b, tag) : a >= b;
      return true;
    case Opcode::Not: out = (~a) & mask; return true;
    default: return false;
  }
}

void fold_and_propagate(StorageState& state) {
  std::vector<Operand> known(state.registers.size(), unknown_operand());
  const usize count = state.instrs.size();
  for (usize i = 0; i < count; ++i) {
    ir::Instruction& instr =
        state.instrs[ir::InstructionIdx(static_cast<u32>(i))];
    if (!instr.dst.is_valid()) {
      continue;
    }
    const ir::TypeIdx dst_type = state.registers[instr.dst].type;
    const TypeTag dst_tag = state.types[dst_type.idx].tag;
    if (!ir::is_integer_type(dst_tag)) {
      continue;
    }
    // A select under a known condition is the operand it chooses.
    if (instr.op == Opcode::Select && instr.operands.size() == 3) {
      u64 cond = 0;
      TypeTag cond_tag = TypeTag::I1;
      if (constant_of(state, known, state.operands[instr.operands.head()], cond,
                      cond_tag)) {
        const usize chosen = cond != 0 ? 1 : 2;
        known[instr.dst.idx] = state.operands[instr.operands.head() + chosen];
      }
      continue;
    }
    if (instr.operands.size() != 2) {
      continue;
    }
    u64 a = 0;
    u64 b = 0;
    TypeTag a_tag = TypeTag::I1;
    TypeTag b_tag = TypeTag::I1;
    if (!constant_of(state, known, state.operands[instr.operands.head()], a,
                     a_tag) ||
        !constant_of(state, known, state.operands[instr.operands.head() + 1], b,
                     b_tag)) {
      continue;
    }
    (void)b_tag;
    u64 out = 0;
    // The fold ranks the operands' type: a comparison's destination is a
    // bool, and folding `-1i8 < 0i8` at bool's width compared the raw
    // words unsigned, where the source type is what ranks them.
    if (!fold_binary(instr.op, a_tag, a, b, out)) {
      continue;
    }
    const ir::ImmutableIdx imm =
        state.immutables.push_back(integer_immutable(dst_type, dst_tag, out));
    known[instr.dst.idx] = Operand::from_immutable(imm, dst_type);
  }
  // Every use of a known register takes the known value. Registers are
  // single-definition, so one substitution is valid for the whole
  // function, not only past the defining instruction in its block.
  for (Operand& op : state.operands) {
    if (!op.is<ir::RegisterIdx>()) {
      continue;
    }
    const ir::RegisterIdx reg = op.as_register();
    if (reg.idx >= known.size() ||
        known[reg.idx].type.idx == base::INVALID_IDX) {
      continue;
    }
    Operand replacement = known[reg.idx];
    replacement.type = op.type;
    op = replacement;
  }
}

// Instructions whose result nothing reads and whose execution nothing
// observes. `Load` stays: dropping it would drop a fault a program
// relies on, and `Borrow`/`Move`/`Drop` are ownership markers the
// borrow pass placed.
bool prunable(Opcode op) {
  switch (op) {
    case Opcode::IntAdd:
    case Opcode::IntSub:
    case Opcode::IntMul:
    case Opcode::IntDiv:
    case Opcode::UintDiv:
    case Opcode::IntRem:
    case Opcode::UintRem:
    case Opcode::FAdd:
    case Opcode::FSub:
    case Opcode::FMul:
    case Opcode::FDiv:
    case Opcode::And:
    case Opcode::Or:
    case Opcode::Xor:
    case Opcode::ShiftLeft:
    case Opcode::ArithmeticShiftRight:
    case Opcode::LogicalShiftRight:
    case Opcode::Not:
    case Opcode::BitReverse:
    case Opcode::Eq:
    case Opcode::Ne:
    case Opcode::Le:
    case Opcode::Lt:
    case Opcode::Ge:
    case Opcode::Gt:
    case Opcode::TypeCast:
    case Opcode::TypeSizeOf:
    case Opcode::TypeAlignOf:
    case Opcode::Select:
    case Opcode::GetElementPtr:
    case Opcode::ElemOffset:
    case Opcode::ExtractValue:
    case Opcode::InsertValue: return true;
    default: return false;
  }
}

void eliminate_dead(StorageState& state) {
  std::vector<u32> uses(state.registers.size(), 0);
  bool changed = true;
  while (changed) {
    changed = false;
    std::fill(uses.begin(), uses.end(), 0);
    for (const Operand& op : state.operands) {
      if (op.is<ir::RegisterIdx>()) {
        const ir::RegisterIdx reg = op.as_register();
        if (reg.idx < uses.size()) {
          ++uses[reg.idx];
        }
      }
    }
    const usize count = state.instrs.size();
    for (usize i = 0; i < count; ++i) {
      ir::Instruction& instr =
          state.instrs[ir::InstructionIdx(static_cast<u32>(i))];
      if (!instr.dst.is_valid() || uses[instr.dst.idx] != 0) {
        continue;
      }
      if (!prunable(instr.op)) {
        continue;
      }
      instr.op = Opcode::Noop;
      instr.dst = ir::RegisterIdx(base::INVALID_IDX);
      instr.measure = ir::TypeIdx::invalid();
      instr.operands = {};
      changed = true;
    }
  }
}

}  // namespace

base::Result<ir::VerifiedStorage, ir::VerificationError> optimize(
    ir::VerifiedStorage storage) {
  ir::Storage plain = std::move(storage).unwrap();
  StorageState state = std::move(plain).take_state();
  fold_and_propagate(state);
  eliminate_dead(state);
  ir::StorageBuilder builder(std::move(state));
  return std::move(builder).build();
}

}  // namespace opt
