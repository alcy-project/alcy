// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace ir {

enum class Opcode : u8 {
  Noop,

  Alloca,
  Load,
  Store,
  GetElementPtr,
  // Typed element offset over a pointer of any provenance:
  // operands = [base_ptr, index(integer)]. The destination register
  // carries the element type, so codegen emits a typed GEP without
  // needing an alloca site. Backs `elem_ptr`.
  ElemOffset,
  ExtractValue,
  InsertValue,
  // Copies `len` bytes from `src` to `dst`:
  // operands = [dst_ptr, src_ptr, len(integer)], discarded value.
  Memcopy,

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
  // `TypeSizeOf` and `TypeAlignOf` take no operands and set the
  // destination to the allocation size or alignment of `measure`, the
  // type recorded on the instruction.
  TypeSizeOf,
  TypeAlignOf,

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
  // A live borrow: dst is the reference value, operands = [place].
  // Mutability reads off the dst register type (Ref vs MutRef).
  Borrow,
  // BorrowBegin,
  // BorrowEnd,

  // VecSplat,
  // VecExtract,
  // VecInsert,
  // VecReduce,

  // Explicitly wrapping integer add/sub/mul: the result is the
  // mathematical result reduced modulo 2^N, whatever the build's
  // overflow policy is. The source operations above carry that policy
  // instead (a debug build checks them, a release one wraps), so a
  // compiler-generated or library-requested wrap says so here. They
  // sit after the commented-out block because the binary form's
  // opcode byte is a stored value: a new opcode goes at the end.
  IntAddWrap,
  IntSubWrap,
  IntMulWrap,

  // The enum accesses are semantic: what they mean is the discriminant
  // or a payload field, and the representation - a tag slot or a
  // niche's spare pattern, the payload's offsets - belongs to the
  // backend, because a body whose type arguments are not known yet
  // cannot commit to a layout (ADR-0079's landing).
  // The discriminant at an enum place, as i32: operands = [place].
  EnumTagLoad,
  // Makes an enum place hold a variant: operands = [place, tag(i32)].
  // A niche-carrying enum writes its fieldless spell for the fieldless
  // variant and a zero for the carrying one, whose payload follows.
  EnumTagSet,
  // The address of payload field `field` of variant `variant` at an
  // enum place, carrying the field's type: operands = [place,
  // variant, field]. A niche-carrying enum's payload is the slot.
  EnumFieldAddr,
};

constexpr const char* opcode_to_str(const Opcode opcode) {
  using O = Opcode;
  switch (opcode) {
    case O::Noop: return "Noop";

    case O::Alloca: return "Alloca";
    case O::Load: return "Load";
    case O::Store: return "Store";
    case O::GetElementPtr: return "GetElementPtr";
    case O::ElemOffset: return "ElemOffset";
    case O::ExtractValue: return "ExtractValue";
    case O::InsertValue: return "InsertValue";
    case O::Memcopy: return "Memcopy";

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
    case O::TypeSizeOf: return "TypeSizeOf";
    case O::TypeAlignOf: return "TypeAlignOf";

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
    case O::Drop: return "Drop";
    case O::Borrow: return "Borrow";

    case O::IntAddWrap: return "IntAddWrap";
    case O::IntSubWrap: return "IntSubWrap";
    case O::IntMulWrap: return "IntMulWrap";

    case O::EnumTagLoad: return "EnumTagLoad";
    case O::EnumTagSet: return "EnumTagSet";
    case O::EnumFieldAddr:
      return "EnumFieldAddr";

      // case O::BorrowBegin: return "BorrowBegin";
      // case O::BorrowEnd: return "BorrowEnd";

      // case O::VecSplat: return "VecSplat";
      // case O::VecExtract: return "VecExtract";
      // case O::VecInsert: return "VecInsert";
      // case O::VecReduce: return "VecReduce";
  }
}

// The category a control-flow check needs: an instruction that ends its
// block. The verifier and the emitters ask the same question, so the
// table lives beside the list it names rather than once per reader.
constexpr bool is_terminator(const Opcode op) {
  return op == Opcode::Br || op == Opcode::CondBr || op == Opcode::Switch ||
         op == Opcode::Ret || op == Opcode::Unreachable;
}

}  // namespace ir
