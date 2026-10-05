// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <initializer_list>
#include <utility>
#include <vector>

#include "codegen/wasm/reach.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "ir/block.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"

namespace codegen::wasm {

namespace {

// Four one-block functions. Function 0 calls 1, 1 calls 2, 2 calls itself,
// and 3 calls 0; with `value_edge` function 0 also holds a function value
// for 3, which is an edge that is not a call.
ir::Storage chain(bool value_edge) {
  ir::StorageState state;
  for (u32 i = 0; i < 4; ++i) {
    ir::Function function{ir::FunctionMeta{.return_type = ir::TypeIdx(0),
                                           .param_types = {},
                                           .name = {},
                                           .path = {},
                                           .kind = ir::SymbolKind::Free,
                                           .generics = {}},
                          {}};
    function.blocks = ir::BlockIdxRange{ir::BlockIdx(i), 1};
    state.functions.emplace_back(function);
    ir::Block block{};
    state.blocks.emplace_back(block);
  }
  const auto add = [&state](ir::Opcode op, ir::FunctionIdx target) {
    const ir::Instruction instr{.op = op,
                                .flags = {},
                                .dst = ir::RegisterIdx::invalid(),
                                .measure = ir::TypeIdx::invalid(),
                                .operands = ir::OperandIdxRange{
                                    ir::OperandIdx(state.operands.size()), 1}};
    state.instrs.emplace_back(instr);
    state.operands.emplace_back(
        ir::Operand::from_function(target, ir::TypeIdx(0)));
  };
  add(ir::Opcode::Call, ir::FunctionIdx(1));
  if (value_edge) {
    add(ir::Opcode::Move, ir::FunctionIdx(3));
  }
  add(ir::Opcode::Call, ir::FunctionIdx(2));
  add(ir::Opcode::Call, ir::FunctionIdx(2));
  add(ir::Opcode::Call, ir::FunctionIdx(0));

  // With the value edge, function 0's block holds two instructions; every
  // later block starts one instruction further along.
  const u32 first = value_edge ? 2 : 1;
  state.blocks[0].instrs =
      ir::InstructionIdxRange{ir::InstructionIdx(0), first};
  state.blocks[1].instrs =
      ir::InstructionIdxRange{ir::InstructionIdx(first), 1};
  state.blocks[2].instrs =
      ir::InstructionIdxRange{ir::InstructionIdx(first + 1), 1};
  state.blocks[3].instrs =
      ir::InstructionIdxRange{ir::InstructionIdx(first + 2), 1};
  return ir::Storage(std::move(state));
}

std::vector<bool> reachable(const ir::Storage& storage,
                            std::initializer_list<ir::FunctionIdx> roots) {
  const std::vector<ir::FunctionIdx> root_list(roots);
  return reachable_functions(storage, root_list);
}

}  // namespace

TEST_CASE("Reachability follows calls from the roots") {
  const ir::Storage storage = chain(/*value_edge=*/false);
  const std::vector<bool> set = reachable(storage, {ir::FunctionIdx(0)});
  CHECK(set.size() == 4);
  CHECK(set[0]);
  CHECK(set[1]);
  CHECK(set[2]);
  // Function 3 calls back into the reachable set, but nothing reaches it.
  CHECK(!set[3]);
}

TEST_CASE("A function value operand is an edge too") {
  const ir::Storage storage = chain(/*value_edge=*/true);
  const std::vector<bool> set = reachable(storage, {ir::FunctionIdx(0)});
  CHECK(set.size() == 4);
  // The move in function 0 names function 3, which no call reaches.
  CHECK(set[3]);
}

TEST_CASE("No roots reach nothing") {
  const ir::Storage storage = chain(/*value_edge=*/false);
  const std::vector<bool> set = reachable(storage, {});
  CHECK(set.size() == 4);
  CHECK(!set[0]);
  CHECK(!set[1]);
  CHECK(!set[2]);
  CHECK(!set[3]);
}

}  // namespace codegen::wasm
