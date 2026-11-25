// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/riscv64/encoder.h"

#include <cstdint>
#include <optional>
#include <variant>

#include "codegen/riscv64/isa.h"
#include "codegen/riscv64/minst.h"
#include "debug/dcheck.h"
#include "fpag/base/numeric.h"

namespace codegen::riscv64 {
namespace {

constexpr u32 OP_IMM = 0x13;
constexpr u32 LOAD = 0x03;
constexpr u32 STORE = 0x23;
constexpr u32 BRANCH = 0x63;
constexpr u32 JAL = 0x6f;
constexpr u32 JALR = 0x67;
constexpr u32 LUI = 0x37;
constexpr u32 AUIPC = 0x17;
[[maybe_unused]] constexpr u32 SYSTEM = 0x73;
constexpr u32 OP = 0x33;
[[maybe_unused]] constexpr u32 OP_FP = 0x53;

u32 bits(u32 value, const u32 hi, const u32 lo) {
  return (value >> lo) & ((1u << (hi - lo + 1)) - 1);
}

u32 r_type(const u32 funct7,
           const u32 rs2,
           const u32 rs1,
           const u32 funct3,
           const u32 rd,
           const u32 opcode) {
  return funct7 << 25 | rs2 << 20 | rs1 << 15 | funct3 << 12 | rd << 7 | opcode;
}

u32 i_type(const i32 imm,
           const u32 rs1,
           const u32 funct3,
           const u32 rd,
           const u32 opcode) {
  return static_cast<u32>(imm) << 20 | rs1 << 15 | funct3 << 12 | rd << 7 |
         opcode;
}

u32 s_type(const i32 imm,
           const u32 rs2,
           const u32 rs1,
           const u32 funct3,
           const u32 opcode) {
  const u32 value = static_cast<u32>(imm);
  return bits(value, 11, 5) << 25 | rs2 << 20 | rs1 << 15 | funct3 << 12 |
         bits(value, 4, 0) << 7 | opcode;
}

u32 b_type(const i32 imm,
           const u32 rs2,
           const u32 rs1,
           const u32 funct3,
           const u32 opcode) {
  const u32 value = static_cast<u32>(imm);
  return bits(value, 12, 12) << 31 | bits(value, 10, 5) << 25 | rs2 << 20 |
         rs1 << 15 | funct3 << 12 | bits(value, 4, 1) << 8 |
         bits(value, 11, 11) << 7 | opcode;
}

u32 u_type(const i32 imm, const u32 rd, const u32 opcode) {
  return (static_cast<u32>(imm) & 0xfffff000u) | rd << 7 | opcode;
}

u32 j_type(const i32 imm, const u32 rd, const u32 opcode) {
  const u32 value = static_cast<u32>(imm);
  return bits(value, 20, 20) << 31 | bits(value, 10, 1) << 21 |
         bits(value, 11, 11) << 20 | bits(value, 19, 12) << 12 | rd << 7 |
         opcode;
}

u32 reg(const Operand& operand) {
  if (const Reg* value = std::get_if<Reg>(&operand); value != nullptr) {
    return reg_code(*value);
  }
  return 0;
}

i32 imm(const Operand& operand) {
  if (const Imm* value = std::get_if<Imm>(&operand); value != nullptr) {
    return static_cast<i32>(value->value);
  }
  return 0;
}

const Label* label_of(const Operand& operand) {
  return std::get_if<Label>(&operand);
}

u32 branch_funct3(const Cond cond) {
  switch (cond) {
    case Cond::Eq: return 0;
    case Cond::Ne: return 1;
    case Cond::Lt: return 4;
    case Cond::Ge: return 5;
    case Cond::Ltu: return 6;
    case Cond::Geu: return 7;
  }
  return 0;
}

u32 load_funct3(const MOp op) {
  switch (op) {
    case MOp::Lb: return 0;
    case MOp::Lbu: return 4;
    case MOp::Lh: return 1;
    case MOp::Lhu: return 5;
    case MOp::Lw: return 2;
    case MOp::Lwu: return 6;
    case MOp::Ld: return 3;
    default: return 3;
  }
}

u32 store_funct3(const MOp op) {
  switch (op) {
    case MOp::Sb: return 0;
    case MOp::Sh: return 1;
    case MOp::Sw: return 2;
    case MOp::Sd: return 3;
    default: return 3;
  }
}

u32 alu_funct(const MOp op, u32& funct7) {
  funct7 = 0;
  switch (op) {
    case MOp::Add:
    case MOp::Addi: return 0;
    case MOp::Sub: funct7 = 0x20; return 0;
    case MOp::Sll:
    case MOp::Slli: return 1;
    case MOp::Slt:
    case MOp::Slti: return 2;
    case MOp::Sltu:
    case MOp::Sltiu: return 3;
    case MOp::Xor:
    case MOp::Xori: return 4;
    case MOp::Srl:
    case MOp::Srli: return 5;
    case MOp::Sra:
    case MOp::Srai: funct7 = 0x20; return 5;
    case MOp::Or:
    case MOp::Ori: return 6;
    case MOp::And:
    case MOp::Andi: return 7;
    case MOp::Mul: funct7 = 1; return 0;
    case MOp::Mulh: funct7 = 1; return 1;
    case MOp::Mulhu: funct7 = 1; return 2;
    case MOp::Div: funct7 = 1; return 4;
    case MOp::Divu: funct7 = 1; return 5;
    case MOp::Rem: funct7 = 1; return 6;
    case MOp::Remu: funct7 = 1; return 7;
    default: return 0;
  }
}

// Re-embed a 21-bit J-type or a split 13-bit B-type immediate into a
// word that was emitted with a zero placeholder.
u32 patch_word(const u32 word, const i32 delta) {
  const u32 opcode = word & 0x7f;
  const u32 value = static_cast<u32>(delta);
  if (opcode == JAL) {
    return (bits(value, 20, 20) << 31) | (word & 0x000ffff0) |
           (bits(value, 10, 1) << 21) | (bits(value, 11, 11) << 20) |
           (bits(value, 19, 12) << 12) | (word & 0x0000007f);
  }
  // Preserve bits 8-24 (funct3, rs1 and all five bits of rs2); only
  // the branch displacement bits 7, 8-11, 25-30 and 31 are rewritten.
  return (bits(value, 12, 12) << 31) | (word & 0x01ffff00) |
         (bits(value, 10, 5) << 25) | (bits(value, 4, 1) << 8) |
         (bits(value, 11, 11) << 7) | (word & 0x0000007f);
}

}  // namespace

Encoded encode(const Body& body) {
  Encoded encoded;
  u32 labels = body.label_count;
  for (const MInst& inst : body.insts) {
    if (inst.label != MInst::NO_LABEL && inst.label >= labels) {
      labels = inst.label + 1;
    }
  }
  encoded.label_offsets.assign(labels, 0);
  for (const MInst& inst : body.insts) {
    if (inst.label != MInst::NO_LABEL) {
      encoded.label_offsets[inst.label] =
          static_cast<u64>(encoded.bytes.size());
    }
    u32 word = 0;
    switch (inst.op) {
      case MOp::Lui:
        word = u_type(imm(inst.at(1)), reg(inst.at(0)), LUI);
        break;
      case MOp::Auipc:
        word = u_type(imm(inst.at(1)), reg(inst.at(0)), AUIPC);
        break;
      case MOp::Jal:
        if (const Label* label = label_of(inst.at(1)); label != nullptr) {
          encoded.patches.push_back(Encoded::Patch{
              .offset = encoded.bytes.size(), .label = label->id});
          word = j_type(0, reg(inst.at(0)), JAL);
        } else {
          word = j_type(imm(inst.at(1)), reg(inst.at(0)), JAL);
        }
        break;
      case MOp::Jalr:
        word =
            i_type(imm(inst.at(2)), reg(inst.at(1)), 0, reg(inst.at(0)), JALR);
        break;
      case MOp::Beq:
      case MOp::Bne:
      case MOp::Blt:
      case MOp::Bge:
      case MOp::Bltu:
      case MOp::Bgeu:
        if (const Label* label = label_of(inst.at(2)); label != nullptr) {
          encoded.patches.push_back(Encoded::Patch{
              .offset = encoded.bytes.size(), .label = label->id});
          word = b_type(0, reg(inst.at(1)), reg(inst.at(0)),
                        branch_funct3(inst.cond), BRANCH);
        } else {
          word = b_type(imm(inst.at(2)), reg(inst.at(1)), reg(inst.at(0)),
                        branch_funct3(inst.cond), BRANCH);
        }
        break;
      case MOp::Lbu:
      case MOp::Lb:
      case MOp::Lh:
      case MOp::Lhu:
      case MOp::Lw:
      case MOp::Lwu:
      case MOp::Ld: {
        const Mem& mem = std::get<Mem>(inst.at(1));
        DCHECK(mem.base.has_value());
        word = i_type(mem.disp,
                      reg_code(std::get_if<Reg>(&*mem.base)
                                   ? *std::get_if<Reg>(&*mem.base)
                                   : ZERO),
                      load_funct3(inst.op), reg(inst.at(0)), LOAD);
        break;
      }
      case MOp::Sb:
      case MOp::Sh:
      case MOp::Sw:
      case MOp::Sd: {
        const Mem& mem = std::get<Mem>(inst.at(1));
        DCHECK(mem.base.has_value());
        word = s_type(mem.disp, reg(inst.at(0)),
                      reg_code(std::get_if<Reg>(&*mem.base)
                                   ? *std::get_if<Reg>(&*mem.base)
                                   : ZERO),
                      store_funct3(inst.op), STORE);
        break;
      }
      case MOp::Flw:
      case MOp::Fld: {
        const Mem& mem = std::get<Mem>(inst.at(1));
        DCHECK(mem.base.has_value());
        word = i_type(mem.disp,
                      reg_code(std::get_if<Reg>(&*mem.base)
                                   ? *std::get_if<Reg>(&*mem.base)
                                   : ZERO),
                      inst.op == MOp::Flw ? 2 : 3, reg(inst.at(0)), 0x07);
        break;
      }
      case MOp::Fsw:
      case MOp::Fsd: {
        const Mem& mem = std::get<Mem>(inst.at(1));
        DCHECK(mem.base.has_value());
        word = s_type(mem.disp, reg(inst.at(0)),
                      reg_code(std::get_if<Reg>(&*mem.base)
                                   ? *std::get_if<Reg>(&*mem.base)
                                   : ZERO),
                      inst.op == MOp::Fsw ? 2 : 3, 0x27);
        break;
      }
      case MOp::Fadd:
      case MOp::Fsub:
      case MOp::Fmul:
      case MOp::Fdiv: {
        const bool single = inst.width == 4;
        u32 funct7 = 0;
        switch (inst.op) {
          case MOp::Fadd: funct7 = single ? 0x00 : 0x01; break;
          case MOp::Fsub: funct7 = single ? 0x04 : 0x05; break;
          case MOp::Fmul: funct7 = single ? 0x08 : 0x09; break;
          case MOp::Fdiv: funct7 = single ? 0x0c : 0x0d; break;
          default: break;
        }
        word = r_type(funct7, reg(inst.at(2)), reg(inst.at(1)), 0,
                      reg(inst.at(0)), OP_FP);
        break;
      }
      case MOp::Feq:
      case MOp::Flt:
      case MOp::Fle: {
        const u32 funct3 = inst.op == MOp::Feq   ? 2
                           : inst.op == MOp::Flt ? 1
                                                 : 0;
        const u32 funct7 = inst.width == 4 ? 0x50 : 0x51;
        word = r_type(funct7, reg(inst.at(2)), reg(inst.at(1)), funct3,
                      reg(inst.at(0)), OP_FP);
        break;
      }
      case MOp::Fcvt: {
        const FcvtMode mode = inst.count > 2
                                  ? static_cast<FcvtMode>(imm(inst.at(2)))
                                  : FcvtMode::F64ToI64;
        u32 funct7 = 0;
        u32 funct3 = 0;
        u32 rs2 = 0;
        switch (mode) {
          case FcvtMode::F64ToI64:
            funct7 = 0x61;
            funct3 = 1;  // rtz: truncate toward zero
            rs2 = 2;
            break;  // fcvt.l.d
          case FcvtMode::F32ToI64:
            funct7 = 0x60;
            funct3 = 1;
            rs2 = 2;
            break;  // fcvt.l.s
          case FcvtMode::I64ToF64:
            funct7 = 0x69;
            funct3 = 7;
            rs2 = 2;
            break;  // fcvt.d.l
          case FcvtMode::I32ToF64:
            funct7 = 0x69;
            funct3 = 0;
            break;  // fcvt.d.w
          case FcvtMode::F64ToIU64:
            funct7 = 0x61;
            funct3 = 1;
            rs2 = 3;
            break;  // fcvt.lu.d
          case FcvtMode::F32ToIU64:
            funct7 = 0x60;
            funct3 = 1;
            rs2 = 3;
            break;  // fcvt.lu.s
          case FcvtMode::F64ToF32:
            funct7 = 0x20;
            funct3 = 7;
            rs2 = 1;
            break;  // fcvt.s.d
          case FcvtMode::F32ToF64:
            funct7 = 0x21;
            funct3 = 0;
            rs2 = 0;
            break;  // fcvt.d.s
          case FcvtMode::IU64ToF64:
            funct7 = 0x69;
            funct3 = 7;
            rs2 = 3;
            break;  // fcvt.d.lu
          case FcvtMode::IU32ToF64:
            funct7 = 0x69;
            funct3 = 0;
            rs2 = 1;
            break;  // fcvt.d.wu
          case FcvtMode::IU64ToF32:
            funct7 = 0x68;
            funct3 = 7;
            rs2 = 3;
            break;  // fcvt.s.lu
          case FcvtMode::IU32ToF32:
            funct7 = 0x68;
            funct3 = 7;
            rs2 = 1;
            break;  // fcvt.s.wu
          case FcvtMode::I64ToF32:
            funct7 = 0x68;
            funct3 = 7;
            rs2 = 2;
            break;  // fcvt.s.l
          case FcvtMode::I32ToF32:
            funct7 = 0x68;
            funct3 = 7;
            rs2 = 0;
            break;  // fcvt.s.w
          default: break;
        }
        word = r_type(funct7, rs2, reg(inst.at(1)), funct3, reg(inst.at(0)),
                      OP_FP);
        break;
      }
      case MOp::Fneg: {
        // fsgnjn rd, rs, rs: negate the sign of rs (fneg).
        word = r_type(inst.width == 4 ? 0x10 : 0x11, reg(inst.at(1)),
                      reg(inst.at(1)), 1, reg(inst.at(0)), OP_FP);
        break;
      }
      case MOp::Fmov: {
        // fsgnj: rd = rs1 with rs2's sign; using the same register is a copy.
        word = r_type(inst.width == 4 ? 0x10 : 0x11, reg(inst.at(1)),
                      reg(inst.at(1)), 0, reg(inst.at(0)), OP_FP);
        break;
      }
      case MOp::Ecall: word = 0x00000073; break;
      case MOp::Nop: word = 0x00000013; break;
      default: {
        u32 funct7 = 0;
        const u32 funct3 = alu_funct(inst.op, funct7);
        const Operand* immediate = nullptr;
        if (inst.count > 1 && std::holds_alternative<Imm>(inst.at(1))) {
          immediate = &inst.at(1);
        } else if (inst.count > 2 && std::holds_alternative<Imm>(inst.at(2))) {
          immediate = &inst.at(2);
        }
        if (immediate != nullptr) {
          const u32 rs1 = immediate == &inst.at(1) && inst.count > 2
                              ? reg(inst.at(2))
                              : reg(inst.at(1));
          // There is no subi: subtract by adding the negated immediate.
          const i32 value =
              inst.op == MOp::Sub ? -imm(*immediate) : imm(*immediate);
          // RV64 shifts take a six-bit amount: funct6 occupies the
          // immediate's high six bits and the amount the low six. `srai`
          // needs funct6 0x10 there or it decodes as `srli`.
          const bool shift = inst.op == MOp::Slli || inst.op == MOp::Srli ||
                             inst.op == MOp::Srai;
          const i32 encoded =
              shift ? static_cast<i32>(((funct7 >> 1) << 6) | (value & 0x3f))
                    : value;
          word = i_type(encoded, rs1, funct3, reg(inst.at(0)), OP_IMM);
        } else {
          // Three-operand RISC-V: rd is operand 0, the sources are 1/2.
          const u32 rs1 = reg(inst.count > 2 ? inst.at(1) : inst.at(0));
          const u32 rs2 = reg(inst.count > 2 ? inst.at(2) : inst.at(1));
          word = r_type(funct7, rs2, rs1, funct3, reg(inst.at(0)), OP);
        }
        break;
      }
    }
    // Fixed-width instructions: append the little-endian word.
    encoded.bytes.push_back(static_cast<u8>(word & 0xff));
    encoded.bytes.push_back(static_cast<u8>((word >> 8) & 0xff));
    encoded.bytes.push_back(static_cast<u8>((word >> 16) & 0xff));
    encoded.bytes.push_back(static_cast<u8>((word >> 24) & 0xff));
  }
  for (const Encoded::Patch& patch : encoded.patches) {
    const i64 delta = static_cast<i64>(encoded.label_offsets[patch.label]) -
                      static_cast<i64>(patch.offset);
    const u32 word = static_cast<u32>(encoded.bytes[patch.offset]) |
                     static_cast<u32>(encoded.bytes[patch.offset + 1]) << 8 |
                     static_cast<u32>(encoded.bytes[patch.offset + 2]) << 16 |
                     static_cast<u32>(encoded.bytes[patch.offset + 3]) << 24;
    const u32 patched = patch_word(word, static_cast<i32>(delta));
    encoded.bytes[patch.offset] = static_cast<u8>(patched & 0xff);
    encoded.bytes[patch.offset + 1] = static_cast<u8>((patched >> 8) & 0xff);
    encoded.bytes[patch.offset + 2] = static_cast<u8>((patched >> 16) & 0xff);
    encoded.bytes[patch.offset + 3] = static_cast<u8>((patched >> 24) & 0xff);
  }
  return encoded;
}

}  // namespace codegen::riscv64
