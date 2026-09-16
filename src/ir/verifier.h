// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace ir {

class Storage;

enum class VerifyErrorKind : u8 {
  FunctionBlocksOutOfRange,
  FunctionParamTypesOutOfRange,
  BlockInstrsOutOfRange,
  BlockParamsOutOfRange,
  BlockParamRegOutOfRange,
  InstrOperandsOutOfRange,
  InstrDstOutOfRange,
  UnknownOperandTag,
  OperandIdxOutOfRange,
  UndefinedRegister,
  RedefinedRegister,
  UnterminatedBlock,
  MisplacedTerminator,
  InvalidCallee,
  InvalidBranchTarget,
};

struct VerifyError {
  VerifyErrorKind kind;
  // The idx value of the offending entity (function, block, instruction,
  // operand, or register, depending on kind).
  u32 index;
};

constexpr std::string_view format_as(const VerifyErrorKind kind) {
  using K = VerifyErrorKind;
  switch (kind) {
    case K::FunctionBlocksOutOfRange: return "FunctionBlocksOutOfRange";
    case K::FunctionParamTypesOutOfRange: return "FunctionParamTypesOutOfRange";
    case K::BlockInstrsOutOfRange: return "BlockInstrsOutOfRange";
    case K::BlockParamsOutOfRange: return "BlockParamsOutOfRange";
    case K::BlockParamRegOutOfRange: return "BlockParamRegOutOfRange";
    case K::InstrOperandsOutOfRange: return "InstrOperandsOutOfRange";
    case K::InstrDstOutOfRange: return "InstrDstOutOfRange";
    case K::UnknownOperandTag: return "UnknownOperandTag";
    case K::OperandIdxOutOfRange: return "OperandIdxOutOfRange";
    case K::UndefinedRegister: return "UndefinedRegister";
    case K::RedefinedRegister: return "RedefinedRegister";
    case K::UnterminatedBlock: return "UnterminatedBlock";
    case K::MisplacedTerminator: return "MisplacedTerminator";
    case K::InvalidCallee: return "InvalidCallee";
    case K::InvalidBranchTarget: return "InvalidBranchTarget";
  }
}

using VerifyResult = base::Result<void, VerifyError>;

// Checks structural invariants of the storage: index range bounds, operand
// tag validity, single-definition of registers, terminator placement, and
// callee/branch target shapes. Does not type-check instructions; that is
// the analyzer's job.
VerifyResult verify_storage(const Storage& storage);

}  // namespace ir
