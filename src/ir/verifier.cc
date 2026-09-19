// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include "ir/verifier.h"

#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "ir/block.h"
#include "ir/common.h"
#include "ir/function.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/storage.h"
#include "ir/type_util.h"

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
  // Type table and composite metadata.
  for (TypeIdx tidx(0); tidx.idx < storage.types().size(); ++tidx) {
    const TypeNode& node = storage.types()[tidx];
    if (node.tag == TypeTag::Struct) {
      const StructTypeIdx sidx = node.as_struct();
      if (sidx.idx >= storage.struct_types().size()) {
        return err(VerifyErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      const StructType& struct_type = storage.struct_types()[sidx];
      if (!range_in_bounds(struct_type.fields.head(), struct_type.fields.size(),
                           storage.types().size())) {
        return err(VerifyErrorKind::StructFieldsOutOfRange, sidx.idx);
      }
    } else if (node.tag == TypeTag::Array) {
      const ArrayTypeIdx aidx = node.as_array();
      if (aidx.idx >= storage.array_types().size()) {
        return err(VerifyErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      if (storage.array_types()[aidx].element.idx >= storage.types().size()) {
        return err(VerifyErrorKind::TypeIdxOutOfRange,
                   storage.array_types()[aidx].element.idx);
      }
    } else if (node.tag == TypeTag::Enum) {
      const EnumTypeIdx eidx = node.as_enum();
      if (eidx.idx >= storage.enum_types().size()) {
        return err(VerifyErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      const EnumType& enum_type = storage.enum_types()[eidx];
      if (!range_in_bounds(enum_type.variants.head(), enum_type.variants.size(),
                           storage.enum_variant_types().size())) {
        return err(VerifyErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      for (EnumVariantTypeIdx vidx = enum_type.variants.head();
           vidx.idx < enum_type.variants.head().idx + enum_type.variants.size();
           vidx = EnumVariantTypeIdx(vidx.idx + 1)) {
        const EnumVariantType& variant = storage.enum_variant_types()[vidx];
        if (!range_in_bounds(variant.fields.head(), variant.fields.size(),
                             storage.types().size())) {
          return err(VerifyErrorKind::EnumFieldsOutOfRange, vidx.idx);
        }
      }
    } else if (node.tag == TypeTag::Tuple) {
      const TupleTypeIdx tuple_idx = node.as_tuple();
      if (tuple_idx.idx >= storage.tuple_types().size()) {
        return err(VerifyErrorKind::TypeMetadataOutOfRange, tidx.idx);
      }
      const TupleType& tuple = storage.tuple_types()[tuple_idx];
      if (!range_in_bounds(tuple.elements.head(), tuple.elements.size(),
                           storage.types().size())) {
        return err(VerifyErrorKind::TupleFieldsOutOfRange, tuple_idx.idx);
      }
    }
  }

  // Registers carry types.
  for (RegisterIdx ridx(0); ridx.idx < storage.registers().size(); ++ridx) {
    if (storage.registers()[ridx].type.idx >= storage.types().size()) {
      return err(VerifyErrorKind::TypeIdxOutOfRange, ridx.idx);
    }
  }

  for (ImmutableIdx iidx(0); iidx.idx < storage.immutables().size(); ++iidx) {
    if (storage.immutables()[iidx].type.idx >= storage.types().size()) {
      return err(VerifyErrorKind::TypeIdxOutOfRange, iidx.idx);
    }
  }

  // Function-level ranges.
  for (FunctionIdx fidx(0); fidx.idx < storage.functions().size(); ++fidx) {
    const Function& func = storage.functions()[fidx];
    if (!range_in_bounds(func.blocks.head(), func.blocks.size(),
                         storage.blocks().size())) {
      return err(VerifyErrorKind::FunctionBlocksOutOfRange, fidx.idx);
    }
    if (func.meta.return_type.idx >= storage.types().size()) {
      return err(VerifyErrorKind::TypeIdxOutOfRange, fidx.idx);
    }
    for (const TypeIdx tidx : func.meta.param_types) {
      if (tidx.idx >= storage.types().size()) {
        return err(VerifyErrorKind::TypeIdxOutOfRange, tidx.idx);
      }
    }
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
      const BlockParam& param = storage.block_params()[pidx];
      if (param.type.idx >= storage.types().size()) {
        return err(VerifyErrorKind::TypeIdxOutOfRange, pidx.idx);
      }
      const RegisterIdx reg = param.reg;
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
        if (operand.type.idx >= storage.types().size()) {
          return err(VerifyErrorKind::TypeIdxOutOfRange, oidx.idx);
        }
        switch (operand.tag()) {
          case Operand::TagOf<RegisterIdx>:
            if (operand.as_register().idx >= storage.registers().size()) {
              return err(VerifyErrorKind::OperandIdxOutOfRange, oidx.idx);
            }
            if (!defined[operand.as_register().idx]) {
              return err(VerifyErrorKind::UndefinedRegister,
                         operand.as_register().idx);
            }
            break;
          case Operand::TagOf<FunctionIdx>:
            if (operand.as_function().idx >= storage.functions().size()) {
              return err(VerifyErrorKind::OperandIdxOutOfRange, oidx.idx);
            }
            break;
          case Operand::TagOf<BlockIdx>:
            if (operand.as_block().idx >= storage.blocks().size()) {
              return err(VerifyErrorKind::OperandIdxOutOfRange, oidx.idx);
            }
            break;
          case Operand::TagOf<ImmutableIdx>:
            if (operand.as_immutable().idx >= storage.immutables().size()) {
              return err(VerifyErrorKind::OperandIdxOutOfRange, oidx.idx);
            }
            break;
          case Operand::TagOf<ExternalFunctionIdx>:
            if (operand.as_external_function().idx >=
                storage.external_functions().size()) {
              return err(VerifyErrorKind::OperandIdxOutOfRange, oidx.idx);
            }
            break;
          case Operand::TagOf<void>:
            return err(VerifyErrorKind::UnknownOperandTag, oidx.idx);
        }
      }
      if (instr.op == Opcode::Call) {
        if (instr.operands.empty()) {
          return err(VerifyErrorKind::InvalidCallee, iidx.idx);
        }
        const Operand& head = storage.operands()[instr.operands.head()];
        if (!head.is<FunctionIdx>() && !head.is<ExternalFunctionIdx>()) {
          return err(VerifyErrorKind::InvalidCallee, iidx.idx);
        }
        // Argument count must match the callee signature.
        const FunctionMeta& meta =
            head.is<FunctionIdx>()
                ? storage.functions()[head.as_function()].meta
                : storage.external_functions()[head.as_external_function()]
                      .meta;
        if (instr.operands.size() - 1 != meta.param_types.size()) {
          return err(VerifyErrorKind::InvalidCallee, iidx.idx);
        }
      }
      if (instr.op == Opcode::Br) {
        if (instr.operands.empty()) {
          return err(VerifyErrorKind::InvalidBranchTarget, iidx.idx);
        }
        if (!storage.operands()[instr.operands.head()].is<BlockIdx>()) {
          return err(VerifyErrorKind::InvalidBranchTarget, iidx.idx);
        }
      }
      if (instr.op == Opcode::CondBr) {
        // operands = [cond(i1), true_block, false_block]; targets take no
        // block parameters in MVP.
        if (instr.operands.size() != 3) {
          return err(VerifyErrorKind::InvalidCondBr, iidx.idx);
        }
        const Operand& cond = storage.operands()[instr.operands.head()];
        if (storage.types()[cond.type.idx].tag != TypeTag::I1) {
          return err(VerifyErrorKind::InvalidCondBr, iidx.idx);
        }
        for (u32 offset = 1; offset <= 2; ++offset) {
          const Operand& target =
              storage.operands()[instr.operands.head() + offset];
          if (!target.is<BlockIdx>()) {
            return err(VerifyErrorKind::InvalidCondBr, iidx.idx);
          }
          if (!storage.blocks()[target.as_block()].block_params.empty()) {
            return err(VerifyErrorKind::InvalidCondBr, iidx.idx);
          }
        }
      }
      if (instr.op == Opcode::Switch) {
        // operands = [value, default_block, (case_imm, case_block)...];
        // targets take no block parameters in MVP.
        if (instr.operands.size() < 2 || (instr.operands.size() % 2) != 0) {
          return err(VerifyErrorKind::InvalidSwitch, iidx.idx);
        }
        const Operand& value = storage.operands()[instr.operands.head()];
        if (!is_integer_type(storage.types()[value.type.idx].tag)) {
          return err(VerifyErrorKind::InvalidSwitch, iidx.idx);
        }
        const Operand& default_target =
            storage.operands()[instr.operands.head() + 1];
        if (!default_target.is<BlockIdx>()) {
          return err(VerifyErrorKind::InvalidSwitch, iidx.idx);
        }
        if (!storage.blocks()[default_target.as_block()].block_params.empty()) {
          return err(VerifyErrorKind::InvalidSwitch, iidx.idx);
        }
        for (u32 offset = 2; offset < instr.operands.size(); offset += 2) {
          const Operand& case_value =
              storage.operands()[instr.operands.head() + offset];
          const Operand& case_target =
              storage.operands()[instr.operands.head() + offset + 1];
          if (!case_value.is<ImmutableIdx>() || !case_target.is<BlockIdx>()) {
            return err(VerifyErrorKind::InvalidSwitch, iidx.idx);
          }
          if (!storage.blocks()[case_target.as_block()].block_params.empty()) {
            return err(VerifyErrorKind::InvalidSwitch, iidx.idx);
          }
        }
      }
      if (instr.op == Opcode::GetElementPtr) {
        // operands = [base_ptr(register), index(integer)...].
        if (instr.operands.size() < 2) {
          return err(VerifyErrorKind::InvalidGetElementPtr, iidx.idx);
        }
        if (!storage.operands()[instr.operands.head()].is<RegisterIdx>()) {
          return err(VerifyErrorKind::InvalidGetElementPtr, iidx.idx);
        }
        for (u32 offset = 1; offset < instr.operands.size(); ++offset) {
          const Operand& index =
              storage.operands()[instr.operands.head() + offset];
          if (!is_integer_type(storage.types()[index.type.idx].tag)) {
            return err(VerifyErrorKind::InvalidGetElementPtr, iidx.idx);
          }
        }
      }
      if (instr.op == Opcode::ExtractValue || instr.op == Opcode::InsertValue) {
        // Extract: [aggregate, index(imm)...]; Insert: [aggregate, value,
        // index(imm)...]. Indexes must be integer immediates.
        const u32 first_index = (instr.op == Opcode::ExtractValue) ? 1 : 2;
        if (instr.operands.size() < first_index + 1) {
          return err(VerifyErrorKind::InvalidExtractInsert, iidx.idx);
        }
        for (u32 offset = first_index; offset < instr.operands.size();
             ++offset) {
          const Operand& index =
              storage.operands()[instr.operands.head() + offset];
          if (!index.is<ImmutableIdx>() ||
              !is_integer_type(storage.types()[index.type.idx].tag)) {
            return err(VerifyErrorKind::InvalidExtractInsert, iidx.idx);
          }
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
