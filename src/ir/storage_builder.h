// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <memory>
#include <utility>

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
  StorageBuilder() = default;
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
  OperandIdx operand(Operand operand) {
    return state_.operands.emplace_back(operand);
  }
  TypeIdx type(Type type) { return state_.types.emplace_back(type); }

  Storage build() && { return Storage(std::move(state_)); }

  std::unique_ptr<Storage> build_unique() && {
    return std::make_unique<Storage>(std::move(*this).build());
  }

 private:
  StorageState state_;
};

}  // namespace ir
