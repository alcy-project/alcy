// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include <cstddef>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/instruction_flags.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/register.h"
// #include "ir/storage.h"
// #include "ir/storage_builder.h"
#include "ir/type.h"

namespace ir {

TEST_CASE("Static assertion for IR elements") {
  static_assert(sizeof(Block) == 16);
  static_assert(sizeof(BlockParam) == 8);

  static_assert(sizeof(ExternalFunction) == 40);
  static_assert(sizeof(Function) == 40);
  static_assert(sizeof(FunctionMeta) == 32);

  static_assert(sizeof(Immutable) == 24);
  static_assert(sizeof(Instruction) == 16);
  static_assert(sizeof(InstructionFlags) == 1);
  static_assert(sizeof(Opcode) == 1);
  static_assert(sizeof(Operand) == 8);
  static_assert(alignof(Operand) == alignof(u32));
  static_assert(offsetof(Operand, data) == 0);
  static_assert(sizeof(Register) == 8);
  static_assert(sizeof(Type) == 1);

  // static_assert(sizeof(StorageState) == 216);
  // static_assert(sizeof(Storage) == 216);
  // static_assert(sizeof(StorageBuilder) == 216);
}

}  // namespace ir

