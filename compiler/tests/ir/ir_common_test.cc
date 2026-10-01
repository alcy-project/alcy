// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
#include "ir/seq_builder.h"

namespace ir {

// StringPoolId is a fixed 4-byte id, so sizes below are host-independent.
// A type carrying N of them loses 4N bytes when the id shrinks.
TEST_CASE("Static assertion for IR elements") {
  static_assert(sizeof(Block) == 16);
  static_assert(sizeof(BlockParam) == 8);

  static_assert(sizeof(ExternalFunction) == 36);
  static_assert(sizeof(Function) == 40);
  static_assert(sizeof(FunctionMeta) == 32);

  static_assert(sizeof(Immutable) == 16);
  static_assert(sizeof(Instruction) == 20);
  static_assert(sizeof(InstructionFlags) == 1);
  static_assert(sizeof(Opcode) == 1);
  static_assert(sizeof(Operand) == 12);
  static_assert(alignof(Operand) == alignof(u32));
  static_assert(offsetof(Operand, data) == 0);
  static_assert(sizeof(Register) == 8);
  static_assert(sizeof(TypeTag) == 1);
  static_assert(sizeof(TypeNode) == 8);
  static_assert(sizeof(StructType) == 20);
}

TEST_CASE("SeqBuilder accumulates consecutive indexes") {
  OperandSeq operands;
  CHECK(operands.empty());
  CHECK(operands.size() == 0);

  operands.push(OperandIdx(4));
  operands.push(OperandIdx(5));
  CHECK(operands.size() == 2);

  const OperandIdxRange range = operands.finish();
  CHECK(range.head() == OperandIdx(4));
  CHECK(range.size() == 2);

  const BlockIdxRange empty = BlockSeq().finish();
  CHECK(empty.head() == BlockIdx(0));
  CHECK(empty.size() == 0);
}

}  // namespace ir

