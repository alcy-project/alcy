// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/x86_64/encode.h"

#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "codegen/object/elf64.h"
#include "codegen/x86_64/isa.h"
#include "codegen/x86_64/minst.h"
#include "debug/check.h"
#include "debug/dcheck.h"
#include "fpag/base/numeric.h"

namespace codegen::x86_64 {

namespace {

// The alu family's opcode slot, shared by every width.
enum class Alu : u8 {
  Add = 0,
  Or = 1,
  And = 4,
  Sub = 5,
  Xor = 6,
  Cmp = 7,
};

// The shift family's opcode slot.
enum class Shift : u8 {
  Shl = 4,
  Shr = 5,
  Sar = 7,
};

// The register a memory operand names. Allocation has answered every
// one; a vreg here is an emitter bug the encoder refuses to guess at.
Reg reg_of(const RegRef& ref) {
  const Reg* reg = std::get_if<Reg>(&ref);
  CHECK_MSG(reg != nullptr, "a vreg reached the encoder");
  return *reg;
}

std::optional<Reg> base_of(const Mem& mem) {
  if (!mem.base.has_value()) {
    return std::nullopt;
  }
  return reg_of(*mem.base);
}

std::optional<Reg> index_of(const Mem& mem) {
  if (!mem.index.has_value()) {
    return std::nullopt;
  }
  return reg_of(*mem.index);
}

u8 reg_code(const Reg reg) {
  return static_cast<u8>(reg) & 7;
}

bool rex_bit(const Reg reg) {
  return (static_cast<u8>(reg) & 8) != 0;
}

// Whether an 8-bit operation on this register needs a REX to name the
// byte form: without one, `sil` and its siblings read as the high bytes
// of `ax`, `cx`, `dx`, and `bx`.
bool byte_needs_rex(const Reg reg) {
  const u8 code = static_cast<u8>(reg);
  return code >= 4 && code < 8;
}

void put(std::vector<u8>& out, const u64 value, const u32 bytes) {
  for (u32 at = 0; at < bytes; ++at) {
    out.push_back(static_cast<u8>(value >> (at * 8)));
  }
}

void put_i32(std::vector<u8>& out, const i32 value) {
  put(out, static_cast<u64>(value), 4);
}

void put_i64(std::vector<u8>& out, const i64 value) {
  put(out, static_cast<u64>(value), 8);
}

// The prefixes a width asks for: `0x66` shrinks a 32-bit opcode to 16
// bits, `REX` lifts it to 64 or names the extended registers, and an
// 8-bit operation on a register the small forms cannot name takes a REX
// for that alone.
void prefix(std::vector<u8>& out,
            const u8 width,
            const bool w,
            const std::optional<Reg> reg,
            const std::optional<Reg> rm,
            const std::optional<Reg> index,
            const bool rm_register = true) {
  if (width == 2) {
    out.push_back(0x66);
  }
  const bool byte = width == 1;
  // Codes 4-7 in a byte operation name ah/ch/dh/bh without a REX and
  // spl/bpl/sil/dil with one. Their presence forces a bare REX; only
  // codes 8-15 set the REX.R/B extension bits.
  const bool byte_reg = byte && reg.has_value() && byte_needs_rex(*reg);
  const bool byte_rm =
      byte && rm.has_value() && rm_register && byte_needs_rex(*rm);
  const bool r = reg.has_value() && rex_bit(*reg);
  const bool b = rm.has_value() && rex_bit(*rm);
  const bool x = index.has_value() && rex_bit(*index);
  if (w || r || b || x || byte_reg || byte_rm) {
    out.push_back(static_cast<u8>(0x40 | (w ? 8 : 0) | (r ? 4 : 0) |
                                  (x ? 2 : 0) | (b ? 1 : 0)));
  }
}

// A register-to-register operand pair, in the `mod = 11` form.
void modrm_rr(std::vector<u8>& out, const Reg reg, const Reg rm) {
  out.push_back(static_cast<u8>(0xc0 | reg_code(reg) << 3 | reg_code(rm)));
}

// The ModRM and SIB bytes for one memory operand, and the size of the
// displacement that follows them, which the caller writes after.
u32 modrm_mem(std::vector<u8>& out, const Reg reg, const Mem& mem) {
  DCHECK(mem.base.has_value());
  const Reg base = reg_of(*mem.base);
  DCHECK(!is_xmm(base));
  const bool has_index =
      mem.index.has_value() && reg_code(reg_of(*mem.index)) != 4;
  const bool need_sib = has_index || reg_code(base) == 4;
  const i64 disp = mem.disp;

  if (!need_sib && reg_code(base) == 5) {
    // `[rbp]` has no zero-displacement form, and neither does the
    // rip-relative one this slot would otherwise read as.
    if (disp >= -128 && disp <= 127) {
      out.push_back(
          static_cast<u8>(0x40 | reg_code(reg) << 3 | reg_code(base)));
      return 1;
    }
    out.push_back(static_cast<u8>(0x80 | reg_code(reg) << 3 | reg_code(base)));
    return 4;
  }
  if (!need_sib) {
    if (disp == 0) {
      out.push_back(static_cast<u8>(reg_code(reg) << 3 | reg_code(base)));
      return 0;
    }
    if (disp >= -128 && disp <= 127) {
      out.push_back(
          static_cast<u8>(0x40 | reg_code(reg) << 3 | reg_code(base)));
      return 1;
    }
    out.push_back(static_cast<u8>(0x80 | reg_code(reg) << 3 | reg_code(base)));
    return 4;
  }

  // Base and index, or `[rsp]`, which always needs a SIB byte. An index
  // of `rsp` is spelled as no index at all.
  // A scale of one byte per element is the field the SIB carries
  // literally; `00` is what a scale of one is spelled as.
  const u8 scale_field = mem.scale == 1   ? 0
                         : mem.scale == 2 ? 1
                         : mem.scale == 4 ? 2
                                          : 3;
  const u8 index_code = has_index ? reg_code(reg_of(*mem.index)) : 4;
  const u8 sib =
      static_cast<u8>(scale_field << 6 | index_code << 3 | reg_code(base));
  // Every SIB form spells `rm = 100`, which is what tells the decoder
  // a SIB byte follows.
  if (reg_code(base) == 5) {
    if (disp >= -128 && disp <= 127) {
      out.push_back(static_cast<u8>(0x40 | reg_code(reg) << 3 | 4));
      out.push_back(sib);
      return 1;
    }
    out.push_back(static_cast<u8>(0x80 | reg_code(reg) << 3 | 4));
    out.push_back(sib);
    return 4;
  }
  if (disp == 0) {
    out.push_back(static_cast<u8>(reg_code(reg) << 3 | 4));
    out.push_back(sib);
    return 0;
  }
  if (disp >= -128 && disp <= 127) {
    out.push_back(static_cast<u8>(0x40 | reg_code(reg) << 3 | 4));
    out.push_back(sib);
    return 1;
  }
  out.push_back(static_cast<u8>(0x80 | reg_code(reg) << 3 | 4));
  out.push_back(sib);
  return 4;
}

// The condition slot the opcode family reads, which is the order the
// set and branch forms share and not the order the selector names them
// in.
u8 cond_code(const Cond cond) {
  switch (cond) {
    case Cond::O: return 0x0;
    case Cond::No: return 0x1;
    case Cond::E: return 0x4;
    case Cond::Ne: return 0x5;
    case Cond::B: return 0x2;
    case Cond::Be: return 0x6;
    case Cond::A: return 0x7;
    case Cond::Ae: return 0x3;
    case Cond::L: return 0xc;
    case Cond::Le: return 0xe;
    case Cond::G: return 0xf;
    case Cond::Ge: return 0xd;
    case Cond::S: return 0x8;
    case Cond::Ns: return 0x9;
    case Cond::P: return 0xa;
    case Cond::Np: return 0xb;
  }
  return 0x4;
}

// The mandatory prefix of a scalar SSE form, followed by whatever REX
// the registers need. XMM8-15 share their extension bit with the GPRs.
void sse_prefix(std::vector<u8>& out,
                const u8 mandatory,
                const std::optional<Reg> reg,
                const std::optional<Reg> rm,
                const bool wide = false) {
  if (mandatory != 0) {
    out.push_back(mandatory);
  }
  prefix(out, 4, wide, reg, rm, std::nullopt);
}

void alu_rr(std::vector<u8>& out,
            const Alu alu,
            const u8 width,
            const Reg dst,
            const Reg src) {
  prefix(out, width, width == 8, src, dst, std::nullopt);
  out.push_back(
      static_cast<u8>((width == 1 ? 0x00 : 0x01) | static_cast<u8>(alu) << 3));
  modrm_rr(out, src, dst);
}

void alu_rm_imm(std::vector<u8>& out,
                const Alu alu,
                const u8 width,
                const Reg dst,
                const i64 value) {
  prefix(out, width, width == 8, std::nullopt, dst, std::nullopt);
  // The opcode comes first and the register rides in the ModRM; a
  // 64-bit immediate does not exist, so a value that wide reaches an
  // alu opcode through a register instead.
  if (width == 1) {
    DCHECK(value >= -128 && value <= 127);
    out.push_back(0x80);
  } else if (value >= -128 && value <= 127) {
    out.push_back(0x83);
  } else {
    DCHECK_MSG(width != 8 || (value >= -0x80000000LL && value <= 0x7fffffffLL),
               "a 64-bit immediate needs a register");
    out.push_back(0x81);
  }
  out.push_back(
      static_cast<u8>(0xc0 | static_cast<u8>(alu) << 3 | reg_code(dst)));
  if (width == 1 || (value >= -128 && value <= 127)) {
    put(out, static_cast<u64>(value) & 0xff, 1);
  } else {
    put_i32(out, static_cast<i32>(value));
  }
}

void mem_disp(std::vector<u8>& out, const u32 disp_bytes, const i32 disp) {
  if (disp_bytes == 1) {
    put(out, static_cast<u64>(disp) & 0xff, 1);
  } else if (disp_bytes == 4) {
    put_i32(out, disp);
  }
}

}  // namespace

Encoded encode(const Body& body) {
  Encoded encoded;
  std::vector<u8>& out = encoded.bytes;
  out.reserve(body.insts.size() * 4);

  // Where each label landed, and the branches still waiting for one.
  std::vector<u64> label_at(body.label_count, 0);
  struct Branch {
    u64 at;
    u32 label;
  };
  std::vector<Branch> branches;

  for (const MInst& inst : body.insts) {
    if (inst.label != MInst::NO_LABEL) {
      DCHECK(inst.label < label_at.size());
      label_at[inst.label] = out.size();
    }
    for (u32 at = 0; at < inst.count; ++at) {
      DCHECK(!std::holds_alternative<VReg>(inst.at(at)));
    }
    const u8 width = inst.width;
    const bool w = width == 8;

    switch (inst.op) {
      case MOp::Mov: {
        const Operand& dst = inst.at(0);
        const Operand& src = inst.at(1);
        if (const Reg* to = std::get_if<Reg>(&dst); to != nullptr) {
          if (const Reg* from = std::get_if<Reg>(&src); from != nullptr) {
            prefix(out, width, w, *from, *to, std::nullopt);
            out.push_back(static_cast<u8>(width == 1 ? 0x88 : 0x89));
            modrm_rr(out, *from, *to);
          } else if (const Imm* value = std::get_if<Imm>(&src);
                     value != nullptr) {
            // A 32-bit immediate is what most forms carry; past it,
            // only the register form holds the whole value. At 64 bits
            // the register-immediate opcode reads a whole 64 bits, so a
            // smaller constant takes the store form instead.
            constexpr i64 I32_MAX = 0x7fffffff;
            constexpr i64 I32_MIN = -0x7fffffff - 1;
            if (w && (value->value > I32_MAX || value->value < I32_MIN)) {
              prefix(out, width, true, std::nullopt, *to, std::nullopt);
              out.push_back(static_cast<u8>(0xb8 + reg_code(*to)));
              put_i64(out, value->value);
            } else if (w) {
              prefix(out, width, true, std::nullopt, *to, std::nullopt);
              out.push_back(0xc7);
              out.push_back(static_cast<u8>(0xc0 | reg_code(*to)));
              put_i32(out, static_cast<i32>(value->value));
            } else {
              prefix(out, width, w, std::nullopt, *to, std::nullopt);
              out.push_back(static_cast<u8>(width == 1 ? 0xb0 : 0xb8) +
                            reg_code(*to));
              put(out, static_cast<u64>(value->value), width);
            }
          } else if (const Mem* from = std::get_if<Mem>(&src);
                     from != nullptr) {
            prefix(out, width, w, *to, base_of(*from), index_of(*from),
                   /*rm_register=*/false);
            out.push_back(static_cast<u8>(width == 1 ? 0x8a : 0x8b));
            mem_disp(out, modrm_mem(out, *to, *from), from->disp);
          } else if (const SymbolRef* from = std::get_if<SymbolRef>(&src);
                     from != nullptr) {
            // An address this object cannot know: the whole value rides
            // in the one immediate form that carries 64 bits, and the
            // link fills it.
            prefix(out, 8, true, std::nullopt, *to, std::nullopt);
            out.push_back(static_cast<u8>(0xb8 + reg_code(*to)));
            Encoded::Patch patch;
            patch.offset = out.size();
            patch.kind = object::RelocKind::Abs64;
            patch.symbol = body.symbols[from->id];
            patch.addend = 0;
            encoded.patches.push_back(std::move(patch));
            put_i64(out, 0);
          }
        } else if (const Mem* to = std::get_if<Mem>(&dst); to != nullptr) {
          if (const Reg* from = std::get_if<Reg>(&src); from != nullptr) {
            prefix(out, width, w, *from, base_of(*to), index_of(*to),
                   /*rm_register=*/false);
            out.push_back(static_cast<u8>(width == 1 ? 0x88 : 0x89));
            mem_disp(out, modrm_mem(out, *from, *to), to->disp);
          } else if (const Imm* value = std::get_if<Imm>(&src);
                     value != nullptr) {
            prefix(out, width, w, std::nullopt, base_of(*to), index_of(*to),
                   /*rm_register=*/false);
            out.push_back(width == 1 ? 0xc6 : 0xc7);
            mem_disp(out, modrm_mem(out, Reg::Rax, *to), to->disp);
            put(out, static_cast<u64>(value->value), width == 8 ? 4 : width);
          }
        }
        break;
      }
      case MOp::Movzx: {
        // Widens an unsigned value: `movzx r, r/m8` and its 16-bit form,
        // from a register or from memory.
        const Reg to = std::get<Reg>(inst.at(0));
        prefix(out, 8, true, to, std::nullopt, std::nullopt);
        out.push_back(0x0f);
        out.push_back(width == 1 ? 0xb6 : 0xb7);
        if (const Reg* reg_from = std::get_if<Reg>(&inst.at(1));
            reg_from != nullptr) {
          modrm_rr(out, to, *reg_from);
        } else {
          const Mem& mem_from = std::get<Mem>(inst.at(1));
          mem_disp(out, modrm_mem(out, to, mem_from), mem_from.disp);
        }
        break;
      }
      case MOp::Movsx: {
        // Widens a signed value, from a register or from memory: the
        // narrow forms have their own opcodes and the 32-bit one is
        // named for itself.
        const Reg to = std::get<Reg>(inst.at(0));
        if (width == 4) {
          // `movsxd r64, r/m32` has no 0f prefix; the wider form of the
          // two-byte movsx would be an MMX instruction.
          prefix(out, 8, true, to, std::nullopt, std::nullopt);
          out.push_back(0x63);
        } else {
          prefix(out, 8, true, to, std::nullopt, std::nullopt);
          out.push_back(0x0f);
          out.push_back(width == 1 ? 0xbe : 0xbf);
        }
        if (const Reg* reg_from = std::get_if<Reg>(&inst.at(1));
            reg_from != nullptr) {
          modrm_rr(out, to, *reg_from);
        } else {
          const Mem& mem_from = std::get<Mem>(inst.at(1));
          mem_disp(out, modrm_mem(out, to, mem_from), mem_from.disp);
        }
        break;
      }
      case MOp::Lea: {
        const Reg to = std::get<Reg>(inst.at(0));
        const Mem& from = std::get<Mem>(inst.at(1));
        prefix(out, 8, true, to, base_of(from), index_of(from),
               /*rm_register=*/false);
        out.push_back(0x8d);
        mem_disp(out, modrm_mem(out, to, from), from.disp);
        break;
      }
      case MOp::Movss:
      case MOp::Movsd: {
        const u8 mand = inst.op == MOp::Movss ? 0xf3 : 0xf2;
        const Operand& dst = inst.at(0);
        const Operand& src = inst.at(1);
        if (const Reg* to = std::get_if<Reg>(&dst); to != nullptr) {
          DCHECK(is_xmm(*to));
          if (const Reg* from = std::get_if<Reg>(&src); from != nullptr) {
            sse_prefix(out, mand, *to, *from);
            out.push_back(0x0f);
            out.push_back(0x10);
            modrm_rr(out, *to, *from);
          } else if (const Mem* from = std::get_if<Mem>(&src);
                     from != nullptr) {
            sse_prefix(out, mand, *to, base_of(*from));
            out.push_back(0x0f);
            out.push_back(0x10);
            mem_disp(out, modrm_mem(out, *to, *from), from->disp);
          } else if (const SymbolRef* from = std::get_if<SymbolRef>(&src);
                     from != nullptr) {
            // The address is materialized by allocation; a symbol here
            // holds the float's bytes, loaded through a GPR scratch.
            DCHECK_MSG(false, "a float symbol reaches encoding unresolved");
          }
        } else if (const Mem* to = std::get_if<Mem>(&dst); to != nullptr) {
          const Reg* from = std::get_if<Reg>(&src);
          DCHECK(from != nullptr && is_xmm(*from));
          sse_prefix(out, mand, *from, base_of(*to));
          out.push_back(0x0f);
          out.push_back(0x11);
          mem_disp(out, modrm_mem(out, *from, *to), to->disp);
        }
        break;
      }
      case MOp::Fadd:
      case MOp::Fsub:
      case MOp::Fmul:
      case MOp::Fdiv: {
        const u8 mand = inst.width == 4 ? 0xf3 : 0xf2;
        const u8 opcode = inst.op == MOp::Fadd   ? 0x58
                          : inst.op == MOp::Fsub ? 0x5c
                          : inst.op == MOp::Fmul ? 0x59
                                                 : 0x5e;
        const Reg dst = std::get<Reg>(inst.at(0));
        const Reg src = std::get<Reg>(inst.at(1));
        sse_prefix(out, mand, dst, src);
        out.push_back(0x0f);
        out.push_back(opcode);
        modrm_rr(out, dst, src);
        break;
      }
      case MOp::Fneg: {
        const Reg dst = std::get<Reg>(inst.at(0));
        // xorps with an all-zero register flips the sign bit. Allocation
        // reserves the zero register as the second operand.
        const Reg zero = std::get<Reg>(inst.at(1));
        sse_prefix(out, 0x66, dst, zero);
        out.push_back(0x0f);
        out.push_back(0x57);
        modrm_rr(out, dst, zero);
        break;
      }
      case MOp::Fcom: {
        // comiss has no mandatory prefix; comisd uses 66 (not F2).
        const Reg lhs = std::get<Reg>(inst.at(0));
        const Reg rhs = std::get<Reg>(inst.at(1));
        sse_prefix(out, inst.width == 4 ? 0x00 : 0x66, lhs, rhs);
        out.push_back(0x0f);
        out.push_back(0x2f);
        modrm_rr(out, lhs, rhs);
        break;
      }
      case MOp::CvtI2F: {
        const u8 mand = inst.width == 4 ? 0xf3 : 0xf2;
        const Reg dst = std::get<Reg>(inst.at(0));
        const Reg src = std::get<Reg>(inst.at(1));
        sse_prefix(out, mand, dst, src, /*wide=*/inst.width == 8);
        out.push_back(0x0f);
        out.push_back(0x2a);
        modrm_rr(out, dst, src);
        break;
      }
      case MOp::CvtF2I: {
        const u8 mand = inst.width == 4 ? 0xf3 : 0xf2;
        const Reg dst = std::get<Reg>(inst.at(0));
        const Reg src = std::get<Reg>(inst.at(1));
        const bool wide = inst.count > 2 &&
                          std::holds_alternative<Imm>(inst.at(2)) &&
                          std::get<Imm>(inst.at(2)).value == 8;
        sse_prefix(out, mand, dst, src, /*wide=*/wide);
        out.push_back(0x0f);
        out.push_back(0x2c);
        modrm_rr(out, dst, src);
        break;
      }
      case MOp::CvtF2F: {
        // A widening conversion f32 -> f64 is F3 0F 5A; narrowing is
        // F2 0F 5A. The width field names the source.
        const u8 mand = inst.width == 4 ? 0xf3 : 0xf2;
        const Reg dst = std::get<Reg>(inst.at(0));
        const Reg src = std::get<Reg>(inst.at(1));
        sse_prefix(out, mand, dst, src);
        out.push_back(0x0f);
        out.push_back(0x5a);
        modrm_rr(out, dst, src);
        break;
      }
      case MOp::Add:
      case MOp::Sub:
      case MOp::And:
      case MOp::Or:
      case MOp::Xor:
      case MOp::Cmp: {
        const Alu alu = inst.op == MOp::Add   ? Alu::Add
                        : inst.op == MOp::Sub ? Alu::Sub
                        : inst.op == MOp::And ? Alu::And
                        : inst.op == MOp::Or  ? Alu::Or
                        : inst.op == MOp::Xor ? Alu::Xor
                                              : Alu::Cmp;
        const Reg dst = std::get<Reg>(inst.at(0));
        if (const Reg* src = std::get_if<Reg>(&inst.at(1)); src != nullptr) {
          alu_rr(out, alu, width, dst, *src);
        } else {
          alu_rm_imm(out, alu, width, dst, std::get<Imm>(inst.at(1)).value);
        }
        break;
      }
      case MOp::Imul: {
        // The two-operand form: `dst *= r/m`.
        const Reg dst = std::get<Reg>(inst.at(0));
        const Reg src = std::get<Reg>(inst.at(1));
        prefix(out, width, w, dst, src, std::nullopt);
        out.push_back(0x0f);
        out.push_back(0xaf);
        modrm_rr(out, dst, src);
        break;
      }
      case MOp::Umul: {
        const Reg src = std::get<Reg>(inst.at(0));
        prefix(out, width, w, std::nullopt, src, std::nullopt);
        out.push_back(0xf7);
        out.push_back(static_cast<u8>(0xc0 | 4 << 3 | reg_code(src)));
        break;
      }
      case MOp::Idiv:
      case MOp::Udiv: {
        // One operand, the divisor. The dividend and its sign are in
        // `rax` and `rdx` already.
        const Reg src = std::get<Reg>(inst.at(0));
        prefix(out, width, w, std::nullopt, src, std::nullopt);
        out.push_back(0xf7);
        out.push_back(static_cast<u8>(
            0xc0 | (inst.op == MOp::Idiv ? 7 : 6) << 3 | reg_code(src)));
        break;
      }
      case MOp::Shl:
      case MOp::Shr:
      case MOp::Sar: {
        const Shift shift = inst.op == MOp::Shl   ? Shift::Shl
                            : inst.op == MOp::Shr ? Shift::Shr
                                                  : Shift::Sar;
        const Reg dst = std::get<Reg>(inst.at(0));
        prefix(out, width, w, std::nullopt, dst, std::nullopt);
        const bool by_register = std::holds_alternative<Reg>(inst.at(1));
        if (by_register) {
          // `cl` is the only register a shift reads its count from.
          DCHECK(std::get<Reg>(inst.at(1)) == Reg::Rcx);
          out.push_back(width == 1 ? 0xd2 : 0xd3);
        } else if (std::get<Imm>(inst.at(1)).value == 1) {
          out.push_back(width == 1 ? 0xd0 : 0xd1);
        } else {
          out.push_back(width == 1 ? 0xc0 : 0xc1);
        }
        out.push_back(static_cast<u8>(0xc0 | static_cast<u8>(shift) << 3 |
                                      reg_code(dst)));
        if (!by_register && std::get<Imm>(inst.at(1)).value != 1) {
          put(out, static_cast<u64>(std::get<Imm>(inst.at(1)).value) & 0xff, 1);
        }
        break;
      }
      case MOp::Not: {
        const Reg dst = std::get<Reg>(inst.at(0));
        prefix(out, width, w, std::nullopt, dst, std::nullopt);
        out.push_back(0xf7);
        out.push_back(static_cast<u8>(0xc0 | 2 << 3 | reg_code(dst)));
        break;
      }
      case MOp::Neg: {
        const Reg dst = std::get<Reg>(inst.at(0));
        prefix(out, width, w, std::nullopt, dst, std::nullopt);
        out.push_back(0xf7);
        out.push_back(static_cast<u8>(0xc0 | 3 << 3 | reg_code(dst)));
        break;
      }
      case MOp::Test: {
        const Reg dst = std::get<Reg>(inst.at(0));
        const Reg src = std::get<Reg>(inst.at(1));
        prefix(out, width, w, src, dst, std::nullopt);
        out.push_back(width == 1 ? 0x84 : 0x85);
        modrm_rr(out, src, dst);
        break;
      }
      case MOp::Setcc: {
        const Reg dst = std::get<Reg>(inst.at(0));
        prefix(out, 1, false, dst, std::nullopt, std::nullopt);
        out.push_back(0x0f);
        out.push_back(static_cast<u8>(0x90 + cond_code(inst.cond)));
        out.push_back(static_cast<u8>(0xc0 | reg_code(dst)));
        break;
      }
      case MOp::Jmp: {
        const Label target = std::get<Label>(inst.at(0));
        out.push_back(0xe9);
        branches.push_back(Branch{.at = out.size(), .label = target.id});
        put_i32(out, 0);
        break;
      }
      case MOp::Jcc: {
        const Label target = std::get<Label>(inst.at(0));
        out.push_back(0x0f);
        out.push_back(static_cast<u8>(0x80 + cond_code(inst.cond)));
        branches.push_back(Branch{.at = out.size(), .label = target.id});
        put_i32(out, 0);
        break;
      }
      case MOp::Call: {
        const SymbolRef target = std::get<SymbolRef>(inst.at(0));
        out.push_back(0xe8);
        Encoded::Patch patch;
        patch.offset = out.size();
        patch.kind = object::RelocKind::Plt32;
        patch.symbol = body.symbols[target.id];
        patch.addend = -4;
        encoded.patches.push_back(std::move(patch));
        put_i32(out, 0);
        break;
      }
      case MOp::CallReg: {
        // `call r64`, which reads its target from the register.
        const Reg target = std::get<Reg>(inst.at(0));
        prefix(out, 8, false, std::nullopt, target, std::nullopt);
        out.push_back(0xff);
        out.push_back(static_cast<u8>(0xc0 | 2 << 3 | reg_code(target)));
        break;
      }
      case MOp::Ret: {
        out.push_back(0xc3);
        break;
      }
      case MOp::Syscall: {
        out.push_back(0x0f);
        out.push_back(0x05);
        break;
      }
      case MOp::Nop: {
        out.push_back(0x90);
        break;
      }
      case MOp::Push: {
        const Reg reg = std::get<Reg>(inst.at(0));
        if (rex_bit(reg)) {
          out.push_back(0x41);
        }
        out.push_back(static_cast<u8>(0x50 + reg_code(reg)));
        break;
      }
      case MOp::Pop: {
        const Reg reg = std::get<Reg>(inst.at(0));
        if (rex_bit(reg)) {
          out.push_back(0x41);
        }
        out.push_back(static_cast<u8>(0x58 + reg_code(reg)));
        break;
      }
      case MOp::Cqo: {
        // Sign-extends `rax` into `rdx:rax`, at whatever width the
        // dividend runs.
        if (w) {
          out.push_back(0x48);
        }
        out.push_back(0x99);
        break;
      }
      case MOp::RepMovsb: {
        out.push_back(0xfc);
        out.push_back(0xf3);
        out.push_back(0xa4);
        break;
      }
      case MOp::RepStosb: {
        out.push_back(0xfc);
        out.push_back(0xf3);
        out.push_back(0xaa);
        break;
      }
      case MOp::Ud2: {
        out.push_back(0x0f);
        out.push_back(0x0b);
        break;
      }
    }
  }

  // The branches close over the offsets their targets landed at.
  for (const Branch& branch : branches) {
    const i64 delta = static_cast<i64>(label_at[branch.label]) -
                      static_cast<i64>(branch.at + 4);
    if (branch.at + 4 > out.size()) {
      // A branch the body ends with has nowhere to fall through to; the
      // bytes past it are never run.
      out.resize(static_cast<usize>(branch.at + 4), 0);
    }
    for (u32 at = 0; at < 4; ++at) {
      out[branch.at + at] =
          static_cast<u8>(static_cast<u64>(delta) >> (at * 8));
    }
  }
  return encoded;
}

}  // namespace codegen::x86_64
