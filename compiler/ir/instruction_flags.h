// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace ir {

enum class AtomicRmwOp : u8 {
  Add,
  Sub,
  And,
  Or,
  Xor,
  Exchange,
};

// Packed into every Instruction, so the size is asserted in
// compiler/tests/ir/ir_common_test.cc.
struct InstructionFlags {
  // Meaningful only for AtomicRmw.
  AtomicRmwOp rmw_op : 3 = AtomicRmwOp::Add;
  bool some_flag : 1 = false;
};

}  // namespace ir
