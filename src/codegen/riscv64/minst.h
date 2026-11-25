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

using Operand = std::
    variant<std::monostate, VReg, Reg, Imm, Mem, Label, SymbolRef, SlotRef>;

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
};

}  // namespace codegen::riscv64
