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
#include "ir/type.h"

namespace ir {

// StringPoolId is a fixed 4-byte id, so sizes below are host-independent.
// A type carrying N of them loses 4N bytes when the id shrinks.
TEST_CASE("Static assertion for IR elements") {
  static_assert(sizeof(Block) == 16);
  static_assert(sizeof(BlockParam) == 8);

  static_assert(sizeof(ExternalFunction) == 44);
  static_assert(sizeof(InlineKind) == 1);
  static_assert(sizeof(Function) == 48);
  static_assert(sizeof(FunctionMeta) == 40);

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
  static_assert(sizeof(StructType) == 32);
}

TEST_CASE("Immutable reads its payload by type tag") {
  CHECK(Immutable{TypeIdx(0), {.i1_value = true}}.as_u64_bits(TypeTag::I1) ==
        1);
  CHECK(Immutable{TypeIdx(0), {.i8_value = -2}}.as_u64_bits(TypeTag::I8) ==
        0xFFFFFFFFFFFFFFFEull);
  CHECK(Immutable{TypeIdx(0), {.i16_value = -2}}.as_u64_bits(TypeTag::I16) ==
        0xFFFFFFFFFFFFFFFEull);
  CHECK(Immutable{TypeIdx(0), {.i32_value = -2}}.as_u64_bits(TypeTag::I32) ==
        0xFFFFFFFFFFFFFFFEull);
  CHECK(Immutable{TypeIdx(0), {.i64_value = -2}}.as_u64_bits(TypeTag::I64) ==
        0xFFFFFFFFFFFFFFFEull);
  CHECK(Immutable{TypeIdx(0), {.u8_value = 0xF0}}.as_u64_bits(TypeTag::U8) ==
        0xF0);
  CHECK(Immutable{TypeIdx(0), {.u8_value = 0xF0}}.as_u64_bits(TypeTag::Char) ==
        0xF0);
  CHECK(Immutable{TypeIdx(0), {.u32_value = 0x1F600}}.as_u64_bits(
            TypeTag::Rune) == 0x1F600);
  CHECK(Immutable{TypeIdx(0), {.u32_value = 0xF1234567}}.as_u64_bits(
            TypeTag::U32) == 0xF1234567);
  CHECK(Immutable{TypeIdx(0), {.f32_value = -1.0f}}.as_u64_bits(TypeTag::F32) ==
        0xBF800000);
  CHECK(Immutable{TypeIdx(0), {.f64_value = -0.0}}.as_u64_bits(TypeTag::F64) ==
        0x8000000000000000ull);
  CHECK(Immutable{TypeIdx(0), {.f32_value = -1.0f}}.as_f64_fp(TypeTag::F32) ==
        -1.0);
  CHECK(Immutable{TypeIdx(0), {.f64_value = 2.5}}.as_f64_fp(TypeTag::F64) ==
        2.5);
}

TEST_CASE("Operand factories keep their tag and payload") {
  const Operand imm = Operand::from_immutable(ImmutableIdx(3), TypeIdx(2));
  CHECK(imm.is<ImmutableIdx>());
  CHECK(imm.as_immutable() == ImmutableIdx(3));
  CHECK(imm.type == TypeIdx(2));

  const Operand reg = Operand::from_register(RegisterIdx(4), TypeIdx(1));
  CHECK(reg.is<RegisterIdx>());
  CHECK(reg.as_register() == RegisterIdx(4));

  const Operand fn = Operand::from_function(FunctionIdx(5), TypeIdx(0));
  CHECK(fn.is<FunctionIdx>());
  CHECK(fn.as_function() == FunctionIdx(5));

  const Operand block = Operand::from_block(BlockIdx(6), TypeIdx(0));
  CHECK(block.is<BlockIdx>());
  CHECK(block.as_block() == BlockIdx(6));

  const Operand invalid = Operand::invalid();
  CHECK(invalid.tag() == Operand::TAG_OF<void>);
  CHECK(invalid.type == primitive_idx(TypeTag::Void));
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

