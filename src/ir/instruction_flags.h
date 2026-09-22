// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace ir {

enum class AtomicRmwOp : u8 {
  Add,
  Sub,
  And,
  Or,
  Xor,
  Exchange,
};

// Bit packed 1 B struct for instruction.
struct InstructionFlags {
  // Meaningful only for AtomicRmw.
  AtomicRmwOp rmw_op : 3 = AtomicRmwOp::Add;
  bool some_flag : 1 = false;
};

}  // namespace ir
