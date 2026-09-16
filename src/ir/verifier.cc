// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "ir/verifier.h"

#include <vector>

#include "fpag/base/numeric.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"

namespace ir {

namespace {

constexpr bool is_terminator(const Opcode op) {
  return op == Opcode::Br || op == Opcode::CondBr || op == Opcode::Switch ||
         op == Opcode::Ret || op == Opcode::Unreachable;
}

// Checks that [head, head + size) lies within a storage of vec_size.
template <typename Idx>
constexpr bool range_in_bounds(const Idx head,
                               const u32 size,
                               const usize vec_size) {
  return static_cast<u64>(head.idx) + size <= vec_size;
}

VerifyResult err(const VerifyErrorKind kind, const u32 index) {
  return base::make_err(VerifyError{.kind = kind, .index = index});
}

}  // namespace

VerifyResult verify_storage(const Storage& storage) {
  // Function-level ranges.
  for (FunctionIdx fidx(0); fidx.idx < storage.functions().size(); ++fidx) {
    const Function& func = storage.functions()[fidx];
    if (!range_in_bounds(func.blocks.head(), func.blocks.size(),
                         storage.blocks().size())) {
      return err(VerifyErrorKind::FunctionBlocksOutOfRange, fidx.idx);
    }
    // TODO: check func.meta.param_types against storage.types() once composite
    // type metadata is connected.
  }

  // Register definitions: bound by block params or defined by instructions.
  std::vector<bool> defined(storage.registers().size(), false);
  for (BlockIdx bidx(0); bidx.idx < storage.blocks().size(); ++bidx) {
    const Block& block = storage.blocks()[bidx];
    if (!range_in_bounds(block.instrs.head(), block.instrs.size(),
                         storage.instrs().size())) {
      return err(VerifyErrorKind::BlockInstrsOutOfRange, bidx.idx);
    }
    if (!range_in_bounds(block.block_params.head(), block.block_params.size(),
                         storage.block_params().size())) {
      return err(VerifyErrorKind::BlockParamsOutOfRange, bidx.idx);
    }
    for (const BlockParamIdx pidx : block.block_params) {
      const RegisterIdx reg = storage.block_params()[pidx].reg;
      if (reg.idx >= storage.registers().size()) {
        return err(VerifyErrorKind::BlockParamRegOutOfRange, reg.idx);
      }
      if (defined[reg.idx]) {
        return err(VerifyErrorKind::RedefinedRegister, reg.idx);
      }
      defined[reg.idx] = true;
    }
    for (const InstructionIdx iidx : block.instrs) {
      const RegisterIdx dst = storage.instrs()[iidx].dst;
      if (dst.is_valid()) {
        if (dst.idx >= storage.registers().size()) {
          return err(VerifyErrorKind::InstrDstOutOfRange, iidx.idx);
        }
        if (defined[dst.idx]) {
          return err(VerifyErrorKind::RedefinedRegister, dst.idx);
        }
        defined[dst.idx] = true;
      }
    }
  }

  for (BlockIdx bidx(0); bidx.idx < storage.blocks().size(); ++bidx) {
    const Block& block = storage.blocks()[bidx];
    if (block.instrs.empty()) {
      return err(VerifyErrorKind::UnterminatedBlock, bidx.idx);
    }
    for (const InstructionIdx iidx : block.instrs) {
      const Instruction& instr = storage.instrs()[iidx];
      const bool last =
          (iidx.idx + 1 == block.instrs.head().idx + block.instrs.size());
      if (is_terminator(instr.op) != last) {
        if (last) {
          return err(VerifyErrorKind::UnterminatedBlock, bidx.idx);
        }
        return err(VerifyErrorKind::MisplacedTerminator, iidx.idx);
      }
      if (!range_in_bounds(instr.operands.head(), instr.operands.size(),
                           storage.operands().size())) {
        return err(VerifyErrorKind::InstrOperandsOutOfRange, iidx.idx);
      }
      for (const OperandIdx oidx : instr.operands) {
        const Operand& operand = storage.operands()[oidx];
        switch (operand.tag) {
          case OperandTag::Register:
            if (operand.as_register().idx >= storage.registers().size()) {
              return err(VerifyErrorKind::OperandIdxOutOfRange, oidx.idx);
            }
            if (!defined[operand.as_register().idx]) {
              return err(VerifyErrorKind::UndefinedRegister,
                         operand.as_register().idx);
            }
            break;
          case OperandTag::Function:
            if (operand.as_function().idx >= storage.functions().size()) {
              return err(VerifyErrorKind::OperandIdxOutOfRange, oidx.idx);
            }
            break;
          case OperandTag::Block:
            if (operand.as_block().idx >= storage.blocks().size()) {
              return err(VerifyErrorKind::OperandIdxOutOfRange, oidx.idx);
            }
            break;
          case OperandTag::Immutable:
            if (operand.as_immutable().idx >= storage.immutables().size()) {
              return err(VerifyErrorKind::OperandIdxOutOfRange, oidx.idx);
            }
            break;
          case OperandTag::ExternalFunction:
            if (operand.as_external_function().idx >=
                storage.external_functions().size()) {
              return err(VerifyErrorKind::OperandIdxOutOfRange, oidx.idx);
            }
            break;
          case OperandTag::Unknown:
            return err(VerifyErrorKind::UnknownOperandTag, oidx.idx);
        }
      }
      if (instr.op == Opcode::Call) {
        if (instr.operands.empty()) {
          return err(VerifyErrorKind::InvalidCallee, iidx.idx);
        }
        const OperandTag head_tag =
            storage.operands()[instr.operands.head()].tag;
        if (head_tag != OperandTag::Function &&
            head_tag != OperandTag::ExternalFunction) {
          return err(VerifyErrorKind::InvalidCallee, iidx.idx);
        }
      }
      if (instr.op == Opcode::Br) {
        if (instr.operands.empty()) {
          return err(VerifyErrorKind::InvalidBranchTarget, iidx.idx);
        }
        if (storage.operands()[instr.operands.head()].tag !=
            OperandTag::Block) {
          return err(VerifyErrorKind::InvalidBranchTarget, iidx.idx);
        }
      }
    }
  }

  // Functions must contain at least one block.
  for (FunctionIdx fidx(0); fidx.idx < storage.functions().size(); ++fidx) {
    if (storage.functions()[fidx].blocks.empty()) {
      return err(VerifyErrorKind::UnterminatedBlock, fidx.idx);
    }
  }

  return base::make_ok();
}

}  // namespace ir
