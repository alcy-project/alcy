// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The drop pass reads a journal and rewrites streams; a case builds
// both by hand, so what it asserts is the pass's own answer and not a
// lowering run's.

#include "lowering/drop_elaboration.h"

#include <unordered_map>
#include <vector>

#include "analyzer/types.h"
#include "diag/bag.h"
#include "diag/span.h"
#include "doctest/doctest.h"
#include "fpag/base/idx_range.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "ir/common.h"
#include "ir/opcode.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace lowering {

namespace {

struct Fixture {
  mem::Arena arena;
  diag::DiagBag bag{arena, i18n::Language::EnUs};
  ir::StorageBuilder builder;
  std::vector<u8> needs_drop;
  std::vector<analyzer::CheckedModule::DropGlue> drop_glue;
  std::unordered_map<u32, u32> type_origins;
  std::vector<diag::Span> instr_spans;
  std::vector<ir::InstrSeq> streams;
  std::vector<ir::BlockIdx> blocks;

  Fixture() { arena.reserve(1u << 20); }

  // A type the analyzer measured as ending with code, with the glue
  // method the pass has to resolve.
  ir::TypeIdx drop_type(ir::TypeTag tag, u32 method) {
    const ir::TypeIdx type = builder.primitive(tag);
    if (needs_drop.size() <= type.idx) {
      needs_drop.resize(type.idx + 1, 0);
    }
    needs_drop[type.idx] = 1;
    if (drop_glue.size() <= type.idx) {
      drop_glue.resize(type.idx + 1);
    }
    drop_glue[type.idx] =
        analyzer::CheckedModule::DropGlue{.module = 0, .index = method};
    return type;
  }
};

}  // namespace

TEST_CASE("The drop pass ends a bound local at its site") {
  Fixture fx;
  const ir::TypeIdx type = fx.drop_type(ir::TypeTag::U32, 0);
  const ir::RegisterIdx addr = fx.builder.reg(
      {.type = type, .def_idx = ir::InstructionIdx(base::INVALID_IDX)});

  const std::vector<Local> locals{Local{"value", addr, type}};
  const std::vector<OwnershipEvent> events{
      OwnershipEvent{.kind = OwnershipEvent::Kind::Bind,
                     .local = 0,
                     .path = {},
                     .block = ir::BlockIdx(0),
                     .offset = 0,
                     .seq = 0,
                     .span = {}}};
  const std::vector<EndSite> sites{EndSite{.block = ir::BlockIdx(0),
                                           .offset = 0,
                                           .seq = 1,
                                           .locals = {0},
                                           .span = {}}};
  fx.streams.emplace_back();
  fx.blocks.emplace_back(0);

  const DropElaborationInputs inputs{
      .needs_drop = fx.needs_drop,
      .not_movable = {},
      .drop_glue = fx.drop_glue,
      .builder = fx.builder,
      .bag = fx.bag,
      .locals = locals,
      .ownership = events,
      .end_sites = sites,
      .instr_spans = fx.instr_spans,
      .streams = fx.streams,
      .blocks = fx.blocks,
      .base = 0,
      .type_origins = fx.type_origins,
      .size_one = ir::OperandIdx(0),
      .zero_i32 = ir::OperandIdx(0),
      .width = ir::PointerWidth::W64,
      .drop_method = [](u32, u32) { return ir::FunctionIdx(0); },
      .reserve_block = [] { return ir::BlockIdx(0); },
  };
  elaborate_drops(inputs);

  const base::IdxRange<ir::InstructionIdx> placed = fx.streams[0].finish();
  CHECK(placed.size() == 4);
  if (placed.size() != 4) {
    return;
  }
  const ir::StorageState& state = fx.builder.state();
  CHECK(state.instrs[placed[0].idx].op == ir::Opcode::Drop);
  CHECK(state.instrs[placed[1].idx].op == ir::Opcode::Load);
  CHECK(state.instrs[placed[2].idx].op == ir::Opcode::Move);
  CHECK(state.instrs[placed[3].idx].op == ir::Opcode::Call);
}

TEST_CASE("The drop pass leaves a moved local alone") {
  Fixture fx;
  const ir::TypeIdx type = fx.drop_type(ir::TypeTag::U32, 0);
  const ir::RegisterIdx addr = fx.builder.reg(
      {.type = type, .def_idx = ir::InstructionIdx(base::INVALID_IDX)});

  const std::vector<Local> locals{Local{"value", addr, type}};
  const std::vector<OwnershipEvent> events{
      OwnershipEvent{.kind = OwnershipEvent::Kind::Bind,
                     .local = 0,
                     .path = {},
                     .block = ir::BlockIdx(0),
                     .offset = 0,
                     .seq = 0,
                     .span = {}},
      OwnershipEvent{.kind = OwnershipEvent::Kind::Move,
                     .local = 0,
                     .path = {},
                     .block = ir::BlockIdx(0),
                     .offset = 0,
                     .seq = 1,
                     .span = {}}};
  const std::vector<EndSite> sites{EndSite{.block = ir::BlockIdx(0),
                                           .offset = 0,
                                           .seq = 2,
                                           .locals = {0},
                                           .span = {}}};
  fx.streams.emplace_back();
  fx.blocks.emplace_back(0);

  const DropElaborationInputs inputs{
      .needs_drop = fx.needs_drop,
      .not_movable = {},
      .drop_glue = fx.drop_glue,
      .builder = fx.builder,
      .bag = fx.bag,
      .locals = locals,
      .ownership = events,
      .end_sites = sites,
      .instr_spans = fx.instr_spans,
      .streams = fx.streams,
      .blocks = fx.blocks,
      .base = 0,
      .type_origins = fx.type_origins,
      .size_one = ir::OperandIdx(0),
      .zero_i32 = ir::OperandIdx(0),
      .width = ir::PointerWidth::W64,
      .drop_method = [](u32, u32) { return ir::FunctionIdx(0); },
      .reserve_block = [] { return ir::BlockIdx(0); },
  };
  elaborate_drops(inputs);

  CHECK(fx.streams[0].finish().size() == 0);
}

TEST_CASE("A place path resolves through a tuple and its origin") {
  ir::StorageBuilder builder;
  const ir::TypeIdx element = builder.primitive(ir::TypeTag::U32);
  ir::TypeSeq elements;
  elements.push(element);
  const ir::TypeIdx tuple = builder.tuple_type(elements.finish());
  // A storage copy stands for the declaration it came from, the same
  // link the analyzer leaves behind for the destructor tables.
  const ir::TypeIdx copy = builder.reference_type(tuple, false);
  std::unordered_map<u32, u32> origins;
  origins.emplace(copy.idx, tuple.idx);

  const std::vector<u32> first{0};
  const std::vector<u32> past_end{7};
  CHECK(type_at_path(builder, origins, tuple, first) == element);
  CHECK(type_at_path(builder, origins, copy, first) == element);
  CHECK(type_at_path(builder, origins, tuple, past_end) ==
        builder.error_type());
}

namespace {

// Runs the pass over one empty block and answers the functions its
// calls name, in order: a case states a journal and reads the order
// back. Glue method `n` is function `100 + n`.
std::vector<u32> placed_calls(Fixture& fx,
                              const std::vector<Local>& locals,
                              const std::vector<OwnershipEvent>& events,
                              const std::vector<EndSite>& sites) {
  fx.streams.emplace_back();
  fx.blocks.emplace_back(0);
  const DropElaborationInputs inputs{
      .needs_drop = fx.needs_drop,
      .not_movable = {},
      .drop_glue = fx.drop_glue,
      .builder = fx.builder,
      .bag = fx.bag,
      .locals = locals,
      .ownership = events,
      .end_sites = sites,
      .instr_spans = fx.instr_spans,
      .streams = fx.streams,
      .blocks = fx.blocks,
      .base = 0,
      .type_origins = fx.type_origins,
      .size_one = ir::OperandIdx(0),
      .zero_i32 = ir::OperandIdx(0),
      .width = ir::PointerWidth::W64,
      .drop_method = [](u32,
                        u32 method) { return ir::FunctionIdx(100 + method); },
      .reserve_block = [] { return ir::BlockIdx(0); },
  };
  elaborate_drops(inputs);
  std::vector<u32> called;
  const base::IdxRange<ir::InstructionIdx> placed = fx.streams[0].finish();
  for (ir::InstructionIdx idx : placed) {
    const ir::Instruction& instr = fx.builder.state().instrs[idx.idx];
    if (instr.op != ir::Opcode::Call) {
      continue;
    }
    for (ir::OperandIdx o : instr.operands) {
      const ir::Operand& operand = fx.builder.state().operands[o.idx];
      if (operand.is<ir::FunctionIdx>()) {
        called.push_back(operand.as_function().idx);
      }
    }
  }
  return called;
}

}  // namespace

TEST_CASE("A tuple's fields end in declaration order") {
  Fixture fx;
  const ir::TypeIdx first = fx.drop_type(ir::TypeTag::U32, 0);
  const ir::TypeIdx second = fx.drop_type(ir::TypeTag::U64, 1);
  ir::TypeSeq elements;
  elements.push(first);
  elements.push(second);
  const ir::TypeIdx tuple = fx.builder.tuple_type(elements.finish());
  if (fx.needs_drop.size() <= tuple.idx) {
    fx.needs_drop.resize(tuple.idx + 1, 0);
  }
  // The tuple ends with code, and its own glue is absent: its fields
  // end, in declaration order.
  fx.needs_drop[tuple.idx] = 1;
  const ir::RegisterIdx addr = fx.builder.reg(
      {.type = tuple, .def_idx = ir::InstructionIdx(base::INVALID_IDX)});
  const std::vector<Local> locals{Local{"value", addr, tuple}};
  const std::vector<OwnershipEvent> events{
      OwnershipEvent{.kind = OwnershipEvent::Kind::Bind,
                     .local = 0,
                     .path = {},
                     .block = ir::BlockIdx(0),
                     .offset = 0,
                     .seq = 0,
                     .span = {}}};
  const std::vector<EndSite> sites{EndSite{.block = ir::BlockIdx(0),
                                           .offset = 0,
                                           .seq = 1,
                                           .locals = {0},
                                           .span = {}}};
  const std::vector<u32> called = placed_calls(fx, locals, events, sites);
  CHECK(called == std::vector<u32>{100, 101});
}

TEST_CASE("A block's locals end last-declared first") {
  Fixture fx;
  const ir::TypeIdx first = fx.drop_type(ir::TypeTag::U32, 0);
  const ir::TypeIdx second = fx.drop_type(ir::TypeTag::U64, 1);
  const ir::RegisterIdx a = fx.builder.reg(
      {.type = first, .def_idx = ir::InstructionIdx(base::INVALID_IDX)});
  const ir::RegisterIdx b = fx.builder.reg(
      {.type = second, .def_idx = ir::InstructionIdx(base::INVALID_IDX)});
  const std::vector<Local> locals{Local{"first", a, first},
                                  Local{"second", b, second}};
  const std::vector<OwnershipEvent> events{
      OwnershipEvent{.kind = OwnershipEvent::Kind::Bind,
                     .local = 0,
                     .path = {},
                     .block = ir::BlockIdx(0),
                     .offset = 0,
                     .seq = 0,
                     .span = {}},
      OwnershipEvent{.kind = OwnershipEvent::Kind::Bind,
                     .local = 1,
                     .path = {},
                     .block = ir::BlockIdx(0),
                     .offset = 0,
                     .seq = 1,
                     .span = {}}};
  // The site names them innermost last, the way a scope declares them.
  const std::vector<EndSite> sites{EndSite{.block = ir::BlockIdx(0),
                                           .offset = 0,
                                           .seq = 2,
                                           .locals = {0, 1},
                                           .span = {}}};
  const std::vector<u32> called = placed_calls(fx, locals, events, sites);
  CHECK(called == std::vector<u32>{101, 100});
}

}  // namespace lowering
