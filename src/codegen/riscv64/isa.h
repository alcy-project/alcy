// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace codegen::riscv64 {

constexpr u8 GPR_COUNT = 32;

// RISC-V immediate arithmetic, shared by the selector, allocator and
// backend. `upper20` rounds toward zero in the sign-extended sense, so
// `lower12` always fits a 12-bit addi.
constexpr bool fits_i12(const i64 value) {
  return value >= -2048 && value <= 2047;
}
constexpr bool fits_s21(const i64 value) {
  return value >= -1048576 && value <= 1048575;
}
constexpr i64 upper20(const i64 value) {
  return value >= 0 ? (value + 2048) / 4096 : -((-value + 2047) / 4096);
}
constexpr i64 lower12(const i64 value) {
  return value - upper20(value) * 4096;
}
constexpr i64 upper20_shifted(const i64 value) {
  return upper20(value) << 12;
}

// One register file in numeric order: x0-x31 then f0-f31. The ABI names
// below are aliases; the encoder only reads the number.
enum class Reg : u8 {
  X0 = 0,
  X1,
  X2,
  X3,
  X4,
  X5,
  X6,
  X7,
  X8,
  X9,
  X10,
  X11,
  X12,
  X13,
  X14,
  X15,
  X16,
  X17,
  X18,
  X19,
  X20,
  X21,
  X22,
  X23,
  X24,
  X25,
  X26,
  X27,
  X28,
  X29,
  X30,
  X31,
  F0 = 32,
  F1,
  F2,
  F3,
  F4,
  F5,
  F6,
  F7,
  F8,
  F9,
  F10,
  F11,
  F12,
  F13,
  F14,
  F15,
  F16,
  F17,
  F18,
  F19,
  F20,
  F21,
  F22,
  F23,
  F24,
  F25,
  F26,
  F27,
  F28,
  F29,
  F30,
  F31,
};

constexpr bool is_fpr(const Reg reg) {
  return static_cast<u8>(reg) >= GPR_COUNT;
}

constexpr u8 reg_code(const Reg reg) {
  return static_cast<u8>(reg) % GPR_COUNT;
}

// The integer ABI. a0-a7 carry arguments and a0/a1 the answer; s0-s11
// survive a call; t0-t6 are the scratch the allocator borrows.
constexpr Reg ZERO = Reg::X0;
constexpr Reg RA = Reg::X1;
constexpr Reg SP = Reg::X2;
constexpr Reg GP = Reg::X3;
constexpr Reg TP = Reg::X4;
constexpr Reg T0 = Reg::X5;
constexpr Reg T1 = Reg::X6;
constexpr Reg T2 = Reg::X7;
constexpr Reg S0 = Reg::X8;
constexpr Reg S1 = Reg::X9;
constexpr Reg A0 = Reg::X10;
constexpr Reg A1 = Reg::X11;
constexpr Reg A2 = Reg::X12;
constexpr Reg A3 = Reg::X13;
constexpr Reg A4 = Reg::X14;
constexpr Reg A5 = Reg::X15;
constexpr Reg A6 = Reg::X16;
constexpr Reg A7 = Reg::X17;
constexpr Reg S2 = Reg::X18;
constexpr Reg S11 = Reg::X27;
constexpr Reg T3 = Reg::X28;
constexpr Reg T6 = Reg::X31;

constexpr Reg ARG_REGS[] = {A0, A1, A2, A3, A4, A5, A6, A7};
constexpr u8 ARG_REG_COUNT = 8;

constexpr Reg FA0 = Reg::F10;
constexpr Reg FA7 = Reg::F17;
constexpr Reg FS0 = Reg::F8;
constexpr Reg FS11 = Reg::F27;
constexpr Reg ARG_FREGS[] = {FA0,      Reg::F11, Reg::F12, Reg::F13,
                             Reg::F14, Reg::F15, Reg::F16, FA7};
constexpr u8 ARG_FREG_COUNT = 8;

}  // namespace codegen::riscv64
