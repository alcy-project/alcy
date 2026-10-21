// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "opt/opt.h"

#include <utility>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/verifier.h"

namespace opt {

namespace {

// `fn f() -> i32 { ret (1 + 2) * 3 }`, whose whole body folds to 9.
ir::VerifiedStorage foldable_body() {
  ir::StorageBuilder builder;
  const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
  const ir::ImmutableIdx one =
      builder.immutable({.type = i32, .data = {.i32_value = 1}});
  const ir::ImmutableIdx two =
      builder.immutable({.type = i32, .data = {.i32_value = 2}});
  const ir::ImmutableIdx three =
      builder.immutable({.type = i32, .data = {.i32_value = 3}});

  ir::OperandSeq add_args;
  add_args.push(builder.operand(ir::Operand::from_immutable(one, i32)));
  add_args.push(builder.operand(ir::Operand::from_immutable(two, i32)));
  const ir::InstructionIdx add = builder.instr({
      .op = ir::Opcode::IntAdd,
      .flags = {},
      .dst = ir::RegisterIdx(0),
      .measure = ir::TypeIdx::invalid(),
      .operands = add_args.finish(),
  });
  builder.reg({.type = i32, .def_idx = add});

  ir::OperandSeq mul_args;
  mul_args.push(
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(0), i32)));
  mul_args.push(builder.operand(ir::Operand::from_immutable(three, i32)));
  const ir::InstructionIdx mul = builder.instr({
      .op = ir::Opcode::IntMul,
      .flags = {},
      .dst = ir::RegisterIdx(1),
      .measure = ir::TypeIdx::invalid(),
      .operands = mul_args.finish(),
  });
  builder.reg({.type = i32, .def_idx = mul});

  ir::OperandSeq ret_args;
  ret_args.push(
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(1), i32)));
  const ir::InstructionIdx ret = builder.instr({
      .op = ir::Opcode::Ret,
      .flags = {},
      .dst = ir::RegisterIdx(base::INVALID_IDX),
      .measure = ir::TypeIdx::invalid(),
      .operands = ret_args.finish(),
  });

  ir::InstrSeq instrs;
  instrs.push(add);
  instrs.push(mul);
  instrs.push(ret);
  const ir::BlockIdx block =
      builder.block({.instrs = instrs.finish(), .block_params = {}});
  builder.function({
      .meta = {.return_type = i32,
               .param_types = {},
               .name = str::EMPTY_STRING_ID,
               .path = str::EMPTY_STRING_ID,
               .kind = ir::SymbolKind::Free,
               .generics = ir::TypeIdxRange{}},
      .blocks = {block, 1},
  });
  return std::move(builder).build().unwrap();
}

}  // namespace

TEST_CASE("Folding propagates a constant through every use") {
  base::Result<ir::VerifiedStorage, ir::VerificationError> optimized =
      optimize(foldable_body());
  CHECK(optimized.is_ok());
  if (optimized.is_err()) {
    return;
  }
  ir::VerifiedStorage storage = std::move(optimized).unwrap();
  // The structure is unchanged: folding rewrites values, it does not
  // renumber instructions, so spans stay parallel.
  CHECK(storage->instrs().size() == 3);
  // The add and mul folded, their uses took the constants, and nothing
  // read their registers any more, so both are pruned.
  CHECK(storage->instrs()[ir::InstructionIdx(0)].op == ir::Opcode::Noop);
  CHECK(storage->instrs()[ir::InstructionIdx(1)].op == ir::Opcode::Noop);
  const ir::Instruction& ret = storage->instrs()[ir::InstructionIdx(2)];
  CHECK(ret.op == ir::Opcode::Ret);
  const ir::Operand& returned = storage->operands()[ret.operands.head()];
  CHECK(returned.is<ir::ImmutableIdx>());
  if (returned.is<ir::ImmutableIdx>()) {
    CHECK(storage->immutables()[returned.as_immutable()].as_u64_integer(
              ir::TypeTag::I32) == 9);
  }
}

TEST_CASE("A constant zero divisor is not folded") {
  // Folding it would delete the panic the language promises; the
  // instruction has to survive to the runtime.
  ir::StorageBuilder builder;
  const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
  const ir::ImmutableIdx four =
      builder.immutable({.type = i32, .data = {.i32_value = 4}});
  const ir::ImmutableIdx zero =
      builder.immutable({.type = i32, .data = {.i32_value = 0}});
  ir::OperandSeq args;
  args.push(builder.operand(ir::Operand::from_immutable(four, i32)));
  args.push(builder.operand(ir::Operand::from_immutable(zero, i32)));
  const ir::InstructionIdx div = builder.instr({
      .op = ir::Opcode::IntDiv,
      .flags = {},
      .dst = ir::RegisterIdx(0),
      .measure = ir::TypeIdx::invalid(),
      .operands = args.finish(),
  });
  builder.reg({.type = i32, .def_idx = div});
  ir::OperandSeq ret_args;
  ret_args.push(
      builder.operand(ir::Operand::from_register(ir::RegisterIdx(0), i32)));
  const ir::InstructionIdx ret = builder.instr({
      .op = ir::Opcode::Ret,
      .flags = {},
      .dst = ir::RegisterIdx(base::INVALID_IDX),
      .measure = ir::TypeIdx::invalid(),
      .operands = ret_args.finish(),
  });
  ir::InstrSeq instrs;
  instrs.push(div);
  instrs.push(ret);
  const ir::BlockIdx block =
      builder.block({.instrs = instrs.finish(), .block_params = {}});
  builder.function({
      .meta = {.return_type = i32,
               .param_types = {},
               .name = str::EMPTY_STRING_ID,
               .path = str::EMPTY_STRING_ID,
               .kind = ir::SymbolKind::Free,
               .generics = ir::TypeIdxRange{}},
      .blocks = {block, 1},
  });
  base::Result<ir::VerifiedStorage, ir::VerificationError> optimized =
      optimize(std::move(builder).build().unwrap());
  CHECK(optimized.is_ok());
  if (optimized.is_err()) {
    return;
  }
  ir::VerifiedStorage storage = std::move(optimized).unwrap();
  const ir::Instruction& first = storage->instrs()[ir::InstructionIdx(0)];
  CHECK(first.op == ir::Opcode::IntDiv);
}

}  // namespace opt
