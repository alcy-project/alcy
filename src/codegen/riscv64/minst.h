// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "codegen/riscv64/isa.h"
#include "fpag/base/numeric.h"

namespace codegen::riscv64 {

struct VReg {
  u32 index = 0;
};

struct Imm {
  i64 value = 0;
};

using RegRef = std::variant<VReg, Reg>;

struct Mem {
  std::optional<RegRef> base;
  i32 disp = 0;
};

struct Label {
  u32 id = 0;
};

struct SymbolRef {
  u32 id = 0;
};

struct SlotRef {
  u32 vreg = 0;
  i32 offset = 0;
};

// The conversion an Fcvt instruction performs. Its numeric value is the
// tag carried in the instruction's third operand.
enum class FcvtMode : u8 {
  F64ToI64,
  F32ToI64,
  I64ToF64,
  I32ToF64,
  F64ToIU64,
  F32ToIU64,
  F64ToF32,
  F32ToF64,
  IU64ToF64,
  IU32ToF64,
  IU64ToF32,
  IU32ToF32,
  I64ToF32,
  I32ToF32,
};

using Operand = std::variant<std::monostate,
                             VReg,
                             Reg,
                             Imm,
                             Mem,
                             Label,
                             SymbolRef,
                             SlotRef,
                             FcvtMode>;

// Branch conditions, named by their RISC-V suffix.
enum class Cond : u8 { Eq, Ne, Lt, Ge, Ltu, Geu };

constexpr Cond negate(const Cond cond) {
  switch (cond) {
    case Cond::Eq: return Cond::Ne;
    case Cond::Ne: return Cond::Eq;
    case Cond::Lt: return Cond::Ge;
    case Cond::Ge: return Cond::Lt;
    case Cond::Ltu: return Cond::Geu;
    case Cond::Geu: return Cond::Ltu;
  }
  return Cond::Eq;
}

enum class MOp : u8 {
  Lui,
  Auipc,
  Jal,
  Jalr,
  Beq,
  Bne,
  Blt,
  Bge,
  Bltu,
  Bgeu,
  Lb,
  Lbu,
  Lh,
  Lhu,
  Lw,
  Lwu,
  Ld,
  Sb,
  Sh,
  Sw,
  Sd,
  Addi,
  Slti,
  Sltiu,
  Xori,
  Ori,
  Andi,
  Slli,
  Srli,
  Srai,
  Add,
  Sub,
  Slt,
  Sltu,
  Xor,
  Or,
  And,
  Sll,
  Srl,
  Sra,
  Mul,
  Mulh,
  Mulhu,
  Div,
  Divu,
  Rem,
  Remu,
  Fadd,
  Fsub,
  Fmul,
  Fdiv,
  Feq,
  Flt,
  Fle,
  Fcvt,
  Fmov,
  Fneg,
  // Floating-point loads and stores (f32/f64), width selects the form.
  Flw,
  Fld,
  Fsw,
  Fsd,
  // The address of a vreg's frame slot, resolved by the allocator.
  Addr,
  // The address of a data symbol, expanded by the backend into a
  // PC-relative auipc/addi pair.
  LoadAddr,
  Ecall,
  Nop,
};

// The allocator's three questions about an opcode, answered in one
// table so adding an opcode cannot leave the answers out of step.
struct OpTraits {
  bool writes_result = false;
  bool is_float = false;
  bool needs_register_operands = false;
};

constexpr OpTraits op_traits(const MOp op) {
  switch (op) {
    case MOp::Lui:
    case MOp::Auipc:
    case MOp::Jal:
    case MOp::Jalr:
    case MOp::Lbu:
    case MOp::Lb:
    case MOp::Lh:
    case MOp::Lhu:
    case MOp::Lw:
    case MOp::Lwu:
    case MOp::Ld:
    case MOp::Addr:
    case MOp::LoadAddr: return {.writes_result = true};
    case MOp::Addi:
    case MOp::Slti:
    case MOp::Sltiu:
    case MOp::Xori:
    case MOp::Ori:
    case MOp::Andi:
    case MOp::Slli:
    case MOp::Srli:
    case MOp::Srai:
    case MOp::Add:
    case MOp::Xor:
    case MOp::Or:
    case MOp::And: return {.writes_result = true};
    case MOp::Sub:
    case MOp::Sll:
    case MOp::Srl:
    case MOp::Sra:
    case MOp::Slt:
    case MOp::Sltu:
    case MOp::Div:
    case MOp::Divu:
    case MOp::Rem:
    case MOp::Remu:
    case MOp::Mul:
    case MOp::Mulh:
    case MOp::Mulhu:
      return {.writes_result = true, .needs_register_operands = true};
    case MOp::Fadd:
    case MOp::Fsub:
    case MOp::Fmul:
    case MOp::Fdiv:
      return {.writes_result = true,
              .is_float = true,
              .needs_register_operands = true};
    case MOp::Feq:
    case MOp::Flt:
    case MOp::Fle:
    case MOp::Fcvt:
    case MOp::Fmov:
    case MOp::Fneg:
    case MOp::Flw:
    case MOp::Fld: return {.writes_result = true, .is_float = true};
    case MOp::Fsw:
    case MOp::Fsd: return {.is_float = true};
    case MOp::Beq:
    case MOp::Bne:
    case MOp::Blt:
    case MOp::Bge:
    case MOp::Bltu:
    case MOp::Bgeu: return {.needs_register_operands = true};
    default: return {};
  }
}

struct MInst {
  MOp op = MOp::Nop;
  std::array<Operand, 3> ops{};
  u8 count = 0;
  u8 width = 8;
  Cond cond = Cond::Eq;
  static constexpr u32 NO_LABEL = 0xffffffff;
  u32 label = NO_LABEL;

  const Operand& at(const u32 index) const { return ops[index]; }
};

// The operands an ALU instruction encodes: an immediate form uses
// `immediate` and `rs1`, the register form uses `rs1` and `rs2`. The
// encoder and printer share this so both choose the same form.
struct AluOperands {
  const Operand* immediate = nullptr;
  const Operand* rs1 = nullptr;
  const Operand* rs2 = nullptr;
};

inline AluOperands alu_operands(const MInst& inst) {
  AluOperands result;
  if (inst.count > 1 && std::holds_alternative<Imm>(inst.at(1))) {
    result.immediate = &inst.at(1);
  } else if (inst.count > 2 && std::holds_alternative<Imm>(inst.at(2))) {
    result.immediate = &inst.at(2);
  }
  if (result.immediate != nullptr) {
    result.rs1 = result.immediate == &inst.at(1) && inst.count > 2
                     ? &inst.at(2)
                     : &inst.at(1);
    // The register form is unreachable here, but keep both source pointers
    // valid so a caller cannot dereference a null rs2.
    result.rs2 = result.rs1;
  } else {
    result.rs1 = inst.count > 2 ? &inst.at(1) : &inst.at(0);
    result.rs2 = inst.count > 2 ? &inst.at(2) : &inst.at(1);
  }
  return result;
}

inline MInst make_inst(const MOp op, const u8 width) {
  MInst inst;
  inst.op = op;
  inst.width = width;
  return inst;
}

inline MInst make_inst(const MOp op, const u8 width, Operand a) {
  MInst inst = make_inst(op, width);
  inst.ops[0] = a;
  inst.count = 1;
  return inst;
}

inline MInst make_inst(const MOp op, const u8 width, Operand a, Operand b) {
  MInst inst = make_inst(op, width, a);
  inst.ops[1] = b;
  inst.count = 2;
  return inst;
}

inline MInst make_inst(const MOp op,
                       const u8 width,
                       Operand a,
                       Operand b,
                       Operand c) {
  MInst inst = make_inst(op, width, a, b);
  inst.ops[2] = c;
  inst.count = 3;
  return inst;
}

inline MInst make_branch(const Cond cond,
                         const u8 width,
                         Operand lhs,
                         Operand rhs,
                         Operand target) {
  MInst inst = make_inst(MOp::Beq, width, lhs, rhs, target);
  inst.cond = cond;
  return inst;
}

struct Body {
  struct CallSite {
    u32 label = 0;
    std::string site;
  };
  std::vector<MInst> insts;
  std::vector<CallSite> call_sites;
  std::vector<u32> vreg_types;
  std::vector<u32> vreg_sizes;
  std::vector<bool> vreg_signed;
  std::vector<std::string> symbols;
  std::unordered_map<u32, u32> block_labels;
  u32 label_count = 0;
  bool uses_sret = false;
  // Stack bytes reserved for arguments past the eight register ones.
  u32 outgoing = 0;
};

}  // namespace codegen::riscv64
