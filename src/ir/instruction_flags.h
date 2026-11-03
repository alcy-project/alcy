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
  // Meaningful only for Move: the move was emitted for a value whose
  // type parameter had not settled `Copy`-ness, so the borrow pass
  // treats it as a copy when the substituted type is `Copy` (ADR-0079).
  bool maybe_copy : 1 = false;
  // Meaningful only for ParametricDrop: the end also marks the place
  // moved, which a binding's end does and an explicit one does not.
  bool end_moved : 1 = false;
};

}  // namespace ir
