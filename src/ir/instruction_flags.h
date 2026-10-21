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
// src/tests/ir/ir_common_test.cc.
struct InstructionFlags {
  // Meaningful only for AtomicRmw.
  AtomicRmwOp rmw_op : 3 = AtomicRmwOp::Add;
  // Meaningful only for GetElementPtr and ElemOffset: the address is
  // proved to stay within the object it indexes, which is what lets a
  // backend's alias analysis and folding treat it as such.
  bool inbounds : 1 = false;
  // Meaningful only for Memcopy: log2 of the alignment both pointers
  // promise, zero meaning the copy's byte alignment is all a backend
  // may assume.
  u8 mem_align_log2 : 4 = 0;
};

}  // namespace ir
