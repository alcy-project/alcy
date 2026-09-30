// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "ir/common.h"
#include "ir/instruction_flags.h"
#include "ir/opcode.h"

namespace ir {

struct Instruction {
  Opcode op;
  InstructionFlags flags;

  RegisterIdx dst;

  // Type an opcode measures rather than takes as an operand, as
  // `TypeSizeOf` and `TypeAlignOf` do. Invalid otherwise.
  TypeIdx measure;

  OperandIdxRange operands;
};

}  // namespace ir
