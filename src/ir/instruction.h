// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "fpag/base/numeric.h"
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
