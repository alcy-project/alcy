// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "fpag/base/idx.h"
#include "fpag/base/tagged_union.h"
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
  OperandTag tag;
  Type type;

  union {
    RegisterIdx register_idx;
    FunctionIdx function_idx;
    BlockIdx block_idx;
    ImmutableIdx immutable_idx;
    ExternalFunctionIdx external_function_idx;
  } data;
};

constexpr Operand kInvalidOperand = {
    .tag = OperandTag::Unknown,
    .type = Type::Void,
    .data = {.register_idx = RegisterIdx(base::kInvalidIdx)},
};

}  // namespace ir
