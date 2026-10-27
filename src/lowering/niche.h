// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "fpag/base/numeric.h"
#include "ir/instruction.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "lowering/immediate.h"

namespace lowering {

// A null reference of the given reference type: a pointer-width zero,
// spelled as a pointer and then as the reference. A niche-carrying enum
// spells its fieldless variant with one (ADR-0087), and a reader asks
// whether a place holds that spell.
template <class Emit>
ir::RegisterIdx emit_null_reference(ir::StorageBuilder& builder,
                                    ir::TypeIdx ref_type,
                                    Emit&& emit) {
  // The zero is an integer before it is an address: a Ptr-typed
  // immutable has no constant form in the emitters, and the integer to
  // pointer cast is the one both of them carry.
  const ir::TypeIdx u64 = ir::primitive_idx(ir::TypeTag::U64);
  const ir::TypeIdx ptr = ir::primitive_idx(ir::TypeTag::Ptr);
  const ir::RegisterIdx zero = emit(ir::Opcode::TypeCast, ptr,
                                    std::vector<ir::OperandIdx>{imm_from_u64(
                                        builder, ir::TypeTag::U64, u64, 0)});
  return emit(ir::Opcode::TypeCast, ref_type,
              std::vector<ir::OperandIdx>{
                  builder.operand(ir::Operand::from_register(zero, ptr))});
}

// The discriminant of a niche-carrying enum, synthesized from the
// payload's spell in the spare pattern (ADR-0087): load the slot - the
// payload for a thin reference, the byte for a bool - ask whether the
// pattern is the fieldless spell, and step from the carrying variant's
// index by the answer, because the two variants are one apart.
//
// The lowering and the drop pass both read a place's discriminant, and
// both must read it the same way: the sequence lives here once, and
// `emit` is the caller's instruction sink.
template <class Emit>
ir::RegisterIdx niche_discriminant(ir::StorageBuilder& builder,
                                   const ir::EnumNiche& niche,
                                   ir::OperandIdx slot,
                                   ir::TypeIdx i32_ty,
                                   Emit&& emit) {
  const auto operand = [&](ir::RegisterIdx reg, ir::TypeIdx type) {
    return builder.operand(ir::Operand::from_register(reg, type));
  };
  const ir::RegisterIdx loaded =
      emit(ir::Opcode::Load, niche.slot, std::vector<ir::OperandIdx>{slot});
  const ir::TypeIdx i1 = ir::primitive_idx(ir::TypeTag::I1);
  ir::RegisterIdx fieldless = ir::RegisterIdx(base::INVALID_IDX);
  if (niche.slot == niche.payload) {
    // A thin reference: the null pattern is the fieldless spell.
    const ir::RegisterIdx null_ref =
        emit_null_reference(builder, niche.payload, emit);
    fieldless =
        emit(ir::Opcode::Eq, i1,
             std::vector<ir::OperandIdx>{operand(loaded, niche.payload),
                                         operand(null_ref, niche.payload)});
  } else {
    // A bool payload rides a byte: anything above one is the fieldless
    // spell.
    fieldless = emit(ir::Opcode::Gt, i1,
                     std::vector<ir::OperandIdx>{
                         operand(loaded, niche.slot),
                         imm_from_u64(builder, ir::TypeTag::U8, niche.slot,
                                      static_cast<u64>(niche.sentinel) - 1)});
  }
  const ir::RegisterIdx step =
      emit(ir::Opcode::TypeCast, i32_ty,
           std::vector<ir::OperandIdx>{operand(fieldless, i1)});
  const bool fieldless_first = niche.fieldless < niche.carrying;
  return emit(fieldless_first ? ir::Opcode::IntSub : ir::Opcode::IntAdd, i32_ty,
              std::vector<ir::OperandIdx>{
                  imm_from_u64(builder, ir::TypeTag::I32, i32_ty,
                               static_cast<u64>(niche.carrying)),
                  operand(step, i32_ty)});
}

}  // namespace lowering
