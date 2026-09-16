// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "ir/verifier.h"

#include <utility>

#include "doctest/doctest.h"
#include "fpag/base/idx.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace ir {

namespace {

// Builds a minimal valid storage: one function with a single block
// [Alloca r0, Ret r0].
Storage valid_storage() {
  StorageBuilder builder;

  const ImmutableIdx imm =
      builder.immutable({.type = Type::I32, .data = {.i32_value = 1}});
  const OperandIdx alloc_arg =
      builder.operand(Operand::from_immutable(imm, Type::I32));

  const RegisterIdx ret_reg(0);
  const InstructionIdx inst_alloc = builder.instr({
      .op = Opcode::Alloca,
      .flags = {},
      .dst = ret_reg,
      .operands = {alloc_arg, 1},
  });
  builder.reg({.type = Type::I32, .def_idx = inst_alloc});

  const OperandIdx ret_arg =
      builder.operand(Operand::from_register(ret_reg, Type::I32));
  const InstructionIdx inst_ret = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .operands = {ret_arg, 1},
  });

  const BlockIdx block = builder.block({
      .instrs = InstructionIdxRange::from_to(inst_alloc, inst_ret),
      .block_params = {},
  });
  builder.function({
      .meta = {.return_type = Type::I32,
               .param_types = {},
               .name = str::kEmptyStringId},
      .blocks = {block, 1},
  });
  return std::move(builder).build();
}

FunctionMeta void_meta() {
  return FunctionMeta{.return_type = Type::Void,
                      .param_types = {},
                      .name = str::kEmptyStringId};
}

VerifyErrorKind check(Storage&& storage) {
  VerifyResult result = verify_storage(storage);
  CHECK(result.is_err());
  return std::move(result).unwrap_err().kind;
}

}  // namespace

TEST_CASE("Verify valid storage") {
  Storage storage = valid_storage();
  CHECK(verify_storage(storage).is_ok());
}

TEST_CASE("Verify unterminated block") {
  StorageBuilder builder;
  const ImmutableIdx imm =
      builder.immutable({.type = Type::I32, .data = {.i32_value = 1}});
  const OperandIdx arg =
      builder.operand(Operand::from_immutable(imm, Type::I32));
  const InstructionIdx inst = builder.instr({
      .op = Opcode::Alloca,
      .flags = {},
      .dst = RegisterIdx(0),
      .operands = {arg, 1},
  });
  builder.reg({.type = Type::I32, .def_idx = inst});
  const BlockIdx block = builder.block({
      .instrs = {inst, 1},
      .block_params = {},
  });
  builder.function({
      .meta = void_meta(),
      .blocks = {block, 1},
  });
  CHECK(check(std::move(builder).build()) ==
        VerifyErrorKind::UnterminatedBlock);
}

TEST_CASE("Verify misplaced terminator") {
  StorageBuilder builder;
  const ImmutableIdx imm =
      builder.immutable({.type = Type::I32, .data = {.i32_value = 1}});
  const OperandIdx alloc_arg =
      builder.operand(Operand::from_immutable(imm, Type::I32));
  const InstructionIdx inst_alloc = builder.instr({
      .op = Opcode::Alloca,
      .flags = {},
      .dst = RegisterIdx(0),
      .operands = {alloc_arg, 1},
  });
  builder.reg({.type = Type::I32, .def_idx = inst_alloc});
  const OperandIdx ret_arg =
      builder.operand(Operand::from_register(RegisterIdx(0), Type::I32));
  const InstructionIdx inst_ret1 = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .operands = {ret_arg, 1},
  });
  const OperandIdx ret_arg2 =
      builder.operand(Operand::from_register(RegisterIdx(0), Type::I32));
  const InstructionIdx inst_ret2 = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .operands = {ret_arg2, 1},
  });
  const BlockIdx block = builder.block({
      .instrs = InstructionIdxRange::from_to(inst_alloc, inst_ret2),
      .block_params = {},
  });
  builder.function({
      .meta = void_meta(),
      .blocks = {block, 1},
  });
  CHECK(check(std::move(builder).build()) ==
        VerifyErrorKind::MisplacedTerminator);
  (void)inst_ret1;
}

TEST_CASE("Verify unknown operand tag") {
  StorageBuilder builder;
  Operand unknown{};
  unknown.tag = OperandTag::Unknown;
  unknown.type = Type::Void;
  const OperandIdx oidx = builder.operand(unknown);
  const InstructionIdx inst = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .operands = {oidx, 1},
  });
  const BlockIdx block = builder.block({
      .instrs = {inst, 1},
      .block_params = {},
  });
  builder.function({
      .meta = void_meta(),
      .blocks = {block, 1},
  });
  CHECK(check(std::move(builder).build()) ==
        VerifyErrorKind::UnknownOperandTag);
}

TEST_CASE("Verify undefined register") {
  StorageBuilder builder;
  // Register 1 exists but is never defined or bound.
  builder.reg(
      {.type = Type::I32, .def_idx = InstructionIdx(base::kInvalidIdx)});
  builder.reg(
      {.type = Type::I32, .def_idx = InstructionIdx(base::kInvalidIdx)});
  const OperandIdx arg =
      builder.operand(Operand::from_register(RegisterIdx(1), Type::I32));
  const InstructionIdx inst = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .operands = {arg, 1},
  });
  const BlockIdx block = builder.block({
      .instrs = {inst, 1},
      .block_params = {},
  });
  builder.function({
      .meta = {.return_type = Type::I32,
               .param_types = {},
               .name = str::kEmptyStringId},
      .blocks = {block, 1},
  });
  CHECK(check(std::move(builder).build()) ==
        VerifyErrorKind::UndefinedRegister);
}

TEST_CASE("Verify redefined register") {
  StorageBuilder builder;
  const ImmutableIdx imm =
      builder.immutable({.type = Type::I32, .data = {.i32_value = 1}});
  const OperandIdx arg =
      builder.operand(Operand::from_immutable(imm, Type::I32));
  const InstructionIdx inst1 = builder.instr({
      .op = Opcode::Alloca,
      .flags = {},
      .dst = RegisterIdx(0),
      .operands = {arg, 1},
  });
  const OperandIdx arg2 =
      builder.operand(Operand::from_immutable(imm, Type::I32));
  const InstructionIdx inst2 = builder.instr({
      .op = Opcode::Alloca,
      .flags = {},
      .dst = RegisterIdx(0),
      .operands = {arg2, 1},
  });
  builder.reg({.type = Type::I32, .def_idx = inst1});
  const OperandIdx ret_arg =
      builder.operand(Operand::from_register(RegisterIdx(0), Type::I32));
  const InstructionIdx inst_ret = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .operands = {ret_arg, 1},
  });
  const BlockIdx block = builder.block({
      .instrs = InstructionIdxRange::from_to(inst1, inst_ret),
      .block_params = {},
  });
  builder.function({
      .meta = void_meta(),
      .blocks = {block, 1},
  });
  CHECK(check(std::move(builder).build()) ==
        VerifyErrorKind::RedefinedRegister);
  (void)inst2;
}

TEST_CASE("Verify invalid callee") {
  StorageBuilder builder;
  const ImmutableIdx imm =
      builder.immutable({.type = Type::I32, .data = {.i32_value = 0}});
  const OperandIdx head =
      builder.operand(Operand::from_immutable(imm, Type::I32));
  const InstructionIdx inst = builder.instr({
      .op = Opcode::Call,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .operands = {head, 1},
  });
  const OperandIdx ret_arg =
      builder.operand(Operand::from_immutable(imm, Type::I32));
  const InstructionIdx inst_ret = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .operands = {ret_arg, 1},
  });
  const BlockIdx block = builder.block({
      .instrs = InstructionIdxRange::from_to(inst, inst_ret),
      .block_params = {},
  });
  builder.function({
      .meta = void_meta(),
      .blocks = {block, 1},
  });
  CHECK(check(std::move(builder).build()) == VerifyErrorKind::InvalidCallee);
}

TEST_CASE("Verify invalid branch target") {
  StorageBuilder builder;
  // Bind register 0 via a block param so the operand itself is valid.
  builder.reg(
      {.type = Type::I32, .def_idx = InstructionIdx(base::kInvalidIdx)});
  const BlockParamIdx param = builder.block_param({
      .type = Type::I32,
      .reg = RegisterIdx(0),
  });
  const OperandIdx head =
      builder.operand(Operand::from_register(RegisterIdx(0), Type::I32));
  const InstructionIdx inst = builder.instr({
      .op = Opcode::Br,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .operands = {head, 1},
  });
  const BlockIdx block = builder.block({
      .instrs = {inst, 1},
      .block_params = {param, 1},
  });
  builder.function({
      .meta = void_meta(),
      .blocks = {block, 1},
  });
  CHECK(check(std::move(builder).build()) ==
        VerifyErrorKind::InvalidBranchTarget);
}

TEST_CASE("Verify out of range indexes") {
  // Instruction operands range exceeds the operands storage.
  {
    StorageBuilder builder;
    const OperandIdx arg =
        builder.operand(Operand::from_register(RegisterIdx(0), Type::I32));
    const InstructionIdx inst = builder.instr({
        .op = Opcode::Ret,
        .flags = {},
        .dst = RegisterIdx(base::kInvalidIdx),
        .operands = {arg, 4},
    });
    const BlockIdx block = builder.block({
        .instrs = {inst, 1},
        .block_params = {},
    });
    builder.function({
        .meta = void_meta(),
        .blocks = {block, 1},
    });
    CHECK(check(std::move(builder).build()) ==
          VerifyErrorKind::InstrOperandsOutOfRange);
  }
  // Instruction dst exceeds the registers storage.
  {
    StorageBuilder builder;
    const ImmutableIdx imm =
        builder.immutable({.type = Type::I32, .data = {.i32_value = 1}});
    const OperandIdx arg =
        builder.operand(Operand::from_immutable(imm, Type::I32));
    const InstructionIdx inst = builder.instr({
        .op = Opcode::Alloca,
        .flags = {},
        .dst = RegisterIdx(7),
        .operands = {arg, 1},
    });
    builder.reg({.type = Type::I32, .def_idx = inst});
    const BlockIdx block = builder.block({
        .instrs = {inst, 1},
        .block_params = {},
    });
    builder.function({
        .meta = void_meta(),
        .blocks = {block, 1},
    });
    // The block has no terminator, but dst bounds are checked first.
    CHECK(check(std::move(builder).build()) ==
          VerifyErrorKind::InstrDstOutOfRange);
  }
}

}  // namespace ir
