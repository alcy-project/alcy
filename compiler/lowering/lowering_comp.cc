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

bool Lowerer::emit_fmt_pieces(diag::Span span,
                              const std::vector<analyzer::FmtPiece>& pieces,
                              ir::OperandIdx tup_op,
                              const std::vector<ir::TypeIdx>& elem_types,
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
    // Integers format as decimal through a scratch buffer. A signed
    // value is split into sign and magnitude first: widening with a
    // sign-extending cast would print -1 as twenty digits.
    const ir::RegisterIdx loaded =
        emit(ir::Opcode::Load, elem_ty, {to_operand(elem_addr, elem_ty)});
    const ir::TypeIdx u64_ty = builder.primitive(ir::TypeTag::U64);
    const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
    bool negative = false;
    ir::RegisterIdx is_negative = ir::RegisterIdx(base::INVALID_IDX);
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
      is_negative =
          emit(ir::Opcode::Lt, boolean,
               {to_operand(loaded, elem_ty), to_operand(zero, elem_ty)});
      wide =
          emit(ir::Opcode::Select, u64_ty,
               {to_operand(is_negative, boolean), to_operand(abs_value, u64_ty),
                to_operand(positive, u64_ty)});
      negative = true;
    } else {
      wide = emit(ir::Opcode::TypeCast, u64_ty, {to_operand(loaded, elem_ty)});
    }
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
    if (negative) {
      // The sign is a separate one-byte piece in front of the digits.
      // A zero-length copy is what drops it for a positive value, so
      // the digits stay at the tail of the scratch buffer.
      const ir::TypeIdx sign_ty = builder.array_type(u8_ty, 2);
      const ir::RegisterIdx sign_buf =
          emit(ir::Opcode::Alloca, sign_ty, {size_one});
      for (u32 cell = 0; cell < 2; ++cell) {
        const ir::RegisterIdx slot = emit(
            ir::Opcode::GetElementPtr, u8_ty,
            {to_operand(sign_buf, sign_ty), zero_i32, index_operand(cell)});
        const ir::ImmutableIdx glyph = builder.immutable(
            {.type = u8_ty,
             .data = {.u8_value = static_cast<u8>(cell == 0 ? '-' : ' ')}});
        emit_void(ir::Opcode::Store,
                  {to_operand(glyph, u8_ty), to_operand(slot, u8_ty)});
      }
      const ir::RegisterIdx minus_slot =
          emit(ir::Opcode::GetElementPtr, u8_ty,
               {to_operand(sign_buf, sign_ty), zero_i32, usize_imm(0)});
      const ir::RegisterIdx blank_slot =
          emit(ir::Opcode::GetElementPtr, u8_ty,
               {to_operand(sign_buf, sign_ty), zero_i32, usize_imm(1)});
      const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
      const ir::RegisterIdx sign_ptr = emit(
          ir::Opcode::Select, ptr_ty,
          {to_operand(is_negative, boolean), to_operand(minus_slot, ptr_ty),
           to_operand(blank_slot, ptr_ty)});
      const ir::RegisterIdx sign_len =
          emit(ir::Opcode::Select, usize_ty,
               {to_operand(is_negative, boolean), usize_imm(1), usize_imm(0)});
      if (!bounded_copy(to_operand(sign_ptr, ptr_ty),
                        to_operand(sign_len, usize_ty),
                        to_operand(sign_len, usize_ty))) {
        return false;
      }
    }
    // The loop filled the scratch buffer from its end backwards, so
    // the digits start `20 - digits` cells in.
    const ir::RegisterIdx tail =
        emit(ir::Opcode::IntSub, usize_ty,
             {usize_imm(20), to_operand(digits, usize_ty)});
    const ir::RegisterIdx run = emit(
        ir::Opcode::GetElementPtr, u8_ty,
        {to_operand(tmp, digits_ty), zero_i32, to_operand(tail, usize_ty)});
    if (!bounded_copy(to_operand(run, builder.primitive(ir::TypeTag::Ptr)),
                      to_operand(digits, usize_ty),
                      to_operand(digits, usize_ty))) {
      return false;
    }
  }
  return !failed;
}

Val Lowerer::lower_fmt_write(ast::ExprIdx expr,
                             const analyzer::CheckedModule::FnSig& sig) {
  const ast::ExprNode& node = ast.exprs[expr];
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  if (args.size() != 3) {
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
  Val tup = lower_expr(args[2], nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx tup_type = expr_type(args[2]);
  if (tag_of(tup_type) != ir::TypeTag::Tuple &&
      tag_of(tup_type) != ir::TypeTag::Void) {
    internal(node.span, "arguments without tuple");
    return Val{size_one, error_type(), false, false};
  }
  if (!tup.address && tag_of(tup_type) == ir::TypeTag::Tuple) {
    tup = address_of(tup);
  }
  std::vector<ir::TypeIdx> elem_types;
  if (tag_of(tup_type) == ir::TypeTag::Tuple) {
    const ir::TupleType& shape =
        builder.state().tuple_types[builder.state().types[tup_type].as_tuple()];
    elem_types.reserve(shape.elements.size());
    for (ir::TypeIdx element : shape.elements) {
      elem_types.push_back(element);
    }
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
  for (ir::TypeIdx element : elem_types) {
    if (!analyzer::is_formattable_tag(tag_of(element))) {
      unsupported(node.span, "argument is not formattable");
      return Val{size_one, error_type(), false, false};
    }
  }
  const ir::TypeIdx usize_ty = usize_type();
  const ir::OperandIdx capacity_op = to_operand(
      builder.immutable({.type = usize_ty, .data = {.u64_value = capacity}}),
      usize_ty);
  FmtState state;
  if (!emit_fmt_pieces(node.span, parsed.pieces, tup.op, elem_types, buf.op,
                       capacity_op, state, false)) {
    return Val{size_one, error_type(), false, false};
  }
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
  if (args.size() != 2) {
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
  Val tup = lower_expr(args[1], nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx tup_type = expr_type(args[1]);
  ir::OperandIdx tup_op = size_one;
  std::vector<ir::TypeIdx> elem_types;
  if (tag_of(tup_type) == ir::TypeTag::Tuple) {
    if (!tup.address) {
      tup = address_of(tup);
    }
    tup_op = tup.op;
    const ir::TupleType& shape =
        builder.state().tuple_types[builder.state().types[tup_type].as_tuple()];
    for (ir::TypeIdx element : shape.elements) {
      elem_types.push_back(element);
    }
  } else if (tag_of(tup_type) != ir::TypeTag::Void) {
    internal(node.span, "arguments without tuple");
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
  for (ir::TypeIdx element : elem_types) {
    if (!analyzer::is_formattable_tag(tag_of(element))) {
      unsupported(node.span, "argument is not formattable");
      return Val{size_one, error_type(), false, false};
    }
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
  if (!emit_fmt_pieces(node.span, parsed.pieces, tup_op, elem_types, size_one,
                       no_capacity, measured, true)) {
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
                       to_operand(heap, buf_field_ty),
                       to_operand(total, usize_ty), state, false)) {
    return Val{size_one, error_type(), false, false};
  }
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
}  // namespace lowering
