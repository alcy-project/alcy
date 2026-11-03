// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "overlay/overlay.h"

#include <optional>
#include <span>
#include <vector>

#include "comp/comp_value.h"
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

// The check an instruction carries, without its site: the kind, and the
// operand a condition reads when the instruction's own operands are not
// the values the source wrote.
Entry checked_entry(const ir::Storage& storage, const ir::Instruction& instr) {
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
  return entry;
}

// The signed minimum of a width, in the bit pattern an immediate carries.
u64 signed_minimum(ir::TypeTag tag) {
  return static_cast<u64>(1) << (comp::int_bytes(tag) * 8 - 1);
}

// Whether a checked build can abort on this operation. A check whose
// values the IR knows can be ruled out here; a value it does not know
// keeps the abort possible, because the profile is the build's and not
// the passes' to read (ADR-0067).
bool aborts(const ir::Storage& storage,
            const ir::Instruction& instr,
            const Entry& entry) {
  const ir::TypeTag tag = entry.kind == Check::None
                              ? ir::TypeTag::I1
                              : first_operand_tag(storage, instr);
  switch (entry.kind) {
    case Check::None: return false;
    case Check::Overflow: {
      const std::optional<u64> left =
          integer_constant(storage, instr.operands.head());
      const std::optional<u64> right =
          integer_constant(storage, instr.operands.head() + 1);
      if (!left.has_value() || !right.has_value()) {
        return true;
      }
      return overflows(instr.op, tag, *left, *right);
    }
    case Check::ShiftRange: {
      const std::optional<u64> count = integer_constant(storage, entry.operand);
      return !count.has_value() || *count >= integer_bits(tag);
    }
    case Check::MinOverOne: {
      const std::optional<u64> divisor =
          integer_constant(storage, entry.operand);
      const std::optional<u64> dividend =
          integer_constant(storage, instr.operands.head());
      if (!divisor.has_value() || !dividend.has_value()) {
        return true;
      }
      return comp::sign_extend(*divisor, tag) == -1 &&
             comp::sign_extend(*dividend, tag) ==
                 comp::sign_extend(signed_minimum(tag), tag);
    }
  }
  return false;
}

}  // namespace

bool overflows(ir::Opcode op, ir::TypeTag tag, u64 left, u64 right) {
  switch (op) {
    case Opcode::IntAdd: return comp::add_overflows(left, right, tag);
    case Opcode::IntSub: return comp::sub_overflows(left, right, tag);
    case Opcode::IntMul: return comp::mul_overflows(left, right, tag);
    default: return false;
  }
}

Checks checked_instructions(const ir::Storage& storage) {
  const auto& instrs = storage.instrs();
  Checks checks{.kinds = std::vector<Check>(instrs.size(), Check::None),
                .raw = std::vector<bool>(instrs.size(), false)};
  for (usize at = 0; at < instrs.size(); ++at) {
    const ir::Instruction& instr =
        instrs[ir::InstructionIdx{static_cast<u32>(at)}];
    const Entry entry = checked_entry(storage, instr);
    if (!aborts(storage, instr, entry)) {
      continue;
    }
    checks.kinds[at] = entry.kind;
    // The check reads the instruction's own second operand unless the
    // entry names another: the definition lowering left behind is what
    // makes the check about the value the source wrote.
    if (entry.kind == Check::Overflow ||
        entry.operand == instr.operands.head() + 1) {
      continue;
    }
    const ir::Operand& shaped = storage.operands()[instr.operands.head() + 1];
    if (!shaped.is<ir::RegisterIdx>()) {
      continue;
    }
    const ir::InstructionIdx def =
        storage.registers()[shaped.as_register()].def_idx;
    if (def.is_valid()) {
      checks.raw[def.idx] = true;
    }
  }
  return checks;
}

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
    Entry entry = checked_entry(storage, instr);
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
