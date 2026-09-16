// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "fpag/base/idx.h"
#include "fpag/base/idx_range.h"
#include "fpag/base/numeric.h"

namespace ir {

using IdBaseType = base::IdxBaseType;

struct Function;
using FunctionIdx = base::Idx<Function, IdBaseType>;
using FunctionIdxRange = base::IdxRange<FunctionIdx>;
// constexpr u32 kFunctionParameterTypesSooThreshold = 8;

struct Block;
using BlockIdx = base::Idx<Block, IdBaseType>;
using BlockIdxRange = base::IdxRange<BlockIdx>;
// constexpr u32 kBlockParameterTypesSooThreshold = 20;

struct BlockParam;
using BlockParamIdx = base::Idx<BlockParam, IdBaseType>;
using BlockParamIdxRange = base::IdxRange<BlockParamIdx>;

struct Immutable;
using ImmutableIdx = base::Idx<Immutable, IdBaseType>;
using ImmutableIdxRange = base::IdxRange<ImmutableIdx>;

struct Register;
using RegisterIdx = base::Idx<Register, IdBaseType>;
using RegisterIdxRange = base::IdxRange<RegisterIdx>;

struct Instruction;
using InstructionIdx = base::Idx<Instruction, IdBaseType>;
using InstructionIdxRange = base::IdxRange<InstructionIdx>;

struct ExternalFunction;
using ExternalFunctionIdx = base::Idx<ExternalFunction, IdBaseType>;
using ExternalFunctionIdxRange = base::IdxRange<ExternalFunctionIdx>;

struct Operand;
using OperandIdx = base::Idx<Operand, IdBaseType>;
using OperandIdxRange = base::IdxRange<OperandIdx>;

enum class Type : u8;
using TypeIdx = base::Idx<Type, IdBaseType>;
using TypeIdxRange = base::IdxRange<TypeIdx>;

}  // namespace ir
