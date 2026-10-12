// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "overlay/overlay.h"

#include <optional>
#include <span>

#include "debug/dcheck.h"
#include "ir/common.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/register.h"
#include "ir/storage.h"
#include "ir/type.h"
#include "ir/type_util.h"

namespace overlay {

namespace {

using ir::Instruction;
using ir::Opcode;

// The width a shift count is checked against.
u64 integer_bits(ir::TypeTag tag) {
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

// The type an instruction's first operand carries; operations without
// operands never reach the checks.
ir::TypeTag first_operand_tag(const ir::Storage& storage,
                              const ir::Instruction& instr) {
  DCHECK_GT(instr.operands.size(), 0u);
  return storage.types()[storage.operands()[instr.operands.head()].type].tag;
}

// The value an integer immediate holds, or nothing when the operand is
// not one.
std::optional<u64> integer_constant(const ir::Storage& storage,
                                    ir::OperandIdx operand) {
  const ir::Operand& op = storage.operands()[operand];
  if (!op.is<ir::ImmutableIdx>()) {
    return std::nullopt;
  }
  const ir::Immutable& imm = storage.immutables()[op.as_immutable()];
  const ir::TypeTag tag = storage.types()[op.type].tag;
  if (!ir::is_integer_type(tag)) {
    return std::nullopt;
  }
  return imm.as_u64_integer(tag);
}

// The operand holding the count the source wrote for a shift. Lowering
// masks the count before the shift (ADR-0066), so the mask's own first
// operand is the count the overshift check is about; a shift without
// the mask shape checks its operand.
ir::OperandIdx raw_shift_count(const ir::Storage& storage,
                               const ir::Instruction& shift) {
  DCHECK_EQ(shift.operands.size(), 2u);
  const ir::OperandIdx amount = shift.operands.head() + 1;
  const ir::Operand& op = storage.operands()[amount];
  if (!op.is<ir::RegisterIdx>()) {
    return amount;
  }
  const ir::InstructionIdx def = storage.registers()[op.as_register()].def_idx;
  if (!def.is_valid()) {
    return amount;
  }
  const ir::Instruction& mask = storage.instrs()[def];
  if (mask.op != Opcode::And || mask.operands.size() != 2) {
    return amount;
  }
  const ir::OperandIdx value = mask.operands.head();
  const std::optional<u64> width_mask = integer_constant(storage, value + 1);
  if (!width_mask.has_value() ||
      *width_mask != integer_bits(first_operand_tag(storage, shift)) - 1) {
    return amount;
  }
  return value;
}

// The operand holding the divisor the source wrote for a signed
// division. Lowering replaces a `-1` divisor with one (ADR-0066), so the
// replace select's false arm is the divisor the check is about; a
// division without that shape checks its operand.
ir::OperandIdx raw_divisor(const ir::Storage& storage,
                           const ir::Instruction& division) {
  DCHECK_EQ(division.operands.size(), 2u);
  const ir::OperandIdx divisor = division.operands.head() + 1;
  const ir::Operand& op = storage.operands()[divisor];
  if (!op.is<ir::RegisterIdx>()) {
    return divisor;
  }
  const ir::InstructionIdx def = storage.registers()[op.as_register()].def_idx;
  if (!def.is_valid()) {
    return divisor;
  }
  const ir::Instruction& safe = storage.instrs()[def];
  if (safe.op != Opcode::Select || safe.operands.size() != 3) {
    return divisor;
  }
  return safe.operands.head() + 2;
}

}  // namespace

Overlay build(const ir::Storage& storage,
              bool overflow_checks,
              std::span<const str::StringPoolId> sites) {
  Overlay overlay;
  const auto& instrs = storage.instrs();
  overlay.entries.resize(instrs.size());
  if (!overflow_checks) {
    return overlay;
  }
  for (usize at = 0; at < instrs.size(); ++at) {
    const ir::Instruction& instr =
        instrs[ir::InstructionIdx{static_cast<u32>(at)}];
    Entry entry;
    switch (instr.op) {
      case Opcode::IntAdd:
      case Opcode::IntSub:
      case Opcode::IntMul: entry.kind = Check::Overflow; break;
      case Opcode::ShiftLeft:
      case Opcode::ArithmeticShiftRight:
      case Opcode::LogicalShiftRight:
        entry.kind = Check::ShiftRange;
        entry.operand = raw_shift_count(storage, instr);
        break;
      case Opcode::IntDiv:
        if (ir::is_signed_integer_type(first_operand_tag(storage, instr))) {
          entry.kind = Check::MinOverOne;
          entry.operand = raw_divisor(storage, instr);
        }
        break;
      default: break;
    }
    if (entry.kind == Check::None) {
      continue;
    }
    if (at < sites.size()) {
      entry.site = sites[at];
    }
    overlay.entries[at] = entry;
  }
  return overlay;
}

}  // namespace overlay
