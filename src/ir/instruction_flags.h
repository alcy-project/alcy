// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

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
