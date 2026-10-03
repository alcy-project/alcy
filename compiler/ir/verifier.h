// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "diag/diagnostic.h"
#include "diag/stage.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace ir {

class Storage;

enum class VerificationErrorKind : u8 {
  FunctionBlocksOutOfRange,
  FunctionParamTypesOutOfRange,
  BlockInstrsOutOfRange,
  BlockParamsOutOfRange,
  BlockParamRegOutOfRange,
  InstrOperandsOutOfRange,
  InstrDstOutOfRange,
  UnknownOperandTag,
  OperandIdxOutOfRange,
  TypeIdxOutOfRange,
  StructFieldsOutOfRange,
  TypeMetadataOutOfRange,
  UndefinedRegister,
  RedefinedRegister,
  UnterminatedBlock,
  MisplacedTerminator,
  InvalidCallee,
  InvalidBranchTarget,
  InvalidCondBr,
  InvalidSwitch,
  InvalidGetElementPtr,
  InvalidExtractInsert,
  InvalidBorrow,
  InvalidMemcopy,
  InvalidTypeQuery,
  EnumFieldsOutOfRange,
  TupleFieldsOutOfRange,
  FuncFieldsOutOfRange,
};

struct VerificationError {
  VerificationErrorKind kind;
  // The idx value of the offending entity (function, block, instruction,
  // operand, or register, depending on kind).
  u32 index;
};

constexpr std::string_view format_as(const VerificationErrorKind kind) {
  using K = VerificationErrorKind;
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
    case K::TypeIdxOutOfRange: return "TypeIdxOutOfRange";
    case K::StructFieldsOutOfRange: return "StructFieldsOutOfRange";
    case K::TypeMetadataOutOfRange: return "TypeMetadataOutOfRange";
    case K::UndefinedRegister: return "UndefinedRegister";
    case K::RedefinedRegister: return "RedefinedRegister";
    case K::UnterminatedBlock: return "UnterminatedBlock";
    case K::MisplacedTerminator: return "MisplacedTerminator";
    case K::InvalidCallee: return "InvalidCallee";
    case K::InvalidBranchTarget: return "InvalidBranchTarget";
    case K::InvalidCondBr: return "InvalidCondBr";
    case K::InvalidSwitch: return "InvalidSwitch";
    case K::InvalidGetElementPtr: return "InvalidGetElementPtr";
    case K::InvalidExtractInsert: return "InvalidExtractInsert";
    case K::InvalidBorrow: return "InvalidBorrow";
    case K::InvalidMemcopy: return "InvalidMemcopy";
    case K::InvalidTypeQuery: return "InvalidTypeQuery";
    case K::EnumFieldsOutOfRange: return "EnumFieldsOutOfRange";
    case K::TupleFieldsOutOfRange: return "TupleFieldsOutOfRange";
    case K::FuncFieldsOutOfRange: return "FuncFieldsOutOfRange";
  }
}

using VerificationResult = base::Result<void, VerificationError>;

// Converts a structural error into a span-less diagnostic. `ir` owns no
// enum of its own: a code here is the VerificationErrorKind's ordinal,
// shifted by one because ids start at 1, so a kind and its code cannot
// drift apart and adding a kind needs no renumbering. The message is the
// kind name. Human-friendly texts arrive with later phases that know
// source locations.
inline diag::Diagnostic to_diagnostic(const VerificationError& error) {
  const u8 id = static_cast<u8>(error.kind) + 1;
  return diag::Diagnostic{
      .severity = diag::Severity::Error,
      .code = diag::Code{diag::Stage::Ir, id},
      .message = format_as(error.kind),
      .primary_span = {},
  };
}

// Checks structural invariants of the storage: index range bounds, operand
// tag validity, single-definition of registers, terminator placement, and
// callee/branch target shapes. Does not type-check instructions; that is
// the analyzer's job.
VerificationResult verify_storage(const Storage& storage);

}  // namespace ir
