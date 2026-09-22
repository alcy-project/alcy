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

  OperandIdxRange operands;
};

}  // namespace ir
