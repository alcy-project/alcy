// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "codegen/riscv64/minst.h"
#include "fpag/base/numeric.h"

namespace codegen::riscv64 {

struct Frame {
  u32 size = 0;
  std::vector<i32> vreg_disp;
  std::vector<u32> vreg_size;
  std::vector<bool> vreg_signed;
};

[[nodiscard]] Frame layout_frame(const Body& body);

}  // namespace codegen::riscv64
