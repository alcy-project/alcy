// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "overlay/overlay.h"

#include <span>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/page_allocator.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/symbol_table.h"
#include "ir/type.h"

namespace overlay {

namespace {

// A void function whose block is `body` and ends with the return the
// verifier wants, so a test writes only the instructions it is about.
ir::VerifiedStorage block_of(ir::StorageBuilder& builder,
                             std::span<const ir::InstructionIdx> body) {
  const ir::TypeIdx ret_ty = builder.primitive(ir::TypeTag::Void);
  const ir::InstructionIdx ret = builder.instr({
      .op = ir::Opcode::Ret,
      .flags = {},
      .dst = ir::RegisterIdx(base::INVALID_IDX),
      .measure = ir::TypeIdx::invalid(),
      .operands = {},
  });
  ir::InstrSeq instrs;
  for (const ir::InstructionIdx idx : body) {
    instrs.push(idx);
  }
  instrs.push(ret);
  const ir::BlockIdx block =
      builder.block({.instrs = instrs.finish(), .block_params = {}});
  builder.function({
      .meta = {.return_type = ret_ty,
               .param_types = {},
               .name = str::EMPTY_STRING_ID,
               .path = str::EMPTY_STRING_ID,
               .kind = ir::SymbolKind::Free,
               .generics = ir::TypeIdxRange{}},
      .blocks = {block, 1},
  });
  return std::move(builder).build().unwrap();
}

// The immediates two-operand tests share.
struct Operands {
  ir::TypeIdx i32;
  ir::ImmutableIdx one;
  ir::ImmutableIdx two;
  ir::OperandIdxRange pair;
};

Operands two_operands(ir::StorageBuilder& builder) {
  const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
  const ir::ImmutableIdx one =
      builder.immutable({.type = i32, .data = {.i32_value = 1}});
  const ir::ImmutableIdx two =
      builder.immutable({.type = i32, .data = {.i32_value = 2}});
  ir::OperandSeq args;
  args.push(builder.operand(ir::Operand::from_immutable(one, i32)));
  args.push(builder.operand(ir::Operand::from_immutable(two, i32)));
  return Operands{i32, one, two, args.finish()};
}

}  // namespace

TEST_CASE("The checks off leave every entry None") {
  ir::StorageBuilder builder;
  const Operands operands = two_operands(builder);
  const ir::InstructionIdx add = builder.instr({
      .op = ir::Opcode::IntAdd,
      .flags = {},
      .dst = ir::RegisterIdx(0),
      .measure = ir::TypeIdx::invalid(),
      .operands = operands.pair,
  });
  builder.reg({.type = operands.i32, .def_idx = add});
  const ir::InstructionIdx body[] = {add};
  ir::VerifiedStorage storage = block_of(builder, body);
  const Overlay checks = build(*storage, false, {});
  CHECK(checks.entries.size() == 2);
  CHECK(checks.entries[0].kind == Check::None);
  CHECK(checks.entries[1].kind == Check::None);
}

TEST_CASE(
    "The checked operations carry their entries and the wrap forms do not") {
  ir::StorageBuilder builder;
  const Operands operands = two_operands(builder);
  const ir::InstructionIdx add = builder.instr({
      .op = ir::Opcode::IntAdd,
      .flags = {},
      .dst = ir::RegisterIdx(0),
      .measure = ir::TypeIdx::invalid(),
      .operands = operands.pair,
  });
  builder.reg({.type = operands.i32, .def_idx = add});
  const ir::InstructionIdx wrap = builder.instr({
      .op = ir::Opcode::IntAddWrap,
      .flags = {},
      .dst = ir::RegisterIdx(1),
      .measure = ir::TypeIdx::invalid(),
      .operands = operands.pair,
  });
  builder.reg({.type = operands.i32, .def_idx = wrap});
  const ir::InstructionIdx body[] = {add, wrap};
  ir::VerifiedStorage storage = block_of(builder, body);

  const usize page = mem::page_size();
  ir::SymbolTable pool(page);
  const str::StringPoolId site = pool.intern("main.al:2:3");
  std::vector<str::StringPoolId> sites{site, site};
  const Overlay checks = build(*storage, true, sites);
  CHECK(checks.entries.size() == 3);
  CHECK(checks.entries[0].kind == Check::Overflow);
  // The instruction's own operands answer, so no raw operand is named.
  CHECK(!checks.entries[0].operand.is_valid());
  CHECK(checks.entries[0].site == site);
  // A wrapping form is explicit: the build checks nothing there.
  CHECK(checks.entries[1].kind == Check::None);
}

TEST_CASE("A masked shift's entry names the count the source wrote") {
  ir::StorageBuilder builder;
  const Operands operands = two_operands(builder);
  const ir::ImmutableIdx mask =
      builder.immutable({.type = operands.i32, .data = {.i32_value = 31}});
  // The count the source wrote, with the mask lowering puts in front of
  // the shift; the entry has to reach past the mask.
  ir::OperandSeq mask_args;
  mask_args.push(
      builder.operand(ir::Operand::from_immutable(operands.two, operands.i32)));
  mask_args.push(
      builder.operand(ir::Operand::from_immutable(mask, operands.i32)));
  const ir::OperandIdxRange mask_operands = mask_args.finish();
  const ir::OperandIdx raw_count = mask_operands.head();
  const ir::InstructionIdx and_idx = builder.instr({
      .op = ir::Opcode::And,
      .flags = {},
      .dst = ir::RegisterIdx(0),
      .measure = ir::TypeIdx::invalid(),
      .operands = mask_operands,
  });
  builder.reg({.type = operands.i32, .def_idx = and_idx});
  ir::OperandSeq shift_args;
  shift_args.push(
      builder.operand(ir::Operand::from_immutable(operands.one, operands.i32)));
  shift_args.push(builder.operand(
      ir::Operand::from_register(ir::RegisterIdx(0), operands.i32)));
  const ir::InstructionIdx shift = builder.instr({
      .op = ir::Opcode::ShiftLeft,
      .flags = {},
      .dst = ir::RegisterIdx(1),
      .measure = ir::TypeIdx::invalid(),
      .operands = shift_args.finish(),
  });
  builder.reg({.type = operands.i32, .def_idx = shift});
  const ir::InstructionIdx body[] = {and_idx, shift};
  ir::VerifiedStorage storage = block_of(builder, body);
  const Overlay checks = build(*storage, true, {});
  CHECK(checks.entries.size() == 3);
  CHECK(checks.entries[0].kind == Check::None);
  CHECK(checks.entries[1].kind == Check::ShiftRange);
  CHECK(checks.entries[1].operand == raw_count);
}

TEST_CASE("A signed division's entry names the divisor the source wrote") {
  ir::StorageBuilder builder;
  const Operands operands = two_operands(builder);
  const ir::TypeIdx i1 = builder.primitive(ir::TypeTag::I1);
  const ir::TypeIdx u32 = builder.primitive(ir::TypeTag::U32);
  const ir::ImmutableIdx selector =
      builder.immutable({.type = i1, .data = {.i1_value = false}});
  // The replaced divisor lowering puts in front of a signed division;
  // the entry has to reach past it to the divisor the source wrote.
  ir::OperandSeq select_args;
  select_args.push(builder.operand(ir::Operand::from_immutable(selector, i1)));
  select_args.push(
      builder.operand(ir::Operand::from_immutable(operands.one, operands.i32)));
  select_args.push(
      builder.operand(ir::Operand::from_immutable(operands.two, operands.i32)));
  const ir::OperandIdxRange select_operands = select_args.finish();
  const ir::OperandIdx raw_divisor = select_operands.head() + 2;
  const ir::InstructionIdx select = builder.instr({
      .op = ir::Opcode::Select,
      .flags = {},
      .dst = ir::RegisterIdx(0),
      .measure = ir::TypeIdx::invalid(),
      .operands = select_operands,
  });
  builder.reg({.type = operands.i32, .def_idx = select});
  ir::OperandSeq div_args;
  div_args.push(
      builder.operand(ir::Operand::from_immutable(operands.one, operands.i32)));
  div_args.push(builder.operand(
      ir::Operand::from_register(ir::RegisterIdx(0), operands.i32)));
  const ir::InstructionIdx divide = builder.instr({
      .op = ir::Opcode::IntDiv,
      .flags = {},
      .dst = ir::RegisterIdx(1),
      .measure = ir::TypeIdx::invalid(),
      .operands = div_args.finish(),
  });
  builder.reg({.type = operands.i32, .def_idx = divide});
  ir::OperandSeq uint_args;
  uint_args.push(
      builder.operand(ir::Operand::from_immutable(operands.one, u32)));
  uint_args.push(
      builder.operand(ir::Operand::from_immutable(operands.two, u32)));
  const ir::InstructionIdx unsigned_divide = builder.instr({
      .op = ir::Opcode::UintDiv,
      .flags = {},
      .dst = ir::RegisterIdx(2),
      .measure = ir::TypeIdx::invalid(),
      .operands = uint_args.finish(),
  });
  builder.reg({.type = u32, .def_idx = unsigned_divide});
  const ir::InstructionIdx body[] = {select, divide, unsigned_divide};
  ir::VerifiedStorage storage = block_of(builder, body);
  const Overlay checks = build(*storage, true, {});
  CHECK(checks.entries.size() == 4);
  CHECK(checks.entries[0].kind == Check::None);
  CHECK(checks.entries[1].kind == Check::MinOverOne);
  CHECK(checks.entries[1].operand == raw_divisor);
  // An unsigned division cannot overflow at `-1`.
  CHECK(checks.entries[2].kind == Check::None);
}

}  // namespace overlay
