// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace codegen::x86_64 {

// The machine this backend emits for. The register file is one list so a
// register's identity is its index, and the two classes the ABI names
// (general purpose and vector) read apart by range.
enum class Reg : u8 {
  Rax,
  Rcx,
  Rdx,
  Rbx,
  Rsp,
  Rbp,
  Rsi,
  Rdi,
  R8,
  R9,
  R10,
  R11,
  R12,
  R13,
  R14,
  R15,
  Xmm0,
  Xmm1,
  Xmm2,
  Xmm3,
  Xmm4,
  Xmm5,
  Xmm6,
  Xmm7,
  Xmm8,
  Xmm9,
  Xmm10,
  Xmm11,
  Xmm12,
  Xmm13,
  Xmm14,
  Xmm15,
};

constexpr u8 GPR_COUNT = 16;

constexpr bool is_xmm(Reg reg) {
  return static_cast<u8>(reg) >= GPR_COUNT;
}

// The registers a call may clobber. Every vector register is
// caller-saved on this ABI; the frame's own two are never operands.
constexpr bool is_caller_saved(Reg reg) {
  switch (reg) {
    case Reg::Rax:
    case Reg::Rcx:
    case Reg::Rdx:
    case Reg::Rsi:
    case Reg::Rdi:
    case Reg::R8:
    case Reg::R9:
    case Reg::R10:
    case Reg::R11: return true;
    default: return is_xmm(reg);
  }
}

// Where the ABI passes and answers. Aggregates cross by address instead,
// so these carry scalars and the words of a two-word value.
constexpr Reg ARG_REGS[] = {Reg::Rdi, Reg::Rsi, Reg::Rdx,
                            Reg::Rcx, Reg::R8,  Reg::R9};
constexpr Reg ARG_XMM_REGS[] = {Reg::Xmm0, Reg::Xmm1, Reg::Xmm2, Reg::Xmm3,
                                Reg::Xmm4, Reg::Xmm5, Reg::Xmm6, Reg::Xmm7};
constexpr u8 ARG_XMM_COUNT = 8;
constexpr Reg RETURN_REG = Reg::Rax;
constexpr Reg RETURN_XMM_REG = Reg::Xmm0;

// The registers allocation borrows to bring an instruction its operands.
// Values live in frame slots, so a scratch is free again by the next
// instruction; the pair is what a two-operand form needs at most.
constexpr Reg SCRATCH_A = Reg::Rax;
constexpr Reg SCRATCH_B = Reg::Rcx;

// The x86-64 SysV integer argument registers, in call order.
constexpr u8 ARG_REG_COUNT = 6;

}  // namespace codegen::x86_64
