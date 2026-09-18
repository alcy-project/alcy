// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <memory>
#include <utility>

#include "debug/dcheck.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_pool_id.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/operand.h"
#include "ir/register.h"
#include "ir/storage.h"
#include "ir/type.h"

namespace ir {

class StorageBuilder {
 public:
  StorageBuilder() { intern_primitives(); }
  explicit StorageBuilder(StorageState&& state) : state_(std::move(state)) {}

  ~StorageBuilder() = default;

  StorageBuilder(const StorageBuilder&) = delete;
  StorageBuilder& operator=(const StorageBuilder&) = delete;

  StorageBuilder(StorageBuilder&&) noexcept = default;
  StorageBuilder& operator=(StorageBuilder&&) noexcept = default;

  StorageBuilder& functions(StorageState::Functions&& functions) {
    state_.functions = std::move(functions);
    return *this;
  }
  StorageBuilder& blocks(StorageState::Blocks&& blocks) {
    state_.blocks = std::move(blocks);
    return *this;
  }
  StorageBuilder& block_params(StorageState::BlockParams&& block_params) {
    state_.block_params = std::move(block_params);
    return *this;
  }
  StorageBuilder& instrs(StorageState::Instructions&& instrs) {
    state_.instrs = std::move(instrs);
    return *this;
  }
  StorageBuilder& immutables(StorageState::Immutables&& immutables) {
    state_.immutables = std::move(immutables);
    return *this;
  }
  StorageBuilder& registers(StorageState::Registers&& registers) {
    state_.registers = std::move(registers);
    return *this;
  }
  StorageBuilder& external_functions(
      StorageState::ExternalFunctions&& external_functions) {
    state_.external_functions = std::move(external_functions);
    return *this;
  }
  StorageBuilder& operands(StorageState::Operands&& operands) {
    state_.operands = std::move(operands);
    return *this;
  }
  StorageBuilder& parameter_types(StorageState::Types&& parameter_types) {
    state_.types = std::move(parameter_types);
    return *this;
  }

  FunctionIdx function(Function function) {
    return state_.functions.emplace_back(function);
  }
  BlockIdx block(Block block) { return state_.blocks.emplace_back(block); }

  // Backpatches a block's instruction range. Branch targets must exist before
  // the branching instruction can reference them, while LLVM requires the
  // entry block first in vector order; declare the entry block empty, build
  // the targets, then patch the entry.
  void set_block_instrs(BlockIdx idx, InstructionIdxRange instrs) {
    DCHECK(idx.idx < state_.blocks.size());
    state_.blocks[idx].instrs = instrs;
  }

  void set_block_params(BlockIdx idx, BlockParamIdxRange params) {
    DCHECK(idx.idx < state_.blocks.size());
    state_.blocks[idx].block_params = params;
  }
  BlockParamIdx block_param(BlockParam block_param) {
    return state_.block_params.emplace_back(block_param);
  }
  InstructionIdx instr(Instruction instr) {
    return state_.instrs.emplace_back(instr);
  }
  ImmutableIdx immutable(Immutable immutable) {
    return state_.immutables.emplace_back(immutable);
  }
  RegisterIdx reg(Register reg) { return state_.registers.emplace_back(reg); }
  ExternalFunctionIdx external_function(ExternalFunction external_function) {
    return state_.external_functions.emplace_back(external_function);
  }
  OperandIdx operand(Operand&& operand) {
    return state_.operands.emplace_back(std::move(operand));
  }
  TypeIdx type(TypeTag tag) {
    DCHECK(tag != TypeTag::Struct && tag != TypeTag::Array);
    return primitive(tag);
  }

  // Returns the pre-interned node for a non-composite tag. O(1), no
  // allocation. Struct and Array nodes are created via struct_type() and
  // array_type() instead.
  TypeIdx primitive(TypeTag tag) const {
    DCHECK(tag != TypeTag::Struct && tag != TypeTag::Array);
    return primitive_idx(tag);
  }

  TypeIdx struct_type(str::StringPoolId name, TypeIdxRange fields) {
    TypeNode node{};
    node.tag = TypeTag::Struct;
    node.data.set(state_.struct_types.emplace_back(
        StructType{.name = name, .fields = fields}));
    return state_.types.emplace_back(node);
  }

  TypeIdx array_type(TypeIdx element, u64 count) {
    TypeNode node{};
    node.tag = TypeTag::Array;
    node.data.set(state_.array_types.emplace_back(
        ArrayType{.element = element, .count = count}));
    return state_.types.emplace_back(node);
  }

  // Appends a copy of an existing type entry and returns the new index.
  // Useful for building field lists that reference the same type twice.
  TypeIdx ref_type(TypeIdx idx) {
    DCHECK(idx.idx < state_.types.size());
    return state_.types.emplace_back(state_.types[idx]);
  }

  Storage build() && { return Storage(std::move(state_)); }

  std::unique_ptr<Storage> build_unique() && {
    return std::make_unique<Storage>(std::move(*this).build());
  }

 private:
  // Pre-interns every non-composite tag so primitive() resolves without a
  // table lookup. Only the default constructor does this; a builder created
  // from an existing state assumes its table is already populated.
  void intern_primitives() {
    for (u32 i = 0; i < kPrimitiveTypeCount; ++i) {
      TypeNode node{};
      node.tag = static_cast<TypeTag>(i);
      state_.types.emplace_back(node);
    }
    TypeNode func{};
    func.tag = TypeTag::Function;
    state_.types.emplace_back(func);
  }

  StorageState state_;
};

}  // namespace ir
