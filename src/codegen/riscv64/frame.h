// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "codegen/riscv64/isa.h"
#include "codegen/riscv64/minst.h"
#include "fpag/base/numeric.h"

namespace codegen::riscv64 {

// The frame pointer names the slot holding the caller's frame pointer;
// the word above it holds the return address. Both the prologue and the
// panic-time walk read these offsets, so they live here.
constexpr Reg SAVED_REGS[] = {RA,       S1,       S0,       Reg::X18, Reg::X19,
                              Reg::X20, Reg::X21, Reg::X22, Reg::X23, Reg::X24,
                              Reg::X25, Reg::X26, Reg::X27};
constexpr u32 SAVE_AREA_BYTES = sizeof(SAVED_REGS) * 8;
constexpr i64 PREV_FP_DISP = 0;
constexpr i64 RETURN_ADDR_DISP = 8;

constexpr i64 save_disp(const u32 size, const u32 index) {
  return static_cast<i64>(size) - 8 * (static_cast<i64>(index) + 1);
}

struct Frame {
  u32 size = 0;
  std::vector<i32> vreg_disp;
  std::vector<u32> vreg_size;
  std::vector<bool> vreg_signed;
};

[[nodiscard]] Frame layout_frame(const Body& body);

}  // namespace codegen::riscv64
