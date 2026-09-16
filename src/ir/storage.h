// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fpag/base/numeric.h"
#include "fpag/base/vec.h"
#include "ir/block.h"
#include "ir/block_param.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction.h"
#include "ir/operand.h"
#include "ir/register.h"

namespace ir {

struct StorageState {
  template <typename T>
  using Alloc = std::allocator<T>;

  using Functions = base::Vec<Function, FunctionIdx, Alloc<Function>>;
  using Blocks = base::Vec<Block, BlockIdx, Alloc<Block>>;
  using BlockParams = base::Vec<BlockParam, BlockParamIdx, Alloc<BlockParam>>;
  using Instructions =
      base::Vec<Instruction, InstructionIdx, Alloc<Instruction>>;
  using Immutables = base::Vec<Immutable, ImmutableIdx, Alloc<Immutable>>;
  using Registers = base::Vec<Register, RegisterIdx, Alloc<Register>>;
  using ExternalFunctions =
      base::Vec<ExternalFunction, ExternalFunctionIdx, Alloc<ExternalFunction>>;
  using Operands = base::Vec<Operand, OperandIdx, Alloc<Operand>>;
  using Types = base::Vec<Type, TypeIdx, Alloc<Type>>;

  Functions functions;
  Blocks blocks;
  BlockParams block_params;
  Instructions instrs;
  Immutables immutables;
  Registers registers;
  ExternalFunctions external_functions;
  Operands operands;
  Types types;
};

class Storage {
 public:
  explicit Storage(StorageState&& state) : state_(std::move(state)) {}
  ~Storage() = default;

  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;

  Storage(Storage&&) noexcept = default;
  Storage& operator=(Storage&&) noexcept = default;

  const StorageState& state() const { return state_; }

  const StorageState::Functions& functions() const { return state_.functions; }
  const StorageState::Blocks& blocks() const { return state_.blocks; }
  const StorageState::BlockParams& block_params() const {
    return state_.block_params;
  }
  const StorageState::Instructions& instrs() const { return state_.instrs; }
  const StorageState::Immutables& immutables() const {
    return state_.immutables;
  }
  const StorageState::Registers& registers() const { return state_.registers; }
  const StorageState::ExternalFunctions& external_functions() const {
    return state_.external_functions;
  }
  const StorageState::Operands& operands() const { return state_.operands; }
  const StorageState::Types& types() const { return state_.types; }

 private:
  StorageState state_;
};

}  // namespace ir
