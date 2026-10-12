// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/fmt.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "comp/comp_value.h"
#include "diag/span.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/opcode.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/type_util.h"
#include "lowering/lowerer.h"

namespace lowering {

Val Lowerer::materialize_comp_value(const CompVal& value, diag::Span span) {
  const ir::TypeTag tag = tag_of(value.type);
  switch (value.value.tag) {
    case CompValue::Tag::Int:
    case CompValue::Tag::Bool: {
      const u64 bits = value.value.tag == CompValue::Tag::Bool
                           ? (value.value.bool_value ? 1 : 0)
                           : value.value.int_value;
      return Val{imm_from_u64(tag, value.type, bits), value.type, false, false};
    }
    case CompValue::Tag::Str: {
      const str::StringPoolId id = intern_copied(value.value.str_value);
      if (id == str::INVALID_STRING_POOL_ID) {
        return Val{size_one, error_type(), false, false};
      }
      const ir::ImmutableIdx imm =
          builder.immutable({.type = value.type, .data = {.str_id_value = id}});
      return Val{to_operand(imm, value.type), value.type, false, false};
    }
    case CompValue::Tag::Void: return Val{size_one, value.type, false, false};
    case CompValue::Tag::Tuple: {
      const ir::TupleType& shape =
          builder.state()
              .tuple_types[builder.state().types[value.type].as_tuple()];
      if (shape.elements.size() != value.value.fields.size()) {
        internal(span, "comp tuple arity");
        return Val{size_one, error_type(), false, false};
      }
      const ir::RegisterIdx addr =
          emit(ir::Opcode::Alloca, value.type, {size_one});
      for (u32 i = 0; i < static_cast<u32>(shape.elements.size()); ++i) {
        const CompVal field{value.value.fields[i], shape.elements[i]};
        Val lowered = materialize_comp_value(field, span);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::RegisterIdx gep =
            emit(ir::Opcode::GetElementPtr, shape.elements[i],
                 {to_operand(addr, value.type), zero_i32, index_operand(i)});
        emit_void(ir::Opcode::Store,
                  {use_value(lowered), to_operand(gep, shape.elements[i])});
      }
      return Val{to_operand(addr, value.type), value.type, true, false};
    }
    case CompValue::Tag::Array: {
      const ir::ArrayType& shape =
          builder.state()
              .array_types[builder.state().types[value.type].as_array()];
      if (shape.count != value.value.fields.size()) {
        internal(span, "comp array arity");
        return Val{size_one, error_type(), false, false};
      }
      const ir::RegisterIdx addr =
          emit(ir::Opcode::Alloca, value.type, {size_one});
      for (u32 i = 0; i < static_cast<u32>(shape.count); ++i) {
        const CompVal field{value.value.fields[i], shape.element};
        Val lowered = materialize_comp_value(field, span);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::RegisterIdx gep =
            emit(ir::Opcode::GetElementPtr, shape.element,
                 {to_operand(addr, value.type), zero_i32, index_operand(i)});
        emit_void(ir::Opcode::Store,
                  {use_value(lowered), to_operand(gep, shape.element)});
      }
      return Val{to_operand(addr, value.type), value.type, true, false};
    }
    case CompValue::Tag::Struct: {
      const auto* info = struct_info(value.type);
      if (info == nullptr || info->fields.size() != value.value.fields.size()) {
        internal(span, "comp struct arity");
        return Val{size_one, error_type(), false, false};
      }
      const ir::RegisterIdx addr =
          emit(ir::Opcode::Alloca, value.type, {size_one});
      for (u32 i = 0; i < static_cast<u32>(info->fields.size()); ++i) {
        const ir::TypeIdx field_type = field_type_of(value.type, i, span);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const CompVal field{value.value.fields[i], field_type};
        Val lowered = materialize_comp_value(field, span);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        const ir::RegisterIdx gep =
            emit(ir::Opcode::GetElementPtr, field_type,
                 {to_operand(addr, value.type), zero_i32, index_operand(i)});
        emit_void(ir::Opcode::Store,
                  {use_value(lowered), to_operand(gep, field_type)});
      }
      return Val{to_operand(addr, value.type), value.type, true, false};
    }
    case CompValue::Tag::Enum: {
      const std::vector<ir::TypeIdx> payloads =
          variant_payload(value.type, value.value.variant);
      if (payloads.size() != value.value.fields.size()) {
        internal(span, "comp variant arity");
        return Val{size_one, error_type(), false, false};
      }
      const ir::TypeIdx slot = enum_slot_type(value.type);
      const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, slot, {size_one});
      const ir::RegisterIdx tag_reg =
          emit(ir::Opcode::GetElementPtr, builder.primitive(ir::TypeTag::I32),
               {to_operand(addr, slot), zero_i32, index_operand(0)});
      emit_void(ir::Opcode::Store,
                {disc_operand(value.value.variant), to_operand(tag_reg, slot)});
      if (!payloads.empty() && !is_unit_payload(payloads)) {
        const ir::TypeIdx payload_type = payload_tuple(payloads);
        const ir::RegisterIdx payload =
            emit(ir::Opcode::Alloca, payload_type, {size_one});
        for (usize i = 0; i < payloads.size(); ++i) {
          const CompVal field{value.value.fields[i], payloads[i]};
          Val lowered = materialize_comp_value(field, span);
          if (failed) {
            return Val{size_one, error_type(), false, false};
          }
          const ir::RegisterIdx field_reg =
              emit(ir::Opcode::GetElementPtr, payloads[i],
                   {to_operand(payload, payload_type), zero_i32,
                    index_operand(static_cast<u32>(i))});
          emit_void(ir::Opcode::Store,
                    {use_value(lowered), to_operand(field_reg, payloads[i])});
        }
        const ir::TypeIdx ptr = builder.primitive(ir::TypeTag::Ptr);
        const ir::RegisterIdx slot_field =
            emit(ir::Opcode::GetElementPtr, ptr,
                 {to_operand(addr, slot), zero_i32, index_operand(1)});
        emit_void(ir::Opcode::Store,
                  {to_operand(payload, ptr), to_operand(slot_field, ptr)});
      }
      return Val{to_operand(addr, slot), value.type, true, false};
    }
  }
}

// Top-level comp evaluation: the evaluator gets the current
// instantiation as its seed and the persistent comp bindings as the
// outer scope; a failure is reported where the lowering reports.
bool Lowerer::comp_evaluate(u32 mod, ast::ExprIdx expr, CompVal& out) {
  comp_.clear_failure();
  if (!comp_.evaluate(mod, expr, cur_inst_, &comp_scope_, out)) {
    report_comp_failure();
    return false;
  }
  return true;
}

bool Lowerer::comp_evaluate_item(u32 mod, ast::ExprIdx init, CompVal& out) {
  comp_.clear_failure();
  if (!comp_.evaluate_item(mod, init, cur_inst_, out)) {
    report_comp_failure();
    return false;
  }
  return true;
}

bool Lowerer::comp_bind_pattern(u32 mod,
                                ast::PatternIdx pattern,
                                const CompVal& value,
                                CompScope& scope,
                                diag::Span span) {
  comp_.clear_failure();
  if (!comp_.bind_pattern(mod, pattern, value, scope, span)) {
    report_comp_failure();
    return false;
  }
  return true;
}

void Lowerer::report_comp_failure() {
  if (comp_.failure_is_internal()) {
    internal(comp_.failure_span(), comp_.failure());
    return;
  }
  unsupported(comp_.failure_span(), comp_.failure());
}

bool Lowerer::check_fmt_arguments(diag::Span span,
                                  const analyzer::FmtTemplate& parsed,
                                  const std::vector<ir::TypeIdx>& elem_types) {
  for (const analyzer::FmtPiece& piece : parsed.pieces) {
    if (!piece.is_arg) {
      continue;
    }
    const ir::TypeIdx element = elem_types[piece.arg];
    const ir::TypeTag tag = tag_of(element);
    if (!analyzer::is_formattable_tag(tag)) {
      // A nominal formats through `fmt`'s `Display`; the recorded call
      // is what the expansion reads, and a missing one is reported
      // where the piece is emitted.
      if (tag != ir::TypeTag::Struct && tag != ir::TypeTag::Enum) {
        unsupported(span, "argument is not formattable");
        return false;
      }
    }
    if (!analyzer::fmt_piece_matches_argument(piece, tag)) {
      unsupported(span, "format precision applies to floats only");
      return false;
    }
  }
  return true;
}

bool Lowerer::emit_fmt_pieces(diag::Span span,
                              const std::vector<analyzer::FmtPiece>& pieces,
                              ir::OperandIdx tup_op,
                              const std::vector<ir::TypeIdx>& elem_types,
                              std::span<const ast::ExprIdx> arg_exprs,
                              ir::OperandIdx dst_base,
                              ir::OperandIdx capacity,
                              FmtState& state,
                              bool measure_only) {
  const ir::TypeIdx usize_ty = usize_type();
  const ir::TypeIdx u8_ty = builder.primitive(ir::TypeTag::U8);
  const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
  auto usize_imm = [&](u64 value) {
    return to_operand(
        builder.immutable({.type = usize_ty, .data = {.u64_value = value}}),
        usize_ty);
  };
  state.capacity = capacity;
  state.off_addr = emit(ir::Opcode::Alloca, usize_ty, {size_one});
  emit_void(ir::Opcode::Store,
            {usize_imm(0), to_operand(state.off_addr, usize_ty)});
  state.tot_addr = emit(ir::Opcode::Alloca, usize_ty, {size_one});
  emit_void(ir::Opcode::Store,
            {usize_imm(0), to_operand(state.tot_addr, usize_ty)});
  auto advance = [&](ir::OperandIdx delta) {
    const ir::RegisterIdx off = emit(ir::Opcode::Load, usize_ty,
                                     {to_operand(state.off_addr, usize_ty)});
    const ir::RegisterIdx grown =
        emit(ir::Opcode::IntAdd, usize_ty, {to_operand(off, usize_ty), delta});
    emit_void(ir::Opcode::Store, {to_operand(grown, usize_ty),
                                  to_operand(state.off_addr, usize_ty)});
  };
  auto accumulate = [&](ir::OperandIdx full) {
    const ir::RegisterIdx total = emit(ir::Opcode::Load, usize_ty,
                                       {to_operand(state.tot_addr, usize_ty)});
    const ir::RegisterIdx grown =
        emit(ir::Opcode::IntAdd, usize_ty, {to_operand(total, usize_ty), full});
    emit_void(ir::Opcode::Store, {to_operand(grown, usize_ty),
                                  to_operand(state.tot_addr, usize_ty)});
  };
  // Copies [src, len), bounded by the buffer: only chunk reaches
  // the buffer, while full always accrues to the total. Measuring
  // skips the copy and keeps the total, so a first pass learns the
  // exact size a growable buffer has to reserve.
  auto bounded_copy = [&](ir::OperandIdx src, ir::OperandIdx len,
                          ir::OperandIdx full) {
    if (measure_only) {
      accumulate(full);
      return !failed;
    }
    const ir::RegisterIdx off = emit(ir::Opcode::Load, usize_ty,
                                     {to_operand(state.off_addr, usize_ty)});
    const ir::RegisterIdx remaining =
        emit(ir::Opcode::IntSub, usize_ty,
             {state.capacity, to_operand(off, usize_ty)});
    const ir::RegisterIdx fits =
        emit(ir::Opcode::Lt, boolean, {len, to_operand(remaining, usize_ty)});
    const ir::RegisterIdx chunk =
        emit(ir::Opcode::Select, usize_ty,
             {to_operand(fits, boolean), len, to_operand(remaining, usize_ty)});
    const ir::OperandIdx dst =
        advance_ptr(dst_base, to_operand(off, usize_ty), span);
    if (failed) {
      return false;
    }
    emit_void(ir::Opcode::Memcopy, {dst, src, to_operand(chunk, usize_ty)});
    advance(to_operand(chunk, usize_ty));
    accumulate(full);
    return !failed;
  };
  auto const_str = [&](const std::string& bytes, ir::OperandIdx& ptr_out,
                       ir::OperandIdx& len_out) {
    const str::StringPoolId id = intern_copied(bytes);
    if (id == str::INVALID_STRING_POOL_ID) {
      return false;
    }
    const ir::ImmutableIdx imm =
        builder.immutable({.type = builder.primitive(ir::TypeTag::Str),
                           .data = {.str_id_value = id}});
    const ir::OperandIdx str_op =
        to_operand(imm, builder.primitive(ir::TypeTag::Str));
    const ir::RegisterIdx ptr =
        emit(ir::Opcode::ExtractValue, builder.primitive(ir::TypeTag::Ptr),
             {str_op, index_operand(0)});
    if (failed) {
      return false;
    }
    const ir::RegisterIdx len =
        emit(ir::Opcode::ExtractValue, usize_ty, {str_op, index_operand(1)});
    if (failed) {
      return false;
    }
    ptr_out = to_operand(ptr, builder.primitive(ir::TypeTag::Ptr));
    len_out = to_operand(len, usize_ty);
    return true;
  };
  // A one-byte piece: the byte when `present`, nothing otherwise. A
  // zero-length copy is what drops it, so the two-byte buffer holds
  // the byte and a blank the pointer selects between.
  auto emit_optional_byte = [&](u8 byte, ir::RegisterIdx present) {
    const ir::TypeIdx buf_ty = builder.array_type(u8_ty, 2);
    const ir::RegisterIdx buf = emit(ir::Opcode::Alloca, buf_ty, {size_one});
    for (u32 cell = 0; cell < 2; ++cell) {
      const ir::RegisterIdx slot =
          emit(ir::Opcode::GetElementPtr, u8_ty,
               {to_operand(buf, buf_ty), zero_i32, index_operand(cell)});
      const ir::ImmutableIdx glyph = builder.immutable(
          {.type = u8_ty,
           .data = {.u8_value = static_cast<u8>(cell == 0 ? byte : ' ')}});
      emit_void(ir::Opcode::Store,
                {to_operand(glyph, u8_ty), to_operand(slot, u8_ty)});
    }
    const ir::RegisterIdx first =
        emit(ir::Opcode::GetElementPtr, u8_ty,
             {to_operand(buf, buf_ty), zero_i32, usize_imm(0)});
    const ir::RegisterIdx second =
        emit(ir::Opcode::GetElementPtr, u8_ty,
             {to_operand(buf, buf_ty), zero_i32, usize_imm(1)});
    const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
    const ir::RegisterIdx ptr =
        emit(ir::Opcode::Select, ptr_ty,
             {to_operand(present, boolean), to_operand(first, ptr_ty),
              to_operand(second, ptr_ty)});
    const ir::RegisterIdx len =
        emit(ir::Opcode::Select, usize_ty,
             {to_operand(present, boolean), usize_imm(1), usize_imm(0)});
    return bounded_copy(to_operand(ptr, ptr_ty), to_operand(len, usize_ty),
                        to_operand(len, usize_ty));
  };
  // The decimal digits of `wide`, most significant first, through a
  // scratch buffer the loop fills from its end backwards.
  auto emit_u64_digits = [&](ir::RegisterIdx wide) {
    const ir::TypeIdx u64_ty = builder.primitive(ir::TypeTag::U64);
    const ir::TypeIdx digits_ty =
        builder.array_type(u8_ty, static_cast<u64>(20));
    const ir::RegisterIdx tmp = emit(ir::Opcode::Alloca, digits_ty, {size_one});
    const ir::RegisterIdx n_addr = emit(ir::Opcode::Alloca, u64_ty, {size_one});
    emit_void(ir::Opcode::Store,
              {to_operand(wide, u64_ty), to_operand(n_addr, u64_ty)});
    const ir::RegisterIdx count_addr =
        emit(ir::Opcode::Alloca, usize_ty, {size_one});
    emit_void(ir::Opcode::Store,
              {usize_imm(0), to_operand(count_addr, usize_ty)});
    const ir::BlockIdx head = reserve_block();
    const ir::BlockIdx body = reserve_block();
    const ir::BlockIdx exit = reserve_block();
    emit_br(head);
    switch_to(head);
    emit_br(body);
    switch_to(body);
    const ir::RegisterIdx pending =
        emit(ir::Opcode::Load, u64_ty, {to_operand(n_addr, u64_ty)});
    const ir::RegisterIdx digit = emit(
        ir::Opcode::UintRem, u64_ty,
        {to_operand(pending, u64_ty),
         to_operand(
             builder.immutable({.type = u64_ty, .data = {.u64_value = 10}}),
             u64_ty)});
    const ir::RegisterIdx rest = emit(
        ir::Opcode::UintDiv, u64_ty,
        {to_operand(pending, u64_ty),
         to_operand(
             builder.immutable({.type = u64_ty, .data = {.u64_value = 10}}),
             u64_ty)});
    emit_void(ir::Opcode::Store,
              {to_operand(rest, u64_ty), to_operand(n_addr, u64_ty)});
    const ir::RegisterIdx narrow =
        emit(ir::Opcode::TypeCast, u8_ty, {to_operand(digit, u64_ty)});
    const ir::RegisterIdx glyph =
        emit(ir::Opcode::IntAdd, u8_ty,
             {to_operand(narrow, u8_ty),
              to_operand(
                  builder.immutable({.type = u8_ty, .data = {.u8_value = 48}}),
                  u8_ty)});
    const ir::RegisterIdx written =
        emit(ir::Opcode::Load, usize_ty, {to_operand(count_addr, usize_ty)});
    const ir::RegisterIdx slot =
        emit(ir::Opcode::IntSub, usize_ty,
             {usize_imm(19), to_operand(written, usize_ty)});
    const ir::RegisterIdx cell = emit(
        ir::Opcode::GetElementPtr, u8_ty,
        {to_operand(tmp, digits_ty), zero_i32, to_operand(slot, usize_ty)});
    emit_void(ir::Opcode::Store,
              {to_operand(glyph, u8_ty), to_operand(cell, u8_ty)});
    const ir::RegisterIdx grown =
        emit(ir::Opcode::IntAdd, usize_ty,
             {to_operand(written, usize_ty), usize_imm(1)});
    emit_void(ir::Opcode::Store,
              {to_operand(grown, usize_ty), to_operand(count_addr, usize_ty)});
    const ir::RegisterIdx left =
        emit(ir::Opcode::Load, u64_ty, {to_operand(n_addr, u64_ty)});
    const ir::RegisterIdx more =
        emit(ir::Opcode::Gt, boolean,
             {to_operand(left, u64_ty),
              to_operand(
                  builder.immutable({.type = u64_ty, .data = {.u64_value = 0}}),
                  u64_ty)});
    emit_cond_br(to_operand(more, boolean), body, exit);
    switch_to(exit);
    if (failed) {
      return false;
    }
    const ir::RegisterIdx digits =
        emit(ir::Opcode::Load, usize_ty, {to_operand(count_addr, usize_ty)});
    // The loop filled the scratch buffer from its end backwards, so
    // the digits start `20 - digits` cells in.
    const ir::RegisterIdx tail =
        emit(ir::Opcode::IntSub, usize_ty,
             {usize_imm(20), to_operand(digits, usize_ty)});
    const ir::RegisterIdx run = emit(
        ir::Opcode::GetElementPtr, u8_ty,
        {to_operand(tmp, digits_ty), zero_i32, to_operand(tail, usize_ty)});
    return bounded_copy(to_operand(run, builder.primitive(ir::TypeTag::Ptr)),
                        to_operand(digits, usize_ty),
                        to_operand(digits, usize_ty));
  };
  // A non-negative float below 1e16 formats as its truncated integer
  // part, which the u64 conversion gives exactly, followed by
  // `digits` fractional digits taken from the difference and rounded
  // half-up. Trailing zeros are trimmed when `trim` is set. `fold`
  // makes a fraction that carries all the way print one digit and
  // report it through `carry_out`, which is what a mantissa needs.
  auto emit_magnitude = [&](ir::RegisterIdx v, u32 digits, bool trim, bool fold,
                            ir::RegisterIdx& carry_out) {
    const ir::TypeIdx f64_ty = builder.primitive(ir::TypeTag::F64);
    const ir::TypeIdx u64_ty = builder.primitive(ir::TypeTag::U64);
    auto f64_imm = [&](f64 literal) {
      return to_operand(
          builder.immutable({.type = f64_ty, .data = {.f64_value = literal}}),
          f64_ty);
    };
    const ir::OperandIdx glyph_zero = to_operand(
        builder.immutable({.type = u8_ty, .data = {.u8_value = 48}}), u8_ty);
    const ir::OperandIdx ten = to_operand(
        builder.immutable({.type = u8_ty, .data = {.u8_value = 10}}), u8_ty);
    const ir::RegisterIdx whole =
        emit(ir::Opcode::TypeCast, u64_ty, {to_operand(v, f64_ty)});
    const ir::RegisterIdx whole_f =
        emit(ir::Opcode::TypeCast, f64_ty, {to_operand(whole, u64_ty)});
    const ir::RegisterIdx difference =
        emit(ir::Opcode::FSub, f64_ty,
             {to_operand(v, f64_ty), to_operand(whole_f, f64_ty)});
    // At and above 2^53 every value is an integer, and widening the
    // truncation rounds, so the difference is not the fraction there.
    const ir::RegisterIdx integral =
        emit(ir::Opcode::Ge, boolean,
             {to_operand(v, f64_ty), f64_imm(9007199254740992.0)});
    ir::RegisterIdx fraction =
        emit(ir::Opcode::Select, f64_ty,
             {to_operand(integral, boolean), f64_imm(0.0),
              to_operand(difference, f64_ty)});
    // One extra digit decides the rounding.
    const ir::TypeIdx frac_ty =
        builder.array_type(u8_ty, static_cast<u64>(digits) + 2);
    const ir::RegisterIdx frac_buf =
        emit(ir::Opcode::Alloca, frac_ty, {size_one});
    for (u32 i = 0; i <= digits; ++i) {
      const ir::RegisterIdx scaled =
          emit(ir::Opcode::FMul, f64_ty,
               {to_operand(fraction, f64_ty), f64_imm(10.0)});
      const ir::RegisterIdx over = emit(
          ir::Opcode::Gt, boolean, {to_operand(scaled, f64_ty), f64_imm(9.0)});
      const ir::RegisterIdx capped =
          emit(ir::Opcode::Select, f64_ty,
               {to_operand(over, boolean), f64_imm(9.0),
                to_operand(scaled, f64_ty)});
      const ir::RegisterIdx narrow =
          emit(ir::Opcode::TypeCast, u8_ty, {to_operand(capped, f64_ty)});
      const ir::RegisterIdx glyph = emit(
          ir::Opcode::IntAdd, u8_ty, {to_operand(narrow, u8_ty), glyph_zero});
      const ir::RegisterIdx cell =
          emit(ir::Opcode::GetElementPtr, u8_ty,
               {to_operand(frac_buf, frac_ty), zero_i32, index_operand(i)});
      emit_void(ir::Opcode::Store,
                {to_operand(glyph, u8_ty), to_operand(cell, u8_ty)});
      const ir::RegisterIdx digit =
          emit(ir::Opcode::TypeCast, f64_ty, {to_operand(narrow, u8_ty)});
      const ir::RegisterIdx rest =
          emit(ir::Opcode::FSub, f64_ty,
               {to_operand(scaled, f64_ty), to_operand(digit, f64_ty)});
      const ir::RegisterIdx below = emit(
          ir::Opcode::Lt, boolean, {to_operand(rest, f64_ty), f64_imm(0.0)});
      fraction = emit(
          ir::Opcode::Select, f64_ty,
          {to_operand(below, boolean), f64_imm(0.0), to_operand(rest, f64_ty)});
    }
    // Half-up: the extra digit starts the carry and it runs down the
    // fraction into the integer part.
    const ir::RegisterIdx last =
        emit(ir::Opcode::GetElementPtr, u8_ty,
             {to_operand(frac_buf, frac_ty), zero_i32, index_operand(digits)});
    const ir::RegisterIdx last_glyph =
        emit(ir::Opcode::Load, u8_ty, {to_operand(last, u8_ty)});
    const ir::OperandIdx half = to_operand(
        builder.immutable({.type = u8_ty, .data = {.u8_value = 53}}), u8_ty);
    ir::RegisterIdx carry =
        emit(ir::Opcode::Ge, boolean, {to_operand(last_glyph, u8_ty), half});
    for (u32 i = digits; i-- > 0;) {
      const ir::RegisterIdx cell =
          emit(ir::Opcode::GetElementPtr, u8_ty,
               {to_operand(frac_buf, frac_ty), zero_i32, index_operand(i)});
      const ir::RegisterIdx glyph =
          emit(ir::Opcode::Load, u8_ty, {to_operand(cell, u8_ty)});
      const ir::RegisterIdx value = emit(
          ir::Opcode::IntSub, u8_ty, {to_operand(glyph, u8_ty), glyph_zero});
      const ir::RegisterIdx one =
          emit(ir::Opcode::TypeCast, u8_ty, {to_operand(carry, boolean)});
      const ir::RegisterIdx summed =
          emit(ir::Opcode::IntAdd, u8_ty,
               {to_operand(value, u8_ty), to_operand(one, u8_ty)});
      const ir::RegisterIdx wrapped_ten =
          emit(ir::Opcode::Eq, boolean, {to_operand(summed, u8_ty), ten});
      const ir::RegisterIdx wrapped =
          emit(ir::Opcode::Select, u8_ty,
               {to_operand(wrapped_ten, boolean),
                to_operand(
                    builder.immutable({.type = u8_ty, .data = {.u8_value = 0}}),
                    u8_ty),
                to_operand(summed, u8_ty)});
      const ir::RegisterIdx wrapped_glyph = emit(
          ir::Opcode::IntAdd, u8_ty, {to_operand(wrapped, u8_ty), glyph_zero});
      emit_void(ir::Opcode::Store,
                {to_operand(wrapped_glyph, u8_ty), to_operand(cell, u8_ty)});
      carry = wrapped_ten;
    }
    const ir::RegisterIdx carry_wide =
        emit(ir::Opcode::TypeCast, u64_ty, {to_operand(carry, boolean)});
    const ir::RegisterIdx adjusted =
        emit(ir::Opcode::IntAdd, u64_ty,
             {to_operand(whole, u64_ty), to_operand(carry_wide, u64_ty)});
    ir::RegisterIdx printed = adjusted;
    carry_out = carry;
    if (fold) {
      const ir::OperandIdx one = to_operand(
          builder.immutable({.type = u64_ty, .data = {.u64_value = 1}}),
          u64_ty);
      const ir::RegisterIdx is_ten = emit(
          ir::Opcode::Eq, boolean,
          {to_operand(adjusted, u64_ty),
           to_operand(
               builder.immutable({.type = u64_ty, .data = {.u64_value = 10}}),
               u64_ty)});
      printed = emit(
          ir::Opcode::Select, u64_ty,
          {to_operand(is_ten, boolean), one, to_operand(adjusted, u64_ty)});
      carry_out = is_ten;
    }
    if (!emit_u64_digits(printed)) {
      return false;
    }
    // The fraction follows the point, trimmed of what rounds away.
    ir::OperandIdx length = usize_imm(digits);
    if (trim) {
      for (u32 i = digits; i-- > 0;) {
        const ir::RegisterIdx cell =
            emit(ir::Opcode::GetElementPtr, u8_ty,
                 {to_operand(frac_buf, frac_ty), zero_i32, index_operand(i)});
        const ir::RegisterIdx glyph =
            emit(ir::Opcode::Load, u8_ty, {to_operand(cell, u8_ty)});
        const ir::RegisterIdx is_zero = emit(
            ir::Opcode::Eq, boolean, {to_operand(glyph, u8_ty), glyph_zero});
        const ir::RegisterIdx at_edge =
            emit(ir::Opcode::Eq, boolean, {length, usize_imm(i + 1)});
        const ir::RegisterIdx shrink =
            emit(ir::Opcode::And, boolean,
                 {to_operand(at_edge, boolean), to_operand(is_zero, boolean)});
        length = to_operand(
            emit(ir::Opcode::Select, usize_ty,
                 {to_operand(shrink, boolean), usize_imm(i), length}),
            usize_ty);
      }
    }
    if (digits > 0) {
      const ir::RegisterIdx has_fraction =
          emit(ir::Opcode::Gt, boolean, {length, usize_imm(0)});
      if (!emit_optional_byte('.', has_fraction)) {
        return false;
      }
      const ir::RegisterIdx run =
          emit(ir::Opcode::GetElementPtr, u8_ty,
               {to_operand(frac_buf, frac_ty), zero_i32, usize_imm(0)});
      if (!bounded_copy(to_operand(run, builder.primitive(ir::TypeTag::Ptr)),
                        length, length)) {
        return false;
      }
    }
    return !failed;
  };
  // Floats format as decimal magnitudes. `{}` prints up to six
  // fractional digits, rounded half-up and trimmed of trailing zeros,
  // and gives way to a mantissa-and-exponent form outside
  // [1e-4, 1e16); `{:.Nf}` (and its `{:.N}` sugar) fixes the
  // fractional digits and keeps the same cutoffs. A magnitude that
  // does not fit the fixed form is why the exponent exists at all.
  auto emit_float = [&](ir::RegisterIdx value, u8 precision) {
    const ir::TypeIdx f64_ty = builder.primitive(ir::TypeTag::F64);
    const ir::TypeIdx u64_ty = builder.primitive(ir::TypeTag::U64);
    const ir::TypeIdx i32_ty = builder.primitive(ir::TypeTag::I32);
    const bool trim = precision == analyzer::FMT_NO_PRECISION;
    const u32 digits = trim ? 6u : static_cast<u32>(precision);
    auto f64_imm = [&](f64 literal) {
      return to_operand(
          builder.immutable({.type = f64_ty, .data = {.f64_value = literal}}),
          f64_ty);
    };
    // `NaN` equals nothing, not even itself; the comparison is
    // ordered, so only its negation reports the value.
    const ir::RegisterIdx self_equal =
        emit(ir::Opcode::Eq, boolean,
             {to_operand(value, f64_ty), to_operand(value, f64_ty)});
    const ir::RegisterIdx not_a_number =
        emit(ir::Opcode::Not, boolean, {to_operand(self_equal, boolean)});
    const ir::BlockIdx nan_block = reserve_block();
    const ir::BlockIdx finite_block = reserve_block();
    const ir::BlockIdx float_join = reserve_block();
    emit_cond_br(to_operand(not_a_number, boolean), nan_block, finite_block);
    switch_to(nan_block);
    {
      ir::OperandIdx ptr = ir::OperandIdx::invalid();
      ir::OperandIdx len = ir::OperandIdx::invalid();
      if (!const_str("NaN", ptr, len) || !bounded_copy(ptr, len, len)) {
        return false;
      }
      emit_br(float_join);
    }
    switch_to(finite_block);
    const ir::OperandIdx zero = f64_imm(0.0);
    const ir::RegisterIdx negative =
        emit(ir::Opcode::Lt, boolean, {to_operand(value, f64_ty), zero});
    const ir::RegisterIdx flipped =
        emit(ir::Opcode::FSub, f64_ty, {zero, to_operand(value, f64_ty)});
    const ir::RegisterIdx magnitude =
        emit(ir::Opcode::Select, f64_ty,
             {to_operand(negative, boolean), to_operand(flipped, f64_ty),
              to_operand(value, f64_ty)});
    if (!emit_optional_byte('-', negative)) {
      return false;
    }
    // Only an infinite magnitude exceeds the largest finite double;
    // the sign piece already sits in front of it.
    const ir::RegisterIdx infinite =
        emit(ir::Opcode::Gt, boolean,
             {to_operand(magnitude, f64_ty), f64_imm(1.7976931348623157e308)});
    const ir::BlockIdx inf_block = reserve_block();
    const ir::BlockIdx magnitude_block = reserve_block();
    emit_cond_br(to_operand(infinite, boolean), inf_block, magnitude_block);
    switch_to(inf_block);
    {
      ir::OperandIdx ptr = ir::OperandIdx::invalid();
      ir::OperandIdx len = ir::OperandIdx::invalid();
      if (!const_str("inf", ptr, len) || !bounded_copy(ptr, len, len)) {
        return false;
      }
      emit_br(float_join);
    }
    switch_to(magnitude_block);
    // The fixed form covers what the integer digits of the truncation
    // reach; past that, and below the point's first digit, the
    // mantissa and its exponent take over.
    const ir::RegisterIdx big =
        emit(ir::Opcode::Ge, boolean,
             {to_operand(magnitude, f64_ty), f64_imm(1e16)});
    const ir::RegisterIdx nonzero =
        emit(ir::Opcode::Gt, boolean, {to_operand(magnitude, f64_ty), zero});
    const ir::RegisterIdx below_point =
        emit(ir::Opcode::Lt, boolean,
             {to_operand(magnitude, f64_ty), f64_imm(1e-4)});
    const ir::RegisterIdx tiny =
        emit(ir::Opcode::And, boolean,
             {to_operand(nonzero, boolean), to_operand(below_point, boolean)});
    const ir::RegisterIdx scientific =
        emit(ir::Opcode::Or, boolean,
             {to_operand(big, boolean), to_operand(tiny, boolean)});
    const ir::BlockIdx sci_head = reserve_block();
    const ir::BlockIdx fixed_block = reserve_block();
    emit_cond_br(to_operand(scientific, boolean), sci_head, fixed_block);
    switch_to(sci_head);
    // Normalize to a mantissa in [1, 10); each step moves the
    // exponent the other way.
    const ir::RegisterIdx m_addr = emit(ir::Opcode::Alloca, f64_ty, {size_one});
    emit_void(ir::Opcode::Store,
              {to_operand(magnitude, f64_ty), to_operand(m_addr, f64_ty)});
    const ir::RegisterIdx e_addr = emit(ir::Opcode::Alloca, i32_ty, {size_one});
    const ir::OperandIdx zero_e = to_operand(
        builder.immutable({.type = i32_ty, .data = {.i64_value = 0}}), i32_ty);
    emit_void(ir::Opcode::Store, {zero_e, to_operand(e_addr, i32_ty)});
    const ir::BlockIdx norm_head = reserve_block();
    const ir::BlockIdx norm_body = reserve_block();
    const ir::BlockIdx norm_exit = reserve_block();
    emit_br(norm_head);
    switch_to(norm_head);
    {
      const ir::RegisterIdx m =
          emit(ir::Opcode::Load, f64_ty, {to_operand(m_addr, f64_ty)});
      const ir::RegisterIdx shrink =
          emit(ir::Opcode::Ge, boolean, {to_operand(m, f64_ty), f64_imm(10.0)});
      const ir::RegisterIdx grow =
          emit(ir::Opcode::Lt, boolean, {to_operand(m, f64_ty), f64_imm(1.0)});
      const ir::RegisterIdx again =
          emit(ir::Opcode::Select, boolean,
               {to_operand(big, boolean), to_operand(shrink, boolean),
                to_operand(grow, boolean)});
      emit_cond_br(to_operand(again, boolean), norm_body, norm_exit);
    }
    switch_to(norm_body);
    {
      const ir::RegisterIdx m =
          emit(ir::Opcode::Load, f64_ty, {to_operand(m_addr, f64_ty)});
      const ir::RegisterIdx factor =
          emit(ir::Opcode::Select, f64_ty,
               {to_operand(big, boolean), f64_imm(0.1), f64_imm(10.0)});
      const ir::RegisterIdx moved =
          emit(ir::Opcode::FMul, f64_ty,
               {to_operand(m, f64_ty), to_operand(factor, f64_ty)});
      emit_void(ir::Opcode::Store,
                {to_operand(moved, f64_ty), to_operand(m_addr, f64_ty)});
      const ir::RegisterIdx e =
          emit(ir::Opcode::Load, i32_ty, {to_operand(e_addr, i32_ty)});
      const ir::RegisterIdx step = emit(
          ir::Opcode::Select, i32_ty,
          {to_operand(big, boolean),
           to_operand(
               builder.immutable({.type = i32_ty, .data = {.i64_value = 1}}),
               i32_ty),
           to_operand(
               builder.immutable({.type = i32_ty, .data = {.i64_value = -1}}),
               i32_ty)});
      const ir::RegisterIdx stepped =
          emit(ir::Opcode::IntAdd, i32_ty,
               {to_operand(e, i32_ty), to_operand(step, i32_ty)});
      emit_void(ir::Opcode::Store,
                {to_operand(stepped, i32_ty), to_operand(e_addr, i32_ty)});
      emit_br(norm_head);
    }
    switch_to(norm_exit);
    {
      const ir::RegisterIdx m =
          emit(ir::Opcode::Load, f64_ty, {to_operand(m_addr, f64_ty)});
      const ir::RegisterIdx e =
          emit(ir::Opcode::Load, i32_ty, {to_operand(e_addr, i32_ty)});
      ir::RegisterIdx folded = ir::RegisterIdx(base::INVALID_IDX);
      if (!emit_magnitude(m, digits, trim, /*fold=*/true, folded)) {
        return false;
      }
      // A mantissa that rounded up to ten is one step of the exponent.
      const ir::RegisterIdx bumped = emit(
          ir::Opcode::IntAdd, i32_ty,
          {to_operand(e, i32_ty),
           to_operand(
               builder.immutable({.type = i32_ty, .data = {.i64_value = 1}}),
               i32_ty)});
      const ir::RegisterIdx exponent =
          emit(ir::Opcode::Select, i32_ty,
               {to_operand(folded, boolean), to_operand(bumped, i32_ty),
                to_operand(e, i32_ty)});
      ir::OperandIdx ptr = ir::OperandIdx::invalid();
      ir::OperandIdx len = ir::OperandIdx::invalid();
      if (!const_str("e", ptr, len) || !bounded_copy(ptr, len, len)) {
        return false;
      }
      const ir::RegisterIdx negative_exponent =
          emit(ir::Opcode::Lt, boolean, {to_operand(exponent, i32_ty), zero_e});
      if (!emit_optional_byte('-', negative_exponent)) {
        return false;
      }
      const ir::RegisterIdx negated = emit(
          ir::Opcode::IntSub, i32_ty, {zero_e, to_operand(exponent, i32_ty)});
      const ir::RegisterIdx magnitude_exponent =
          emit(ir::Opcode::Select, i32_ty,
               {to_operand(negative_exponent, boolean),
                to_operand(negated, i32_ty), to_operand(exponent, i32_ty)});
      const ir::RegisterIdx wide_exponent =
          emit(ir::Opcode::TypeCast, u64_ty,
               {to_operand(magnitude_exponent, i32_ty)});
      if (!emit_u64_digits(wide_exponent)) {
        return false;
      }
      emit_br(float_join);
    }
    switch_to(fixed_block);
    {
      ir::RegisterIdx folded = ir::RegisterIdx(base::INVALID_IDX);
      if (!emit_magnitude(magnitude, digits, trim, /*fold=*/false, folded)) {
        return false;
      }
      emit_br(float_join);
    }
    switch_to(float_join);
    return !failed;
  };
  // A nominal argument formats through `fmt`'s `Display`: the method
  // writes the prefix of its rendering that fits into the remaining
  // destination and returns the total length. The measure pass hands
  // it an empty slice, which is how a growable buffer learns the size;
  // one placeholder may run the method twice.
  auto emit_display_piece =
      [&](diag::Span piece_span, ir::RegisterIdx elem_addr, ir::TypeIdx elem_ty,
          const analyzer::CheckedModule::CallTarget* target) {
        const ir::TypeIdx view_ty =
            builder.reference_type(builder.slice_type(u8_ty), true);
        Val receiver{to_operand(elem_addr, elem_ty), elem_ty, true, false};
        if (measure_only) {
          // An empty view: nothing is written and the total answers.
          const ir::RegisterIdx scratch =
              emit(ir::Opcode::Alloca, u8_ty, {size_one});
          Val empty = build_slice_value(
              to_operand(scratch, builder.reference_type(u8_ty, true)),
              usize_imm(0), view_ty, piece_span);
          if (failed) {
            return false;
          }
          const Val call = emit_spec_call(target, piece_span, receiver, empty,
                                          /*has_argument=*/true);
          if (failed) {
            return false;
          }
          accumulate(call.op);
          return !failed;
        }
        const ir::RegisterIdx off = emit(
            ir::Opcode::Load, usize_ty, {to_operand(state.off_addr, usize_ty)});
        const ir::RegisterIdx remaining =
            emit(ir::Opcode::IntSub, usize_ty,
                 {state.capacity, to_operand(off, usize_ty)});
        const ir::OperandIdx dst =
            advance_ptr(dst_base, to_operand(off, usize_ty), piece_span);
        if (failed) {
          return false;
        }
        Val view = build_slice_value(dst, to_operand(remaining, usize_ty),
                                     view_ty, piece_span);
        if (failed) {
          return false;
        }
        const Val call = emit_spec_call(target, piece_span, receiver, view,
                                        /*has_argument=*/true);
        if (failed) {
          return false;
        }
        // The method writes at most what fits; the total is what a
        // destination of its own would have taken.
        const ir::RegisterIdx fits =
            emit(ir::Opcode::Lt, boolean,
                 {call.op, to_operand(remaining, usize_ty)});
        const ir::RegisterIdx chunk = emit(ir::Opcode::Select, usize_ty,
                                           {to_operand(fits, boolean), call.op,
                                            to_operand(remaining, usize_ty)});
        advance(to_operand(chunk, usize_ty));
        accumulate(call.op);
        return !failed;
      };
  for (const analyzer::FmtPiece& piece : pieces) {
    if (failed) {
      return false;
    }
    if (!piece.is_arg) {
      ir::OperandIdx ptr = ir::OperandIdx::invalid();
      ir::OperandIdx len = ir::OperandIdx::invalid();
      if (!const_str(piece.literal, ptr, len)) {
        return false;
      }
      if (!bounded_copy(ptr, len, len)) {
        return false;
      }
      continue;
    }
    const ir::TypeIdx elem_ty = elem_types[piece.arg];
    const ir::RegisterIdx elem_addr =
        emit(ir::Opcode::GetElementPtr, elem_ty,
             {tup_op, zero_i32, index_operand(piece.arg)});
    const ir::TypeTag tag = tag_of(elem_ty);
    if (tag == ir::TypeTag::Str) {
      const ir::RegisterIdx loaded =
          emit(ir::Opcode::Load, elem_ty, {to_operand(elem_addr, elem_ty)});
      const ir::RegisterIdx ptr =
          emit(ir::Opcode::ExtractValue, builder.primitive(ir::TypeTag::Ptr),
               {to_operand(loaded, elem_ty), index_operand(0)});
      const ir::RegisterIdx len =
          emit(ir::Opcode::ExtractValue, usize_ty,
               {to_operand(loaded, elem_ty), index_operand(1)});
      if (!bounded_copy(to_operand(ptr, builder.primitive(ir::TypeTag::Ptr)),
                        to_operand(len, usize_ty), to_operand(len, usize_ty))) {
        return false;
      }
      continue;
    }
    if (tag == ir::TypeTag::I1) {
      const ir::RegisterIdx loaded =
          emit(ir::Opcode::Load, elem_ty, {to_operand(elem_addr, elem_ty)});
      ir::OperandIdx true_ptr = ir::OperandIdx::invalid();
      ir::OperandIdx true_len = ir::OperandIdx::invalid();
      ir::OperandIdx false_ptr = ir::OperandIdx::invalid();
      ir::OperandIdx false_len = ir::OperandIdx::invalid();
      if (!const_str("true", true_ptr, true_len) ||
          !const_str("false", false_ptr, false_len)) {
        return false;
      }
      const ir::RegisterIdx ptr =
          emit(ir::Opcode::Select, builder.primitive(ir::TypeTag::Ptr),
               {to_operand(loaded, elem_ty), true_ptr, false_ptr});
      const ir::RegisterIdx len =
          emit(ir::Opcode::Select, usize_ty,
               {to_operand(loaded, elem_ty), true_len, false_len});
      if (!bounded_copy(to_operand(ptr, builder.primitive(ir::TypeTag::Ptr)),
                        to_operand(len, usize_ty), to_operand(len, usize_ty))) {
        return false;
      }
      continue;
    }
    if (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64) {
      const ir::RegisterIdx loaded =
          emit(ir::Opcode::Load, elem_ty, {to_operand(elem_addr, elem_ty)});
      const ir::TypeIdx f64_ty = builder.primitive(ir::TypeTag::F64);
      const ir::RegisterIdx value = tag == ir::TypeTag::F32
                                        ? emit(ir::Opcode::TypeCast, f64_ty,
                                               {to_operand(loaded, elem_ty)})
                                        : loaded;
      if (failed || !emit_float(value, piece.precision)) {
        return false;
      }
      continue;
    }
    if (tag == ir::TypeTag::Struct || tag == ir::TypeTag::Enum) {
      const analyzer::CheckedModule::CallTarget* target =
          piece.arg < arg_exprs.size() ? call_target(arg_exprs[piece.arg])
                                       : nullptr;
      if (target == nullptr) {
        unsupported(span, "argument is not formattable");
        return false;
      }
      if (failed || !emit_display_piece(span, elem_addr, elem_ty, target)) {
        return false;
      }
      continue;
    }
    // Integers format as decimal through a scratch buffer. A signed
    // value is split into sign and magnitude first: widening with a
    // sign-extending cast would print -1 as twenty digits.
    const ir::RegisterIdx loaded =
        emit(ir::Opcode::Load, elem_ty, {to_operand(elem_addr, elem_ty)});
    const ir::TypeIdx u64_ty = builder.primitive(ir::TypeTag::U64);
    ir::RegisterIdx wide = loaded;
    if (ir::is_signed_integer_type(tag_of(elem_ty))) {
      // The sign is a runtime property, so the digits come from a
      // select between the value and its magnitude. Negating in the
      // source type gives the two's complement; TypeCast picks zero- or
      // sign-extension from the *source* tag, so both arms reinterpret
      // through the same width as an unsigned type before widening.
      const ir::ImmutableIdx zero =
          builder.immutable({.type = elem_ty, .data = {.i64_value = 0}});
      const ir::RegisterIdx magnitude =
          emit(ir::Opcode::IntSub, elem_ty,
               {to_operand(zero, elem_ty), to_operand(loaded, elem_ty)});
      const ir::TypeIdx same_width =
          builder.primitive(ir::unsigned_integer_type(tag_of(elem_ty)));
      auto as_unsigned = [&](ir::RegisterIdx value) -> ir::RegisterIdx {
        const ir::RegisterIdx same = emit(ir::Opcode::TypeCast, same_width,
                                          {to_operand(value, elem_ty)});
        return emit(ir::Opcode::TypeCast, u64_ty,
                    {to_operand(same, same_width)});
      };
      const ir::RegisterIdx positive = as_unsigned(loaded);
      const ir::RegisterIdx abs_value = as_unsigned(magnitude);
      const ir::RegisterIdx is_negative =
          emit(ir::Opcode::Lt, boolean,
               {to_operand(loaded, elem_ty), to_operand(zero, elem_ty)});
      wide =
          emit(ir::Opcode::Select, u64_ty,
               {to_operand(is_negative, boolean), to_operand(abs_value, u64_ty),
                to_operand(positive, u64_ty)});
      if (!emit_optional_byte('-', is_negative)) {
        return false;
      }
    } else {
      wide = emit(ir::Opcode::TypeCast, u64_ty, {to_operand(loaded, elem_ty)});
    }
    if (!emit_u64_digits(wide)) {
      return false;
    }
  }
  return !failed;
}

bool Lowerer::lower_rest_tuple(std::span<const ast::ExprIdx> args,
                               usize first,
                               diag::Span span,
                               Val& out,
                               std::vector<ir::TypeIdx>& elem_types,
                               std::vector<ir::TypeIdx>& arg_types) {
  if (first >= args.size()) {
    out = Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
    return true;
  }
  // Tuple element slots are contiguous type-table entries whose shapes
  // match the argument types; a copy is one entry, the same convention
  // tuple literals use.
  std::vector<ir::TypeIdx> slots;
  ir::TypeSeq element_seq;
  for (usize i = first; i < args.size(); ++i) {
    const ir::TypeIdx element = expr_type(args[i]);
    if (tag_of(element) == ir::TypeTag::Error) {
      return false;
    }
    const ir::TypeIdx slot = builder.ref_type(element);
    slots.push_back(slot);
    arg_types.push_back(element);
    element_seq.push(slot);
  }
  const ir::TypeIdx tuple_type = builder.tuple_type(element_seq.finish());
  const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, tuple_type, {size_one});
  for (usize i = 0; i < slots.size() && !failed; ++i) {
    const ir::TypeIdx slot = slots[i];
    Val value = lower_expr(args[first + i], &slot);
    if (failed) {
      return false;
    }
    const ir::RegisterIdx gep = emit(ir::Opcode::GetElementPtr, slot,
                                     {to_operand(addr, tuple_type), zero_i32,
                                      index_operand(static_cast<u32>(i))});
    emit_void(ir::Opcode::Store, {use_value(value), to_operand(gep, slot)});
  }
  if (failed) {
    return false;
  }
  out = Val{to_operand(addr, tuple_type), tuple_type, true, false};
  elem_types = std::move(slots);
  (void)span;
  return true;
}

Val Lowerer::lower_fmt_write(ast::ExprIdx expr,
                             const analyzer::CheckedModule::FnSig& sig) {
  const ast::ExprNode& node = ast.exprs[expr];
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  if (args.size() < 2) {
    internal(node.span, "call arity");
    return Val{size_one, error_type(), false, false};
  }
  CompVal fmt_val;
  if (!comp_evaluate(module, args[0], fmt_val) ||
      fmt_val.value.tag != CompValue::Tag::Str) {
    if (!failed) {
      internal(node.span, "format string without value");
    }
    return Val{size_one, error_type(), false, false};
  }
  Val buf = lower_expr(args[1], nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  if (tag_of(buf.type) != ir::TypeTag::MutRef) {
    internal(node.span, "buffer without address");
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx buf_pointee =
      builder.ref_types()[builder.types()[buf.type].as_ref()].pointee;
  if (tag_of(buf_pointee) != ir::TypeTag::Array) {
    internal(node.span, "buffer without address");
    return Val{size_one, error_type(), false, false};
  }
  const u64 capacity =
      builder.array_types()[builder.types()[buf_pointee].as_array()].count;
  // The arguments after the buffer are the format arguments; they are
  // collected into the tuple the pieces read (ADR-0055).
  std::vector<ir::TypeIdx> elem_types;
  std::vector<ir::TypeIdx> arg_types;
  Val tup{size_one, error_type(), false, false};
  if (!lower_rest_tuple(args, 2, node.span, tup, elem_types, arg_types)) {
    return Val{size_one, error_type(), false, false};
  }
  const analyzer::FmtTemplate parsed =
      analyzer::parse_format_string(fmt_val.value.str_value);
  if (parsed.error != analyzer::FmtError::None) {
    unsupported(node.span, "invalid format string");
    return Val{size_one, error_type(), false, false};
  }
  if (parsed.placeholders != elem_types.size()) {
    unsupported(node.span, "placeholder count does not match arguments");
    return Val{size_one, error_type(), false, false};
  }
  if (!check_fmt_arguments(node.span, parsed, elem_types)) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx usize_ty = usize_type();
  const ir::OperandIdx capacity_op = to_operand(
      builder.immutable({.type = usize_ty, .data = {.u64_value = capacity}}),
      usize_ty);
  FmtState state;
  if (!emit_fmt_pieces(node.span, parsed.pieces, tup.op, elem_types,
                       args.subspan(2), buf.op, capacity_op, state, false)) {
    return Val{size_one, error_type(), false, false};
  }
  // The arguments moved into the tuple are the expansion's to retire:
  // nothing else owns them once the pieces ran, and the tuple is not a
  // source binding the drop pass could schedule.
  emit_collected_drop(tup.op, arg_types, node.span);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx written =
      emit(ir::Opcode::Load, usize_ty, {to_operand(state.off_addr, usize_ty)});
  const ir::RegisterIdx total =
      emit(ir::Opcode::Load, usize_ty, {to_operand(state.tot_addr, usize_ty)});
  const ir::RegisterIdx slot = emit(ir::Opcode::Alloca, sig.ret, {size_one});
  const ir::TypeIdx outcome_written = field_type_of(sig.ret, 0, node.span);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx outcome_total = field_type_of(sig.ret, 1, node.span);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx field0 =
      emit(ir::Opcode::GetElementPtr, outcome_written,
           {to_operand(slot, sig.ret), zero_i32, index_operand(0)});
  emit_void(ir::Opcode::Store, {to_operand(written, outcome_written),
                                to_operand(field0, outcome_written)});
  const ir::RegisterIdx field1 =
      emit(ir::Opcode::GetElementPtr, outcome_total,
           {to_operand(slot, sig.ret), zero_i32, index_operand(1)});
  emit_void(ir::Opcode::Store, {to_operand(total, outcome_total),
                                to_operand(field1, outcome_total)});
  return Val{to_operand(slot, sig.ret), sig.ret, true, false};
}

Val Lowerer::lower_fmt_format(ast::ExprIdx expr,
                              const analyzer::CheckedModule::FnSig& sig) {
  const ast::ExprNode& node = ast.exprs[expr];
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  if (args.size() < 1) {
    internal(node.span, "call arity");
    return Val{size_one, error_type(), false, false};
  }
  CompVal fmt_val;
  if (!comp_evaluate(module, args[0], fmt_val) ||
      fmt_val.value.tag != CompValue::Tag::Str) {
    if (!failed) {
      internal(node.span, "format string without value");
    }
    return Val{size_one, error_type(), false, false};
  }
  // The arguments after the format string are collected into the tuple
  // the pieces read (ADR-0055).
  std::vector<ir::TypeIdx> elem_types;
  std::vector<ir::TypeIdx> arg_types;
  Val tup{size_one, error_type(), false, false};
  if (!lower_rest_tuple(args, 1, node.span, tup, elem_types, arg_types)) {
    return Val{size_one, error_type(), false, false};
  }
  ir::OperandIdx tup_op = tup.op;
  const analyzer::FmtTemplate parsed =
      analyzer::parse_format_string(fmt_val.value.str_value);
  if (parsed.error != analyzer::FmtError::None) {
    unsupported(node.span, "invalid format string");
    return Val{size_one, error_type(), false, false};
  }
  if (parsed.placeholders != elem_types.size()) {
    unsupported(node.span, "placeholder count does not match arguments");
    return Val{size_one, error_type(), false, false};
  }
  if (!check_fmt_arguments(node.span, parsed, elem_types)) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx usize_ty = usize_type();
  const ir::TypeIdx u8_ty = builder.primitive(ir::TypeTag::U8);
  u32 buf_field = 0;
  u32 len_field = 1;
  u32 cap_field = 2;
  if (!struct_field_index(sig.ret, "buf", buf_field) ||
      !struct_field_index(sig.ret, "len", len_field) ||
      !struct_field_index(sig.ret, "cap", cap_field)) {
    internal(node.span, "string without fields");
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx buf_field_ty = field_type_of(sig.ret, buf_field, node.span);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  // Phase one measures: the pieces run with no destination, and the
  // total they accrue is the exact size the heap buffer reserves. The
  // tuple is lowered once up front, so its side effects happen once
  // and both passes read the same values.
  FmtState measured;
  const ir::OperandIdx no_capacity = to_operand(
      builder.immutable({.type = usize_ty, .data = {.u64_value = 0}}),
      usize_ty);
  if (!emit_fmt_pieces(node.span, parsed.pieces, tup_op, elem_types,
                       args.subspan(1), size_one, no_capacity, measured,
                       true)) {
    return Val{size_one, error_type(), false, false};
  }
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx total = emit(ir::Opcode::Load, usize_ty,
                                     {to_operand(measured.tot_addr, usize_ty)});
  const ir::RegisterIdx heap =
      emit_heap_alloc(u8_ty, buf_field_ty, to_operand(total, usize_ty));
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx slot = emit(ir::Opcode::Alloca, sig.ret, {size_one});
  const ir::RegisterIdx buf_addr =
      emit(ir::Opcode::GetElementPtr, buf_field_ty,
           {to_operand(slot, sig.ret), zero_i32, index_operand(buf_field)});
  emit_void(ir::Opcode::Store, {to_operand(heap, buf_field_ty),
                                to_operand(buf_addr, buf_field_ty)});
  const ir::TypeIdx len_ty = field_type_of(sig.ret, len_field, node.span);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx cap_ty = field_type_of(sig.ret, cap_field, node.span);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx cap_addr =
      emit(ir::Opcode::GetElementPtr, cap_ty,
           {to_operand(slot, sig.ret), zero_i32, index_operand(cap_field)});
  emit_void(ir::Opcode::Store,
            {to_operand(total, usize_ty), to_operand(cap_addr, cap_ty)});
  // Phase two expands into the reserved buffer. The reservation is the
  // measured total, so nothing truncates; the length stored below is
  // the bytes the second pass wrote.
  FmtState state;
  if (!emit_fmt_pieces(node.span, parsed.pieces, tup_op, elem_types,
                       args.subspan(1), to_operand(heap, buf_field_ty),
                       to_operand(total, usize_ty), state, false)) {
    return Val{size_one, error_type(), false, false};
  }
  // The arguments moved into the tuple are the expansion's to retire.
  emit_collected_drop(tup_op, arg_types, node.span);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx written =
      emit(ir::Opcode::Load, usize_ty, {to_operand(state.off_addr, usize_ty)});
  const ir::RegisterIdx len_addr =
      emit(ir::Opcode::GetElementPtr, len_ty,
           {to_operand(slot, sig.ret), zero_i32, index_operand(len_field)});
  emit_void(ir::Opcode::Store,
            {to_operand(written, len_ty), to_operand(len_addr, len_ty)});
  return Val{to_operand(slot, sig.ret), sig.ret, true, false};
}

Val Lowerer::lower_io_print(ast::ExprIdx expr,
                            const analyzer::CheckedModule::FnSig& sig) {
  const ast::ExprNode& node = ast.exprs[expr];
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  if (args.empty()) {
    internal(node.span, "call arity");
    return Val{size_one, error_type(), false, false};
  }
  CompVal fmt_val;
  if (!comp_evaluate(module, args[0], fmt_val) ||
      fmt_val.value.tag != CompValue::Tag::Str) {
    if (!failed) {
      internal(node.span, "format string without value");
    }
    return Val{size_one, error_type(), false, false};
  }
  // The arguments after the format string are collected into the tuple
  // the pieces read (ADR-0055).
  std::vector<ir::TypeIdx> elem_types;
  std::vector<ir::TypeIdx> arg_types;
  Val tup{size_one, error_type(), false, false};
  if (!lower_rest_tuple(args, 1, node.span, tup, elem_types, arg_types)) {
    return Val{size_one, error_type(), false, false};
  }
  const analyzer::FmtTemplate parsed =
      analyzer::parse_format_string(fmt_val.value.str_value);
  if (parsed.error != analyzer::FmtError::None) {
    unsupported(node.span, "invalid format string");
    return Val{size_one, error_type(), false, false};
  }
  if (parsed.placeholders != elem_types.size()) {
    unsupported(node.span, "placeholder count does not match arguments");
    return Val{size_one, error_type(), false, false};
  }
  if (!check_fmt_arguments(node.span, parsed, elem_types)) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx usize_ty = usize_type();
  const ir::TypeIdx u8_ty = builder.primitive(ir::TypeTag::U8);
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  const ir::TypeIdx buf_ty = builder.reference_type(u8_ty, true);
  // Phase one measures; the total is the exact reservation. The tuple
  // is lowered once up front, so its side effects happen once and both
  // passes read the same values.
  FmtState measured;
  const ir::OperandIdx no_capacity = to_operand(
      builder.immutable({.type = usize_ty, .data = {.u64_value = 0}}),
      usize_ty);
  if (!emit_fmt_pieces(node.span, parsed.pieces, tup.op, elem_types,
                       args.subspan(1), size_one, no_capacity, measured,
                       true)) {
    return Val{size_one, error_type(), false, false};
  }
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx total = emit(ir::Opcode::Load, usize_ty,
                                     {to_operand(measured.tot_addr, usize_ty)});
  const ir::RegisterIdx heap =
      emit_heap_alloc(u8_ty, buf_ty, to_operand(total, usize_ty));
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  // Phase two writes the whole measured total, so nothing truncates.
  FmtState state;
  if (!emit_fmt_pieces(node.span, parsed.pieces, tup.op, elem_types,
                       args.subspan(1), to_operand(heap, buf_ty),
                       to_operand(total, usize_ty), state, false)) {
    return Val{size_one, error_type(), false, false};
  }
  // The arguments moved into the tuple are the expansion's to retire.
  emit_collected_drop(tup.op, arg_types, node.span);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  // The hook writes stdout; println adds the newline in a second write.
  const bool newline = sig.name == "println";
  const ir::ExternalFunctionIdx write_ext = declare_external(
      newline ? "alcy_println" : "alcy_print",
      builder.primitive(ir::TypeTag::Void), {ptr_ty, usize_ty});
  emit_void(ir::Opcode::Call,
            {builder.operand(ir::Operand::from_external_function(
                 write_ext, builder.primitive(ir::TypeTag::Function))),
             to_operand(heap, buf_ty), to_operand(total, usize_ty)});
  // The buffer is the expansion's; hand it back with the size and
  // alignment it was reserved at.
  const ir::RegisterIdx align = emit_type_query(ir::Opcode::TypeAlignOf, u8_ty);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::ExternalFunctionIdx free_ext =
      declare_external("alcy_dealloc", builder.primitive(ir::TypeTag::Void),
                       {ptr_ty, usize_ty, usize_ty});
  emit_void(ir::Opcode::Call,
            {builder.operand(ir::Operand::from_external_function(
                 free_ext, builder.primitive(ir::TypeTag::Function))),
             to_operand(heap, buf_ty), to_operand(total, usize_ty),
             to_operand(align, usize_ty)});
  return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
}
}  // namespace lowering
