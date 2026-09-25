// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/verifier.h"

#include <utility>

#include "diag/diagnostic.h"
#include "doctest/doctest.h"
#include "fpag/base/idx.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace ir {

namespace {

// Builds a minimal valid storage: one function with a single block
// [Alloca r0, Ret r0].
Storage valid_storage() {
  StorageBuilder builder;
  InstrSeq instrs;

  const ImmutableIdx imm = builder.immutable(
      {.type = primitive_idx(TypeTag::I32), .data = {.i32_value = 1}});
  const OperandIdx alloc_arg = builder.operand(
      Operand::from_immutable(imm, primitive_idx(TypeTag::I32)));

  const RegisterIdx ret_reg(0);
  const InstructionIdx inst_alloc = builder.instr({
      .op = Opcode::Alloca,
      .flags = {},
      .dst = ret_reg,
      .measure = ir::TypeIdx::invalid(),
      .operands = {alloc_arg, 1},
  });
  instrs.push(inst_alloc);
  builder.reg({.type = primitive_idx(TypeTag::I32), .def_idx = inst_alloc});

  const OperandIdx ret_arg = builder.operand(
      Operand::from_register(ret_reg, primitive_idx(TypeTag::I32)));
  const InstructionIdx inst_ret = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .measure = ir::TypeIdx::invalid(),
      .operands = {ret_arg, 1},
  });
  instrs.push(inst_ret);

  const BlockIdx block = builder.block({
      .instrs = instrs.finish(),
      .block_params = {},
  });
  builder.function({
      .meta = {.return_type = primitive_idx(TypeTag::I32),
               .param_types = {},
               .name = str::kEmptyStringId,
               .path = str::kEmptyStringId,
               .kind = SymbolKind::Foreign,
               .generics = TypeIdxRange{}},
      .blocks = {block, 1},
  });
  return std::move(builder).build();
}

FunctionMeta void_meta() {
  return FunctionMeta{.return_type = primitive_idx(TypeTag::Void),
                      .param_types = {},
                      .name = str::kEmptyStringId,
                      .path = str::kEmptyStringId,
                      .kind = SymbolKind::Foreign,
                      .generics = TypeIdxRange{}};
}

VerifyErrorKind check(Storage&& storage) {
  VerifyResult result = verify_storage(storage);
  CHECK(result.is_err());
  return std::move(result).unwrap_err().kind;
}

}  // namespace

TEST_CASE("Verify valid storage") {
  const Storage storage = valid_storage();
  CHECK(verify_storage(storage).is_ok());
}

TEST_CASE("Verify unterminated block") {
  StorageBuilder builder;
  const ImmutableIdx imm = builder.immutable(
      {.type = primitive_idx(TypeTag::I32), .data = {.i32_value = 1}});
  const OperandIdx arg = builder.operand(
      Operand::from_immutable(imm, primitive_idx(TypeTag::I32)));
  const InstructionIdx inst = builder.instr({
      .op = Opcode::Alloca,
      .flags = {},
      .dst = RegisterIdx(0),
      .measure = ir::TypeIdx::invalid(),
      .operands = {arg, 1},
  });
  builder.reg({.type = primitive_idx(TypeTag::I32), .def_idx = inst});
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
  InstrSeq instrs;
  const ImmutableIdx imm = builder.immutable(
      {.type = primitive_idx(TypeTag::I32), .data = {.i32_value = 1}});
  const OperandIdx alloc_arg = builder.operand(
      Operand::from_immutable(imm, primitive_idx(TypeTag::I32)));
  const InstructionIdx inst_alloc = builder.instr({
      .op = Opcode::Alloca,
      .flags = {},
      .dst = RegisterIdx(0),
      .measure = ir::TypeIdx::invalid(),
      .operands = {alloc_arg, 1},
  });
  instrs.push(inst_alloc);
  builder.reg({.type = primitive_idx(TypeTag::I32), .def_idx = inst_alloc});
  const OperandIdx ret_arg = builder.operand(
      Operand::from_register(RegisterIdx(0), primitive_idx(TypeTag::I32)));
  const InstructionIdx inst_ret1 = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .measure = ir::TypeIdx::invalid(),
      .operands = {ret_arg, 1},
  });
  instrs.push(inst_ret1);
  const OperandIdx ret_arg2 = builder.operand(
      Operand::from_register(RegisterIdx(0), primitive_idx(TypeTag::I32)));
  const InstructionIdx inst_ret2 = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .measure = ir::TypeIdx::invalid(),
      .operands = {ret_arg2, 1},
  });
  instrs.push(inst_ret2);
  const BlockIdx block = builder.block({
      .instrs = instrs.finish(),
      .block_params = {},
  });
  builder.function({
      .meta = void_meta(),
      .blocks = {block, 1},
  });
  CHECK(check(std::move(builder).build()) ==
        VerifyErrorKind::MisplacedTerminator);
}

TEST_CASE("Verify unknown operand tag") {
  StorageBuilder builder;
  Operand unknown{Operand::Payload{}, primitive_idx(TypeTag::Void)};
  const OperandIdx oidx = builder.operand(std::move(unknown));
  const InstructionIdx inst = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .measure = ir::TypeIdx::invalid(),
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
  builder.reg({.type = primitive_idx(TypeTag::I32),
               .def_idx = InstructionIdx(base::kInvalidIdx)});
  builder.reg({.type = primitive_idx(TypeTag::I32),
               .def_idx = InstructionIdx(base::kInvalidIdx)});
  const OperandIdx arg = builder.operand(
      Operand::from_register(RegisterIdx(1), primitive_idx(TypeTag::I32)));
  const InstructionIdx inst = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .measure = ir::TypeIdx::invalid(),
      .operands = {arg, 1},
  });
  const BlockIdx block = builder.block({
      .instrs = {inst, 1},
      .block_params = {},
  });
  builder.function({
      .meta = {.return_type = primitive_idx(TypeTag::I32),
               .param_types = {},
               .name = str::kEmptyStringId,
               .path = str::kEmptyStringId,
               .kind = SymbolKind::Foreign,
               .generics = TypeIdxRange{}},
      .blocks = {block, 1},
  });
  CHECK(check(std::move(builder).build()) ==
        VerifyErrorKind::UndefinedRegister);
}

TEST_CASE("Verify redefined register") {
  StorageBuilder builder;
  InstrSeq instrs;
  const ImmutableIdx imm = builder.immutable(
      {.type = primitive_idx(TypeTag::I32), .data = {.i32_value = 1}});
  const OperandIdx arg = builder.operand(
      Operand::from_immutable(imm, primitive_idx(TypeTag::I32)));
  const InstructionIdx inst1 = builder.instr({
      .op = Opcode::Alloca,
      .flags = {},
      .dst = RegisterIdx(0),
      .measure = ir::TypeIdx::invalid(),
      .operands = {arg, 1},
  });
  instrs.push(inst1);
  const OperandIdx arg2 = builder.operand(
      Operand::from_immutable(imm, primitive_idx(TypeTag::I32)));
  const InstructionIdx inst2 = builder.instr({
      .op = Opcode::Alloca,
      .flags = {},
      .dst = RegisterIdx(0),
      .measure = ir::TypeIdx::invalid(),
      .operands = {arg2, 1},
  });
  instrs.push(inst2);
  builder.reg({.type = primitive_idx(TypeTag::I32), .def_idx = inst1});
  const OperandIdx ret_arg = builder.operand(
      Operand::from_register(RegisterIdx(0), primitive_idx(TypeTag::I32)));
  const InstructionIdx inst_ret = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .measure = ir::TypeIdx::invalid(),
      .operands = {ret_arg, 1},
  });
  instrs.push(inst_ret);
  const BlockIdx block = builder.block({
      .instrs = instrs.finish(),
      .block_params = {},
  });
  builder.function({
      .meta = void_meta(),
      .blocks = {block, 1},
  });
  CHECK(check(std::move(builder).build()) ==
        VerifyErrorKind::RedefinedRegister);
}

TEST_CASE("Verify invalid callee") {
  StorageBuilder builder;
  InstrSeq instrs;
  const ImmutableIdx imm = builder.immutable(
      {.type = primitive_idx(TypeTag::I32), .data = {.i32_value = 0}});
  const OperandIdx head = builder.operand(
      Operand::from_immutable(imm, primitive_idx(TypeTag::I32)));
  const InstructionIdx inst = builder.instr({
      .op = Opcode::Call,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .measure = ir::TypeIdx::invalid(),
      .operands = {head, 1},
  });
  instrs.push(inst);
  const OperandIdx ret_arg = builder.operand(
      Operand::from_immutable(imm, primitive_idx(TypeTag::I32)));
  const InstructionIdx inst_ret = builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .measure = ir::TypeIdx::invalid(),
      .operands = {ret_arg, 1},
  });
  instrs.push(inst_ret);
  const BlockIdx block = builder.block({
      .instrs = instrs.finish(),
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
  builder.reg({.type = primitive_idx(TypeTag::I32),
               .def_idx = InstructionIdx(base::kInvalidIdx)});
  const BlockParamIdx param = builder.block_param({
      .type = primitive_idx(TypeTag::I32),
      .reg = RegisterIdx(0),
  });
  const OperandIdx head = builder.operand(
      Operand::from_register(RegisterIdx(0), primitive_idx(TypeTag::I32)));
  const InstructionIdx inst = builder.instr({
      .op = Opcode::Br,
      .flags = {},
      .dst = RegisterIdx(base::kInvalidIdx),
      .measure = ir::TypeIdx::invalid(),
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
    const OperandIdx arg = builder.operand(
        Operand::from_register(RegisterIdx(0), primitive_idx(TypeTag::I32)));
    const InstructionIdx inst = builder.instr({
        .op = Opcode::Ret,
        .flags = {},
        .dst = RegisterIdx(base::kInvalidIdx),
        .measure = ir::TypeIdx::invalid(),
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
    const ImmutableIdx imm = builder.immutable(
        {.type = primitive_idx(TypeTag::I32), .data = {.i32_value = 1}});
    const OperandIdx arg = builder.operand(
        Operand::from_immutable(imm, primitive_idx(TypeTag::I32)));
    const InstructionIdx inst = builder.instr({
        .op = Opcode::Alloca,
        .flags = {},
        .dst = RegisterIdx(7),
        .measure = ir::TypeIdx::invalid(),
        .operands = {arg, 1},
    });
    builder.reg({.type = primitive_idx(TypeTag::I32), .def_idx = inst});
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

TEST_CASE("Verify struct and array types") {
  // Valid: struct {i32, i32} and [4 x i32] referenced from param types.
  {
    StorageBuilder builder;
    InstrSeq instrs;
    TypeSeq fields;
    const TypeIdx i32 = builder.primitive(TypeTag::I32);
    fields.push(builder.ref_type(i32));
    fields.push(builder.ref_type(i32));
    const TypeIdx st = builder.struct_type(str::kEmptyStringId, fields.finish(),
                                           ir::TypeIdxRange{});
    const TypeIdx arr = builder.array_type(i32, 4);
    TypeSeq params;
    params.push(builder.ref_type(st));
    params.push(builder.ref_type(arr));

    const ImmutableIdx one =
        builder.immutable({.type = i32, .data = {.i32_value = 1}});
    const OperandIdx alloc_arg =
        builder.operand(Operand::from_immutable(one, i32));
    const InstructionIdx inst_alloc = builder.instr({
        .op = Opcode::Alloca,
        .flags = {},
        .dst = RegisterIdx(0),
        .measure = ir::TypeIdx::invalid(),
        .operands = {alloc_arg, 1},
    });
    instrs.push(inst_alloc);
    builder.reg({.type = i32, .def_idx = inst_alloc});

    const OperandIdx ret_arg =
        builder.operand(Operand::from_immutable(one, i32));
    const InstructionIdx inst_ret = builder.instr({
        .op = Opcode::Ret,
        .flags = {},
        .dst = RegisterIdx(base::kInvalidIdx),
        .measure = ir::TypeIdx::invalid(),
        .operands = {ret_arg, 1},
    });
    instrs.push(inst_ret);
    const BlockIdx block = builder.block({
        .instrs = instrs.finish(),
        .block_params = {},
    });
    builder.function({
        .meta = {.return_type = i32,
                 .param_types = params.finish(),
                 .name = str::kEmptyStringId,
                 .path = str::kEmptyStringId,
                 .kind = SymbolKind::Foreign,
                 .generics = TypeIdxRange{}},
        .blocks = {block, 1},
    });
    Storage storage = std::move(builder).build();
    CHECK(verify_storage(storage).is_ok());
  }
  // Struct field range exceeds the types storage.
  {
    StorageBuilder builder;
    builder.struct_type(str::kEmptyStringId, {TypeIdx(99), 1},
                        ir::TypeIdxRange{});
    CHECK(check(std::move(builder).build()) ==
          VerifyErrorKind::StructFieldsOutOfRange);
  }
  // Struct node references a missing metadata entry.
  {
    StorageState state;
    TypeNode bad{};
    bad.tag = TypeTag::Struct;
    bad.data.set(StructTypeIdx(7));
    state.types.emplace_back(bad);
    StorageBuilder builder(std::move(state));
    Storage storage = std::move(builder).build();
    CHECK(check(std::move(storage)) == VerifyErrorKind::TypeMetadataOutOfRange);
  }
}

TEST_CASE("Verify enum types") {
  // Valid: enum with a unit variant and a payload variant.
  {
    StorageBuilder builder;
    const TypeIdx i32 = builder.primitive(TypeTag::I32);
    TypeSeq payload;
    payload.push(builder.ref_type(i32));
    EnumVariantTypeSeq variants;
    variants.push(builder.enum_variant(str::kEmptyStringId, {}));
    variants.push(builder.enum_variant(str::kEmptyStringId, payload.finish()));
    builder.enum_type(str::kEmptyStringId, variants.finish(),
                      ir::TypeIdxRange{});
    Storage storage = std::move(builder).build();
    CHECK(verify_storage(storage).is_ok());
  }
  // Variant range exceeds the variant storage.
  {
    StorageState state;
    state.enum_types.emplace_back(EnumType{
        .name = str::kEmptyStringId,
        .variants = {EnumVariantTypeIdx(7), 1},
        .params = {TypeIdx(0), 0},
    });
    TypeNode bad{};
    bad.tag = TypeTag::Enum;
    bad.data.set(EnumTypeIdx(0));
    state.types.emplace_back(bad);
    StorageBuilder builder(std::move(state));
    Storage storage = std::move(builder).build();
    CHECK(check(std::move(storage)) == VerifyErrorKind::TypeMetadataOutOfRange);
  }
  // Variant payload range exceeds the types storage.
  {
    StorageState state;
    state.enum_variant_types.emplace_back(EnumVariantType{
        .name = str::kEmptyStringId,
        .fields = {TypeIdx(99), 1},
    });
    state.enum_types.emplace_back(EnumType{
        .name = str::kEmptyStringId,
        .variants = {EnumVariantTypeIdx(0), 1},
        .params = {TypeIdx(0), 0},
    });
    TypeNode bad{};
    bad.tag = TypeTag::Enum;
    bad.data.set(EnumTypeIdx(0));
    state.types.emplace_back(bad);
    StorageBuilder builder(std::move(state));
    Storage storage = std::move(builder).build();
    CHECK(check(std::move(storage)) == VerifyErrorKind::EnumFieldsOutOfRange);
  }
}

TEST_CASE("Verify CondBr shapes") {
  // CondBr with a non-i1 condition reports InvalidCondBr.
  {
    StorageBuilder builder;
    const TypeIdx i32 = builder.primitive(TypeTag::I32);

    auto ret_block = [&](ImmutableIdx value) {
      OperandSeq args;
      args.push(builder.operand(Operand::from_immutable(value, i32)));
      const InstructionIdx inst =
          builder.instr({.op = Opcode::Ret,
                         .flags = {},
                         .dst = RegisterIdx(base::kInvalidIdx),
                         .measure = ir::TypeIdx::invalid(),
                         .operands = args.finish()});
      InstrSeq instrs;
      instrs.push(inst);
      return builder.block({.instrs = instrs.finish(), .block_params = {}});
    };

    const ImmutableIdx zero =
        builder.immutable({.type = i32, .data = {.i32_value = 0}});
    builder.reg({.type = i32, .def_idx = InstructionIdx(base::kInvalidIdx)});
    const BlockParamIdx param =
        builder.block_param({.type = i32, .reg = RegisterIdx(0)});
    const BlockIdx entry = builder.block({{}, {}});
    const BlockIdx then_block = ret_block(zero);
    const BlockIdx else_block = ret_block(zero);

    OperandSeq args;
    args.push(builder.operand(Operand::from_register(RegisterIdx(0), i32)));
    args.push(builder.operand(Operand::from_block(then_block, i32)));
    args.push(builder.operand(Operand::from_block(else_block, i32)));
    const InstructionIdx inst =
        builder.instr({.op = Opcode::CondBr,
                       .flags = {},
                       .dst = RegisterIdx(base::kInvalidIdx),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = args.finish()});
    InstrSeq instrs;
    instrs.push(inst);
    builder.set_block_instrs(entry, instrs.finish());
    BlockParamSeq entry_params;
    entry_params.push(param);
    builder.set_block_params(entry, entry_params.finish());

    BlockSeq blocks;
    blocks.push(entry);
    blocks.push(then_block);
    blocks.push(else_block);
    builder.function({
        .meta = {.return_type = i32,
                 .param_types = {},
                 .name = str::kEmptyStringId,
                 .path = str::kEmptyStringId,
                 .kind = SymbolKind::Foreign,
                 .generics = TypeIdxRange{}},
        .blocks = blocks.finish(),
    });
    CHECK(check(std::move(builder).build()) == VerifyErrorKind::InvalidCondBr);
  }
  // CondBr with two operands (missing a target) reports InvalidCondBr.
  {
    StorageBuilder builder;
    const TypeIdx i1 = builder.primitive(TypeTag::I1);
    builder.reg({.type = i1, .def_idx = InstructionIdx(base::kInvalidIdx)});
    const BlockParamIdx param =
        builder.block_param({.type = i1, .reg = RegisterIdx(0)});
    const BlockIdx entry = builder.block({{}, {}});
    OperandSeq args;
    args.push(builder.operand(Operand::from_register(RegisterIdx(0), i1)));
    args.push(builder.operand(Operand::from_block(entry, i1)));
    const InstructionIdx inst =
        builder.instr({.op = Opcode::CondBr,
                       .flags = {},
                       .dst = RegisterIdx(base::kInvalidIdx),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = args.finish()});
    InstrSeq instrs;
    instrs.push(inst);
    builder.set_block_instrs(entry, instrs.finish());
    BlockParamSeq entry_params;
    entry_params.push(param);
    builder.set_block_params(entry, entry_params.finish());
    builder.function({
        .meta = {.return_type = i1,
                 .param_types = {},
                 .name = str::kEmptyStringId,
                 .path = str::kEmptyStringId,
                 .kind = SymbolKind::Foreign,
                 .generics = TypeIdxRange{}},
        .blocks = {entry, 1},
    });
    CHECK(check(std::move(builder).build()) == VerifyErrorKind::InvalidCondBr);
  }
}

TEST_CASE("Verify Switch shapes") {
  // Switch with an odd operand count reports InvalidSwitch.
  {
    StorageBuilder builder;
    const TypeIdx i32 = builder.primitive(TypeTag::I32);
    builder.reg({.type = i32, .def_idx = InstructionIdx(base::kInvalidIdx)});
    const BlockParamIdx param =
        builder.block_param({.type = i32, .reg = RegisterIdx(0)});
    const BlockIdx entry = builder.block({{}, {}});
    const ImmutableIdx zero =
        builder.immutable({.type = i32, .data = {.i32_value = 0}});
    OperandSeq ret_args;
    ret_args.push(builder.operand(Operand::from_immutable(zero, i32)));
    const InstructionIdx inst_ret =
        builder.instr({.op = Opcode::Ret,
                       .flags = {},
                       .dst = RegisterIdx(base::kInvalidIdx),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = ret_args.finish()});
    InstrSeq ret_instrs;
    ret_instrs.push(inst_ret);
    const BlockIdx target =
        builder.block({.instrs = ret_instrs.finish(), .block_params = {}});
    OperandSeq args;
    args.push(builder.operand(Operand::from_register(RegisterIdx(0), i32)));
    args.push(builder.operand(Operand::from_block(target, i32)));
    const ImmutableIdx one =
        builder.immutable({.type = i32, .data = {.i32_value = 1}});
    args.push(builder.operand(Operand::from_immutable(one, i32)));
    const InstructionIdx inst =
        builder.instr({.op = Opcode::Switch,
                       .flags = {},
                       .dst = RegisterIdx(base::kInvalidIdx),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = args.finish()});
    InstrSeq instrs;
    instrs.push(inst);
    builder.set_block_instrs(entry, instrs.finish());
    BlockParamSeq entry_params;
    entry_params.push(param);
    builder.set_block_params(entry, entry_params.finish());
    builder.function({
        .meta = {.return_type = i32,
                 .param_types = {},
                 .name = str::kEmptyStringId,
                 .path = str::kEmptyStringId,
                 .kind = SymbolKind::Foreign,
                 .generics = TypeIdxRange{}},
        .blocks = {entry, 1},
    });
    CHECK(check(std::move(builder).build()) == VerifyErrorKind::InvalidSwitch);
  }
  // Switch with a non-immediate case value reports InvalidSwitch.
  {
    StorageBuilder builder;
    const TypeIdx i32 = builder.primitive(TypeTag::I32);
    const ImmutableIdx one =
        builder.immutable({.type = i32, .data = {.i32_value = 1}});
    const OperandIdx size = builder.operand(Operand::from_immutable(one, i32));
    const InstructionIdx inst_alloc0 =
        builder.instr({.op = Opcode::Alloca,
                       .flags = {},
                       .dst = RegisterIdx(0),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = {size, 1}});
    builder.reg({.type = i32, .def_idx = inst_alloc0});
    const InstructionIdx inst_alloc1 =
        builder.instr({.op = Opcode::Alloca,
                       .flags = {},
                       .dst = RegisterIdx(1),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = {size, 1}});
    builder.reg({.type = i32, .def_idx = inst_alloc1});
    const BlockIdx entry = builder.block({{}, {}});
    OperandSeq args;
    args.push(builder.operand(Operand::from_register(RegisterIdx(0), i32)));
    args.push(builder.operand(Operand::from_block(entry, i32)));
    args.push(builder.operand(Operand::from_register(RegisterIdx(1), i32)));
    args.push(builder.operand(Operand::from_block(entry, i32)));
    const InstructionIdx inst =
        builder.instr({.op = Opcode::Switch,
                       .flags = {},
                       .dst = RegisterIdx(base::kInvalidIdx),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = args.finish()});
    InstrSeq instrs;
    instrs.push(inst_alloc0);
    instrs.push(inst_alloc1);
    instrs.push(inst);
    builder.set_block_instrs(entry, instrs.finish());
    builder.function({
        .meta = {.return_type = i32,
                 .param_types = {},
                 .name = str::kEmptyStringId,
                 .path = str::kEmptyStringId,
                 .kind = SymbolKind::Foreign,
                 .generics = TypeIdxRange{}},
        .blocks = {entry, 1},
    });
    CHECK(check(std::move(builder).build()) == VerifyErrorKind::InvalidSwitch);
  }
}

TEST_CASE("Verify memory shapes") {
  // GetElementPtr without indexes reports InvalidGetElementPtr.
  {
    StorageBuilder builder;
    const TypeIdx i32 = builder.primitive(TypeTag::I32);
    const ImmutableIdx one =
        builder.immutable({.type = i32, .data = {.i32_value = 1}});
    const OperandIdx size = builder.operand(Operand::from_immutable(one, i32));
    const InstructionIdx inst_alloc =
        builder.instr({.op = Opcode::Alloca,
                       .flags = {},
                       .dst = RegisterIdx(0),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = {size, 1}});
    builder.reg({.type = i32, .def_idx = inst_alloc});
    const BlockIdx entry = builder.block({{}, {}});
    OperandSeq args;
    args.push(builder.operand(Operand::from_register(RegisterIdx(0), i32)));
    const InstructionIdx inst =
        builder.instr({.op = Opcode::GetElementPtr,
                       .flags = {},
                       .dst = RegisterIdx(base::kInvalidIdx),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = args.finish()});
    const OperandIdx gep_ret_arg =
        builder.operand(Operand::from_immutable(one, i32));
    const InstructionIdx inst_gep_ret =
        builder.instr({.op = Opcode::Ret,
                       .flags = {},
                       .dst = RegisterIdx(base::kInvalidIdx),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = {gep_ret_arg, 1}});
    InstrSeq instrs;
    instrs.push(inst_alloc);
    instrs.push(inst);
    instrs.push(inst_gep_ret);
    builder.set_block_instrs(entry, instrs.finish());
    builder.function({
        .meta = {.return_type = i32,
                 .param_types = {},
                 .name = str::kEmptyStringId,
                 .path = str::kEmptyStringId,
                 .kind = SymbolKind::Foreign,
                 .generics = TypeIdxRange{}},
        .blocks = {entry, 1},
    });
    CHECK(check(std::move(builder).build()) ==
          VerifyErrorKind::InvalidGetElementPtr);
  }
  // ExtractValue without indexes reports InvalidExtractInsert.
  {
    StorageBuilder builder;
    const TypeIdx i32 = builder.primitive(TypeTag::I32);
    const ImmutableIdx one =
        builder.immutable({.type = i32, .data = {.i32_value = 1}});
    const OperandIdx size = builder.operand(Operand::from_immutable(one, i32));
    const InstructionIdx inst_alloc =
        builder.instr({.op = Opcode::Alloca,
                       .flags = {},
                       .dst = RegisterIdx(0),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = {size, 1}});
    builder.reg({.type = i32, .def_idx = inst_alloc});
    const BlockIdx entry = builder.block({{}, {}});
    OperandSeq args;
    args.push(builder.operand(Operand::from_register(RegisterIdx(0), i32)));
    const InstructionIdx inst =
        builder.instr({.op = Opcode::ExtractValue,
                       .flags = {},
                       .dst = RegisterIdx(1),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = args.finish()});
    builder.reg({.type = i32, .def_idx = inst});
    const OperandIdx ext_ret_arg =
        builder.operand(Operand::from_immutable(one, i32));
    const InstructionIdx inst_ext_ret =
        builder.instr({.op = Opcode::Ret,
                       .flags = {},
                       .dst = RegisterIdx(base::kInvalidIdx),
                       .measure = ir::TypeIdx::invalid(),
                       .operands = {ext_ret_arg, 1}});
    InstrSeq instrs;
    instrs.push(inst_alloc);
    instrs.push(inst);
    instrs.push(inst_ext_ret);
    builder.set_block_instrs(entry, instrs.finish());
    builder.function({
        .meta = {.return_type = i32,
                 .param_types = {},
                 .name = str::kEmptyStringId,
                 .path = str::kEmptyStringId,
                 .kind = SymbolKind::Foreign,
                 .generics = TypeIdxRange{}},
        .blocks = {entry, 1},
    });
    CHECK(check(std::move(builder).build()) ==
          VerifyErrorKind::InvalidExtractInsert);
  }
}

TEST_CASE("Verify call arity") {
  StorageBuilder builder;
  const TypeIdx i32 = builder.primitive(TypeTag::I32);
  const TypeIdx p0 = builder.ref_type(i32);
  const ExternalFunctionIdx callee =
      builder.external_function({.meta = {.return_type = i32,
                                          .param_types = {p0, 1},
                                          .name = str::kEmptyStringId,
                                          .path = str::kEmptyStringId,
                                          .kind = SymbolKind::Foreign,
                                          .generics = TypeIdxRange{}},
                                 .calling_conv = CallingConvention::C});
  const OperandIdx head = builder.operand(Operand::from_external_function(
      callee, primitive_idx(TypeTag::Function)));
  const InstructionIdx inst =
      builder.instr({.op = Opcode::Call,
                     .flags = {},
                     .dst = RegisterIdx(base::kInvalidIdx),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {head, 1}});
  const ImmutableIdx zero =
      builder.immutable({.type = i32, .data = {.i32_value = 0}});
  const OperandIdx ret_arg =
      builder.operand(Operand::from_immutable(zero, i32));
  const InstructionIdx inst_ret =
      builder.instr({.op = Opcode::Ret,
                     .flags = {},
                     .dst = RegisterIdx(base::kInvalidIdx),
                     .measure = ir::TypeIdx::invalid(),
                     .operands = {ret_arg, 1}});
  const BlockIdx entry = builder.block({{}, {}});
  InstrSeq instrs;
  instrs.push(inst);
  instrs.push(inst_ret);
  builder.set_block_instrs(entry, instrs.finish());
  builder.function({
      .meta = {.return_type = i32,
               .param_types = {},
               .name = str::kEmptyStringId,
               .path = str::kEmptyStringId,
               .kind = SymbolKind::Foreign,
               .generics = TypeIdxRange{}},
      .blocks = {entry, 1},
  });
  // One declared parameter but zero call arguments.
  CHECK(check(std::move(builder).build()) == VerifyErrorKind::InvalidCallee);
}

TEST_CASE("VerifyError converts to diagnostic") {
  const VerifyError error{.kind = VerifyErrorKind::UndefinedRegister,
                          .index = 5};
  const diag::Diagnostic diag = to_diagnostic(error);
  CHECK(diag.severity == diag::Severity::Error);
  CHECK(diag.code >= 1000);
  CHECK(diag.code < 2000);
  CHECK(diag.message == "UndefinedRegister");
  CHECK(!diag.has_primary_span);

  // Codes are stable per kind.
  const VerifyError other{.kind = VerifyErrorKind::UnterminatedBlock,
                          .index = 0};
  CHECK(to_diagnostic(other).code != diag.code);
}

}  // namespace ir
