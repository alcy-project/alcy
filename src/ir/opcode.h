// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "fpag/base/numeric.h"

namespace ir {

enum class Opcode : u8 {
  Noop,

  Alloca,
  Load,
  Store,
  GetElementPtr,
  ExtractValue,
  InsertValue,

  IntAdd,
  IntSub,
  IntMul,
  IntDiv,   // Signed division
  UintDiv,  // Unsigned division
  IntRem,   // Signed remainder
  UintRem,  // Unsigned remainder

  FAdd,
  FSub,
  FMul,
  FDiv,

  And,
  Or,
  Xor,
  ShiftLeft,
  ArithmeticShiftRight,
  LogicalShiftRight,
  Not,
  BitReverse,

  Eq,
  Ne,
  Le,
  Lt,
  Ge,
  Gt,

  TypeCast,

  Select,

  Br,
  CondBr,
  Switch,
  Call,
  Ret,
  Unreachable,

  AtomicLoad,
  AtomicStore,
  AtomicRmw,
  AtomicCompareExchange,
  Fence,

  Move,
  Drop,
  // BorrowBegin,
  // BorrowEnd,

  // VecSplat,
  // VecExtract,
  // VecInsert,
  // VecReduce,
};

constexpr const char* opcode_to_str(const Opcode opcode) {
  using O = Opcode;
  switch (opcode) {
    case O::Noop: return "Noop";

    case O::Alloca: return "Alloca";
    case O::Load: return "Load";
    case O::Store: return "Store";
    case O::GetElementPtr: return "GetElementPtr";
    case O::ExtractValue: return "ExtractValue";
    case O::InsertValue: return "InsertValue";

    case O::IntAdd: return "IntAdd";
    case O::IntSub: return "IntSub";
    case O::IntMul: return "IntMul";
    case O::IntDiv: return "IntDiv";
    case O::UintDiv: return "UintDiv";
    case O::IntRem: return "IntRem";
    case O::UintRem: return "UintRem";

    case O::FAdd: return "FAdd";
    case O::FSub: return "FSub";
    case O::FMul: return "FMul";
    case O::FDiv: return "FDiv";

    case O::And: return "And";
    case O::Or: return "Or";
    case O::Xor: return "Xor";
    case O::ShiftLeft: return "ShiftLeft";
    case O::ArithmeticShiftRight: return "ArithmeticShiftRight";
    case O::LogicalShiftRight: return "LogicalShiftRight";
    case O::Not: return "Not";
    case O::BitReverse: return "BitReverse";

    case O::Eq: return "Eq";
    case O::Ne: return "Ne";
    case O::Le: return "Le";
    case O::Lt: return "Lt";
    case O::Ge: return "Ge";
    case O::Gt: return "Gt";

    case O::TypeCast: return "TypeCast";

    case O::Select: return "Select";

    case O::Br: return "Br";
    case O::CondBr: return "CondBr";
    case O::Switch: return "Switch";
    case O::Call: return "Call";
    case O::Ret: return "Ret";
    case O::Unreachable: return "Unreachable";

    case O::AtomicLoad: return "AtomicLoad";
    case O::AtomicStore: return "AtomicStore";
    case O::AtomicRmw: return "AtomicRmw";
    case O::AtomicCompareExchange: return "AtomicCompareExchange";

    case O::Fence: return "Fence";

    case O::Move: return "Move";
    case O::Drop:
      return "Drop";

      // case O::BorrowBegin: return "BorrowBegin";
      // case O::BorrowEnd: return "BorrowEnd";

      // case O::VecSplat: return "VecSplat";
      // case O::VecExtract: return "VecExtract";
      // case O::VecInsert: return "VecInsert";
      // case O::VecReduce: return "VecReduce";
  }
}

}  // namespace ir
