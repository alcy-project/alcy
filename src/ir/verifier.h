// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "diag/diagnostic.h"
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
  InvalidMemcpy,
  EnumFieldsOutOfRange,
  TupleFieldsOutOfRange,
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
    case K::InvalidMemcpy: return "InvalidMemcpy";
    case K::EnumFieldsOutOfRange: return "EnumFieldsOutOfRange";
    case K::TupleFieldsOutOfRange: return "TupleFieldsOutOfRange";
  }
}

using VerifyResult = base::Result<void, VerifyError>;

// Converts a structural error into a span-less diagnostic. Codes 1000-1999
// are reserved for IR verification; the message is the kind name.
// Human-friendly texts arrive with later phases that know source locations.
inline diag::Diagnostic to_diagnostic(const VerifyError& error) {
  return diag::Diagnostic{
      .severity = diag::Severity::Error,
      .code = 1000 + static_cast<u32>(error.kind),
      .message = format_as(error.kind),
      .primary_span = {},
  };
}

// Checks structural invariants of the storage: index range bounds, operand
// tag validity, single-definition of registers, terminator placement, and
// callee/branch target shapes. Does not type-check instructions; that is
// the analyzer's job.
VerifyResult verify_storage(const Storage& storage);

}  // namespace ir
