// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/wasm/reach.h"

#include <span>
#include <vector>

#include "ir/block.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/operand.h"
#include "ir/storage.h"

namespace codegen::wasm {

std::vector<bool> reachable_functions(const ir::Storage& storage,
                                      std::span<const ir::FunctionIdx> roots) {
  std::vector<bool> reachable(storage.functions().size(), false);
  std::vector<ir::FunctionIdx> worklist(roots.begin(), roots.end());
  while (!worklist.empty()) {
    const ir::FunctionIdx function = worklist.back();
    worklist.pop_back();
    if (function.idx >= reachable.size() || reachable[function.idx]) {
      continue;
    }
    reachable[function.idx] = true;
    const ir::Function& body = storage.functions()[function];
    for (const ir::BlockIdx block_idx : body.blocks) {
      const ir::Block& block = storage.blocks()[block_idx];
      for (const ir::InstructionIdx instr_idx : block.instrs) {
        const ir::Instruction& instr = storage.instrs()[instr_idx];
        for (const ir::OperandIdx operand_idx : instr.operands) {
          const ir::Operand& operand = storage.operands()[operand_idx];
          if (operand.is<ir::FunctionIdx>()) {
            worklist.push_back(operand.as_function());
          }
        }
      }
    }
  }
  return reachable;
}

}  // namespace codegen::wasm
