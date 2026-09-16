// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "debug/dcheck.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/union.h"
#include "ir/common.h"
#include "ir/type.h"

namespace ir {

enum class OperandTag : u8 {
  Register,
  Function,
  Block,
  Immutable,
  ExternalFunction,
  Unknown,
};

struct Operand {
  base::Union<RegisterIdx,
              FunctionIdx,
              BlockIdx,
              ImmutableIdx,
              ExternalFunctionIdx>
      data;
  OperandTag tag;
  Type type;

  static constexpr Operand from_register(RegisterIdx idx, Type type) {
    Operand operand{};
    operand.data.set(idx);
    operand.tag = OperandTag::Register;
    operand.type = type;
    return operand;
  }

  static constexpr Operand from_function(FunctionIdx idx, Type type) {
    Operand operand{};
    operand.data.set(idx);
    operand.tag = OperandTag::Function;
    operand.type = type;
    return operand;
  }

  static constexpr Operand from_block(BlockIdx idx, Type type) {
    Operand operand{};
    operand.data.set(idx);
    operand.tag = OperandTag::Block;
    operand.type = type;
    return operand;
  }

  static constexpr Operand from_immutable(ImmutableIdx idx, Type type) {
    Operand operand{};
    operand.data.set(idx);
    operand.tag = OperandTag::Immutable;
    operand.type = type;
    return operand;
  }

  static constexpr Operand from_external_function(ExternalFunctionIdx idx,
                                                  Type type) {
    Operand operand{};
    operand.data.set(idx);
    operand.tag = OperandTag::ExternalFunction;
    operand.type = type;
    return operand;
  }

  static constexpr Operand invalid() {
    Operand operand{};
    operand.data.set(RegisterIdx(base::kInvalidIdx));
    operand.tag = OperandTag::Unknown;
    operand.type = Type::Void;
    return operand;
  }

  inline RegisterIdx as_register() const {
    DCHECK_MSG(tag == OperandTag::Register, "operand is not a register");
    return data.get<RegisterIdx>();
  }

  inline FunctionIdx as_function() const {
    DCHECK_MSG(tag == OperandTag::Function, "operand is not a function");
    return data.get<FunctionIdx>();
  }

  inline BlockIdx as_block() const {
    DCHECK_MSG(tag == OperandTag::Block, "operand is not a block");
    return data.get<BlockIdx>();
  }

  inline ImmutableIdx as_immutable() const {
    DCHECK_MSG(tag == OperandTag::Immutable, "operand is not an immutable");
    return data.get<ImmutableIdx>();
  }

  inline ExternalFunctionIdx as_external_function() const {
    DCHECK_MSG(tag == OperandTag::ExternalFunction,
               "operand is not an external function");
    return data.get<ExternalFunctionIdx>();
  }
};

constexpr Operand kInvalidOperand = Operand::invalid();

}  // namespace ir
