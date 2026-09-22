// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/idx.h"
#include "fpag/base/idx_range.h"
#include "fpag/base/numeric.h"

namespace ir {

namespace details {
template <typename T>
using Idx = base::Idx<T, base::IdxBaseType>;
}

struct Function;
using FunctionIdx = details::Idx<Function>;
using FunctionIdxRange = base::IdxRange<FunctionIdx>;
// constexpr u32 kFunctionParameterTypesSooThreshold = 8;

struct Block;
using BlockIdx = details::Idx<Block>;
using BlockIdxRange = base::IdxRange<BlockIdx>;
// constexpr u32 kBlockParameterTypesSooThreshold = 20;

struct BlockParam;
using BlockParamIdx = details::Idx<BlockParam>;
using BlockParamIdxRange = base::IdxRange<BlockParamIdx>;

struct Immutable;
using ImmutableIdx = details::Idx<Immutable>;
using ImmutableIdxRange = base::IdxRange<ImmutableIdx>;

struct Register;
using RegisterIdx = details::Idx<Register>;
using RegisterIdxRange = base::IdxRange<RegisterIdx>;

struct Instruction;
using InstructionIdx = details::Idx<Instruction>;
using InstructionIdxRange = base::IdxRange<InstructionIdx>;

struct ExternalFunction;
using ExternalFunctionIdx = details::Idx<ExternalFunction>;
using ExternalFunctionIdxRange = base::IdxRange<ExternalFunctionIdx>;

struct Operand;
using OperandIdx = details::Idx<Operand>;
using OperandIdxRange = base::IdxRange<OperandIdx>;

enum class TypeTag : u8;
struct TypeNode;
using TypeIdx = details::Idx<TypeNode>;
using TypeIdxRange = base::IdxRange<TypeIdx>;

struct StructType;
using StructTypeIdx = details::Idx<StructType>;

struct ArrayType;
using ArrayTypeIdx = details::Idx<ArrayType>;

struct EnumType;
using EnumTypeIdx = details::Idx<EnumType>;

struct EnumVariantType;
using EnumVariantTypeIdx = details::Idx<EnumVariantType>;
using EnumVariantTypeIdxRange = base::IdxRange<EnumVariantTypeIdx>;

struct RefType;
using RefTypeIdx = details::Idx<RefType>;

struct TupleType;
using TupleTypeIdx = details::Idx<TupleType>;

}  // namespace ir
