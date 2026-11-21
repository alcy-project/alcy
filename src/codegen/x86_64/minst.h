// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "codegen/x86_64/isa.h"
#include "fpag/base/numeric.h"

namespace codegen::x86_64 {

// A register the body selects to. One per IR register a function reads,
// unbounded: allocation is what gives each a frame slot.
struct VReg {
  u32 index = 0;
};

// An immediate the encoder writes whole. Values beyond a 32-bit
// immediate reach a register through the 64-bit move form.
struct Imm {
  i64 value = 0;
};

// A register a memory operand names: the one that holds the value, or
// the vreg whose slot holds it. Allocation answers every one with a
// register before the encoder reads it.
using RegRef = std::variant<VReg, Reg>;

// A memory reference. `base` absent is rip-relative, which is how a
// symbol's address rides in an instruction; `index` absent is a plain
// base plus displacement.
struct Mem {
  std::optional<RegRef> base;
  std::optional<RegRef> index;
  u8 scale = 1;
  i32 disp = 0;
};

// A block of this body, named so a branch can reach it before its bytes
// exist.
struct Label {
  u32 id = 0;
};

// A symbol this body names: a callee, or a datum in another section. The
// id indexes the body's own table, so a call costs nothing to spell.
struct SymbolRef {
  u32 id = 0;
};

// A word of one vreg's own storage, at the offset from where its slot
// starts. This is how a field of an aggregate the body holds, or the
// second word of a two-word value, is named before allocation knows
// where the slot sits; allocation answers it with a plain frame
// reference and emits nothing.
struct SlotRef {
  u32 vreg = 0;
  i32 offset = 0;
};

using Operand = std::
    variant<std::monostate, VReg, Reg, Imm, Mem, Label, SymbolRef, SlotRef>;

// The branch conditions, in the order the encoder's opcode offset uses.
enum class Cond : u8 {
  O,
  No,
  E,
  Ne,
  L,
  Le,
  G,
  Ge,
  B,
  Be,
  A,
  Ae,
  S,
  Ns,
  P,
  Np,
};

enum class MOp : u8 {
  Mov,
  Movzx,
  Movsx,
  Lea,
  Add,
  Sub,
  And,
  Or,
  Xor,
  Imul,
  // One-operand unsigned multiply: rdx:rax = rax * r/m, with CF set
  // when the product needs more than one word.
  Umul,
  Idiv,
  Udiv,
  Shl,
  Sar,
  Shr,
  Not,
  Neg,
  Cmp,
  Test,
  Setcc,
  Jmp,
  Jcc,
  Call,
  // An indirect call through the register the operand names.
  CallReg,
  Ret,
  // The system-call instruction, which reads the number in `rax` and
  // answers in it.
  Syscall,
  // One byte of nothing, which is what carries a label the body only
  // falls through to.
  Nop,
  Push,
  Pop,
  Cqo,
  // Copies `rcx` bytes from `rsi` to `rdi`, forward. The registers are
  // set by the instructions before it; the copy runs on registers no
  // value lives in.
  RepMovsb,
  // Fills `rcx` bytes at `rdi` with the byte in `al`.
  RepStosb,
  // Scalar floating-point moves: dst is an XMM register or memory, src
  // is an XMM register, memory, or a symbol naming the bytes.
  Movss,
  Movsd,
  // Ordered scalar arithmetic on two XMM registers.
  Fadd,
  Fsub,
  Fmul,
  Fdiv,
  // Flips the sign bit of an XMM register (dst in place).
  Fneg,
  // Compares two XMM registers and sets the flags for a later `setcc`.
  Fcom,
  // Conversions between the integer and floating-point files.
  CvtI2F,
  CvtF2I,
  CvtF2F,
  Ud2,
};

// One machine instruction. Operands sit in encoding order, destination
// first, the way the two-operand forms read; `count` says how many are
// meaningful. `width` is the operand width in bytes, 1 to 8, and is what
// picks the encoding, not a per-opcode constant.
struct MInst {
  MOp op = MOp::Ret;
  std::array<Operand, 3> ops{};
  u8 count = 0;
  u8 width = 8;
  Cond cond = Cond::E;
  // The label this instruction opens, when a branch names it. One label
  // per instruction at most, and the instruction that carries it is the
  // first byte of the block.
  static constexpr u32 NO_LABEL = 0xffffffff;
  u32 label = NO_LABEL;

  Operand& at(const u32 index) { return ops[index]; }
  const Operand& at(const u32 index) const { return ops[index]; }
};

inline MInst make_inst(const MOp op, const u8 width) {
  MInst inst;
  inst.op = op;
  inst.width = width;
  return inst;
}

inline MInst make_inst(const MOp op, const u8 width, Operand a) {
  MInst inst;
  inst.op = op;
  inst.width = width;
  inst.ops[0] = a;
  inst.count = 1;
  return inst;
}

inline MInst make_inst(const MOp op, const u8 width, Operand a, Operand b) {
  MInst inst;
  inst.op = op;
  inst.width = width;
  inst.ops[0] = a;
  inst.ops[1] = b;
  inst.count = 2;
  return inst;
}

inline MInst make_inst(const MOp op,
                       const u8 width,
                       Operand a,
                       Operand b,
                       Operand c) {
  MInst inst;
  inst.op = op;
  inst.width = width;
  inst.ops[0] = a;
  inst.ops[1] = b;
  inst.ops[2] = c;
  inst.count = 3;
  return inst;
}

inline MInst make_jcc(const Cond cond, Operand target) {
  MInst inst;
  inst.op = MOp::Jcc;
  inst.width = 8;
  inst.cond = cond;
  inst.ops[0] = target;
  inst.count = 1;
  return inst;
}

inline MInst make_setcc(const Cond cond, Operand dst) {
  MInst inst;
  inst.op = MOp::Setcc;
  inst.width = 1;
  inst.cond = cond;
  inst.ops[0] = dst;
  inst.count = 1;
  return inst;
}

// A body's text, as `select` produced it: the instructions in order, the
// blocks named by label, and the tables the later stages read. The types
// ride with the vregs because allocation sizes slots from them.
struct Body {
  std::vector<MInst> insts;
  // One entry per vreg: the IR type it holds.
  std::vector<u32> vreg_types;
  // The label each IR block was given, by block index. Block indexes
  // are the module's, so a map is what keeps a body's labels dense; a
  // block the body never branches to still gets one.
  std::unordered_map<u32, u32> block_labels;
  u32 label_count = 0;
  // The names `SymbolRef` indexes: callees, runtime entries, data.
  std::vector<std::string> symbols;
  // Frame slots an `alloca` asks for, in request order: the vreg that
  // holds the address, the byte count, and the alignment.
  struct Alloca {
    u32 vreg = 0;
    u64 size = 0;
    u64 align = 1;
  };
  std::vector<Alloca> allocas;
  // The stack bytes a call may need for arguments past the registers.
  u32 outgoing = 0;
  // A call whose site a backtrace can name: the label on the marker
  // instruction before the call, and the rendered file:line:column.
  struct CallSite {
    u32 label = 0;
    std::string site;
  };
  std::vector<CallSite> call_sites;
};

// Renders a body as text, one instruction per line, in the spelling the
// disassembler's reader would write. This is what a lowering bug is read
// from, so it is part of the type rather than a test's private helper.
[[nodiscard]] std::string dump(const Body& body);

}  // namespace codegen::x86_64
