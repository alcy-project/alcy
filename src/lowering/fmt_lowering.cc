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

bool Lowerer::Fmt::check_arguments(diag::Span span,
                                   const analyzer::FmtTemplate& parsed,
                                   const std::vector<ir::TypeIdx>& elem_types) {
  for (const analyzer::FmtPiece& piece : parsed.pieces) {
    if (!piece.is_arg) {
      continue;
    }
    const ir::TypeIdx element = elem_types[piece.arg];
    const ir::TypeTag tag = owner_.tag_of(element);
    if (!analyzer::is_formattable_tag(tag)) {
      // A nominal formats through `fmt`'s `Display`; the recorded call
      // is what the expansion reads, and a missing one is reported
      // where the piece is emitted.
      if (tag != ir::TypeTag::Struct && tag != ir::TypeTag::Enum) {
        owner_.unsupported(span, "argument is not formattable");
        return false;
      }
    }
    if (!analyzer::fmt_piece_matches_argument(piece, tag)) {
      owner_.unsupported(span, "format precision applies to floats only");
      return false;
    }
  }
  return true;
}

bool Lowerer::Fmt::emit_pieces(diag::Span span,
                               const std::vector<analyzer::FmtPiece>& pieces,
                               ir::OperandIdx tup_op,
                               const std::vector<ir::TypeIdx>& elem_types,
                               std::span<const ast::ExprIdx> arg_exprs,
                               ir::OperandIdx dst_base,
                               ir::OperandIdx capacity,
                               State& state,
                               bool measure_only) {
  const ir::TypeIdx usize_ty = owner_.usize_type();
  const ir::TypeIdx u8_ty = owner_.builder.primitive(ir::TypeTag::U8);
  const ir::TypeIdx boolean = owner_.builder.primitive(ir::TypeTag::I1);
  auto usize_imm = [&](u64 value) {
    return owner_.to_operand(
        owner_.builder.immutable(
            {.type = usize_ty, .data = {.u64_value = value}}),
        usize_ty);
  };
  state.capacity = capacity;
  state.off_addr = owner_.emit(ir::Opcode::Alloca, usize_ty, {owner_.size_one});
  owner_.emit_void(ir::Opcode::Store,
                   {usize_imm(0), owner_.to_operand(state.off_addr, usize_ty)});
  state.tot_addr = owner_.emit(ir::Opcode::Alloca, usize_ty, {owner_.size_one});
  owner_.emit_void(ir::Opcode::Store,
                   {usize_imm(0), owner_.to_operand(state.tot_addr, usize_ty)});
  auto advance = [&](ir::OperandIdx delta) {
    const ir::RegisterIdx off =
        owner_.emit(ir::Opcode::Load, usize_ty,
                    {owner_.to_operand(state.off_addr, usize_ty)});
    const ir::RegisterIdx grown =
        owner_.emit(ir::Opcode::IntAdd, usize_ty,
                    {owner_.to_operand(off, usize_ty), delta});
    owner_.emit_void(ir::Opcode::Store,
                     {owner_.to_operand(grown, usize_ty),
                      owner_.to_operand(state.off_addr, usize_ty)});
  };
  auto accumulate = [&](ir::OperandIdx full) {
    const ir::RegisterIdx total =
        owner_.emit(ir::Opcode::Load, usize_ty,
                    {owner_.to_operand(state.tot_addr, usize_ty)});
    const ir::RegisterIdx grown =
        owner_.emit(ir::Opcode::IntAdd, usize_ty,
                    {owner_.to_operand(total, usize_ty), full});
    owner_.emit_void(ir::Opcode::Store,
                     {owner_.to_operand(grown, usize_ty),
                      owner_.to_operand(state.tot_addr, usize_ty)});
  };
  // Copies [src, len), bounded by the buffer: only chunk reaches
  // the buffer, while full always accrues to the total. Measuring
  // skips the copy and keeps the total, so a first pass learns the
  // exact size a growable buffer has to reserve.
  auto bounded_copy = [&](ir::OperandIdx src, ir::OperandIdx len,
                          ir::OperandIdx full) {
    if (measure_only) {
      accumulate(full);
      return !owner_.failed;
    }
    const ir::RegisterIdx off =
        owner_.emit(ir::Opcode::Load, usize_ty,
                    {owner_.to_operand(state.off_addr, usize_ty)});
    const ir::RegisterIdx remaining =
        owner_.emit(ir::Opcode::IntSub, usize_ty,
                    {state.capacity, owner_.to_operand(off, usize_ty)});
    const ir::RegisterIdx fits = owner_.emit(
        ir::Opcode::Lt, boolean, {len, owner_.to_operand(remaining, usize_ty)});
    const ir::RegisterIdx chunk =
        owner_.emit(ir::Opcode::Select, usize_ty,
                    {owner_.to_operand(fits, boolean), len,
                     owner_.to_operand(remaining, usize_ty)});
    const ir::OperandIdx dst =
        owner_.advance_ptr(dst_base, owner_.to_operand(off, usize_ty), span);
    if (owner_.failed) {
      return false;
    }
    owner_.emit_void(ir::Opcode::Memcopy,
                     {dst, src, owner_.to_operand(chunk, usize_ty)});
    advance(owner_.to_operand(chunk, usize_ty));
    accumulate(full);
    return !owner_.failed;
  };
  auto const_str = [&](const std::string& bytes, ir::OperandIdx& ptr_out,
                       ir::OperandIdx& len_out) {
    const str::StringPoolId id = owner_.intern_copied(bytes);
    if (id == str::INVALID_STRING_POOL_ID) {
      return false;
    }
    const ir::ImmutableIdx imm = owner_.builder.immutable(
        {.type = owner_.builder.primitive(ir::TypeTag::Str),
         .data = {.str_id_value = id}});
    const ir::OperandIdx str_op =
        owner_.to_operand(imm, owner_.builder.primitive(ir::TypeTag::Str));
    const ir::RegisterIdx ptr = owner_.emit(
        ir::Opcode::ExtractValue, owner_.builder.primitive(ir::TypeTag::Ptr),
        {str_op, owner_.index_operand(0)});
    if (owner_.failed) {
      return false;
    }
    const ir::RegisterIdx len = owner_.emit(ir::Opcode::ExtractValue, usize_ty,
                                            {str_op, owner_.index_operand(1)});
    if (owner_.failed) {
      return false;
    }
    ptr_out =
        owner_.to_operand(ptr, owner_.builder.primitive(ir::TypeTag::Ptr));
    len_out = owner_.to_operand(len, usize_ty);
    return true;
  };
  // A one-byte piece: the byte when `present`, nothing otherwise. A
  // zero-length copy is what drops it, so the two-byte buffer holds
  // the byte and a blank the pointer selects between.
  auto emit_optional_byte = [&](u8 byte, ir::RegisterIdx present) {
    const ir::TypeIdx buf_ty = owner_.builder.array_type(u8_ty, 2);
    const ir::RegisterIdx buf =
        owner_.emit(ir::Opcode::Alloca, buf_ty, {owner_.size_one});
    for (u32 cell = 0; cell < 2; ++cell) {
      const ir::RegisterIdx slot =
          owner_.emit(ir::Opcode::GetElementPtr, u8_ty,
                      {owner_.to_operand(buf, buf_ty), owner_.zero_i32,
                       owner_.index_operand(cell)});
      const ir::ImmutableIdx glyph = owner_.builder.immutable(
          {.type = u8_ty,
           .data = {.u8_value = static_cast<u8>(cell == 0 ? byte : ' ')}});
      owner_.emit_void(ir::Opcode::Store, {owner_.to_operand(glyph, u8_ty),
                                           owner_.to_operand(slot, u8_ty)});
    }
    const ir::RegisterIdx first = owner_.emit(
        ir::Opcode::GetElementPtr, u8_ty,
        {owner_.to_operand(buf, buf_ty), owner_.zero_i32, usize_imm(0)});
    const ir::RegisterIdx second = owner_.emit(
        ir::Opcode::GetElementPtr, u8_ty,
        {owner_.to_operand(buf, buf_ty), owner_.zero_i32, usize_imm(1)});
    const ir::TypeIdx ptr_ty = owner_.builder.primitive(ir::TypeTag::Ptr);
    const ir::RegisterIdx ptr = owner_.emit(
        ir::Opcode::Select, ptr_ty,
        {owner_.to_operand(present, boolean), owner_.to_operand(first, ptr_ty),
         owner_.to_operand(second, ptr_ty)});
    const ir::RegisterIdx len = owner_.emit(
        ir::Opcode::Select, usize_ty,
        {owner_.to_operand(present, boolean), usize_imm(1), usize_imm(0)});
    return bounded_copy(owner_.to_operand(ptr, ptr_ty),
                        owner_.to_operand(len, usize_ty),
                        owner_.to_operand(len, usize_ty));
  };
  // The decimal digits of `wide`, most significant first, through a
  // scratch buffer the loop fills from its end backwards.
  auto emit_u64_digits = [&](ir::RegisterIdx wide) {
    const ir::TypeIdx u64_ty = owner_.builder.primitive(ir::TypeTag::U64);
    const ir::TypeIdx digits_ty =
        owner_.builder.array_type(u8_ty, static_cast<u64>(20));
    const ir::RegisterIdx tmp =
        owner_.emit(ir::Opcode::Alloca, digits_ty, {owner_.size_one});
    const ir::RegisterIdx n_addr =
        owner_.emit(ir::Opcode::Alloca, u64_ty, {owner_.size_one});
    owner_.emit_void(ir::Opcode::Store, {owner_.to_operand(wide, u64_ty),
                                         owner_.to_operand(n_addr, u64_ty)});
    const ir::RegisterIdx count_addr =
        owner_.emit(ir::Opcode::Alloca, usize_ty, {owner_.size_one});
    owner_.emit_void(ir::Opcode::Store,
                     {usize_imm(0), owner_.to_operand(count_addr, usize_ty)});
    const ir::BlockIdx head = owner_.reserve_block();
    const ir::BlockIdx body = owner_.reserve_block();
    const ir::BlockIdx exit = owner_.reserve_block();
    owner_.emit_br(head);
    owner_.switch_to(head);
    owner_.emit_br(body);
    owner_.switch_to(body);
    const ir::RegisterIdx pending = owner_.emit(
        ir::Opcode::Load, u64_ty, {owner_.to_operand(n_addr, u64_ty)});
    const ir::RegisterIdx digit = owner_.emit(
        ir::Opcode::UintRem, u64_ty,
        {owner_.to_operand(pending, u64_ty),
         owner_.to_operand(owner_.builder.immutable(
                               {.type = u64_ty, .data = {.u64_value = 10}}),
                           u64_ty)});
    const ir::RegisterIdx rest = owner_.emit(
        ir::Opcode::UintDiv, u64_ty,
        {owner_.to_operand(pending, u64_ty),
         owner_.to_operand(owner_.builder.immutable(
                               {.type = u64_ty, .data = {.u64_value = 10}}),
                           u64_ty)});
    owner_.emit_void(ir::Opcode::Store, {owner_.to_operand(rest, u64_ty),
                                         owner_.to_operand(n_addr, u64_ty)});
    const ir::RegisterIdx narrow = owner_.emit(
        ir::Opcode::TypeCast, u8_ty, {owner_.to_operand(digit, u64_ty)});
    const ir::RegisterIdx glyph = owner_.emit(
        ir::Opcode::IntAdd, u8_ty,
        {owner_.to_operand(narrow, u8_ty),
         owner_.to_operand(owner_.builder.immutable(
                               {.type = u8_ty, .data = {.u8_value = 48}}),
                           u8_ty)});
    const ir::RegisterIdx written = owner_.emit(
        ir::Opcode::Load, usize_ty, {owner_.to_operand(count_addr, usize_ty)});
    const ir::RegisterIdx slot =
        owner_.emit(ir::Opcode::IntSub, usize_ty,
                    {usize_imm(19), owner_.to_operand(written, usize_ty)});
    const ir::RegisterIdx cell =
        owner_.emit(ir::Opcode::GetElementPtr, u8_ty,
                    {owner_.to_operand(tmp, digits_ty), owner_.zero_i32,
                     owner_.to_operand(slot, usize_ty)});
    owner_.emit_void(ir::Opcode::Store, {owner_.to_operand(glyph, u8_ty),
                                         owner_.to_operand(cell, u8_ty)});
    const ir::RegisterIdx grown =
        owner_.emit(ir::Opcode::IntAdd, usize_ty,
                    {owner_.to_operand(written, usize_ty), usize_imm(1)});
    owner_.emit_void(ir::Opcode::Store,
                     {owner_.to_operand(grown, usize_ty),
                      owner_.to_operand(count_addr, usize_ty)});
    const ir::RegisterIdx left = owner_.emit(
        ir::Opcode::Load, u64_ty, {owner_.to_operand(n_addr, u64_ty)});
    const ir::RegisterIdx more = owner_.emit(
        ir::Opcode::Gt, boolean,
        {owner_.to_operand(left, u64_ty),
         owner_.to_operand(owner_.builder.immutable(
                               {.type = u64_ty, .data = {.u64_value = 0}}),
                           u64_ty)});
    owner_.emit_cond_br(owner_.to_operand(more, boolean), body, exit);
    owner_.switch_to(exit);
    if (owner_.failed) {
      return false;
    }
    const ir::RegisterIdx digits = owner_.emit(
        ir::Opcode::Load, usize_ty, {owner_.to_operand(count_addr, usize_ty)});
    // The loop filled the scratch buffer from its end backwards, so
    // the digits start `20 - digits` cells in.
    const ir::RegisterIdx tail =
        owner_.emit(ir::Opcode::IntSub, usize_ty,
                    {usize_imm(20), owner_.to_operand(digits, usize_ty)});
    const ir::RegisterIdx run =
        owner_.emit(ir::Opcode::GetElementPtr, u8_ty,
                    {owner_.to_operand(tmp, digits_ty), owner_.zero_i32,
                     owner_.to_operand(tail, usize_ty)});
    return bounded_copy(
        owner_.to_operand(run, owner_.builder.primitive(ir::TypeTag::Ptr)),
        owner_.to_operand(digits, usize_ty),
        owner_.to_operand(digits, usize_ty));
  };
  // A non-negative float below 1e16 formats as its truncated integer
  // part, which the u64 conversion gives exactly, followed by
  // `digits` fractional digits taken from the difference and rounded
  // half-up. Trailing zeros are trimmed when `trim` is set. `fold`
  // makes a fraction that carries all the way print one digit and
  // report it through `carry_out`, which is what a mantissa needs.
  auto emit_magnitude = [&](ir::RegisterIdx v, u32 digits, bool trim, bool fold,
                            ir::RegisterIdx& carry_out) {
    const ir::TypeIdx f64_ty = owner_.builder.primitive(ir::TypeTag::F64);
    const ir::TypeIdx u64_ty = owner_.builder.primitive(ir::TypeTag::U64);
    auto f64_imm = [&](f64 literal) {
      return owner_.to_operand(
          owner_.builder.immutable(
              {.type = f64_ty, .data = {.f64_value = literal}}),
          f64_ty);
    };
    const ir::OperandIdx glyph_zero = owner_.to_operand(
        owner_.builder.immutable({.type = u8_ty, .data = {.u8_value = 48}}),
        u8_ty);
    const ir::OperandIdx ten = owner_.to_operand(
        owner_.builder.immutable({.type = u8_ty, .data = {.u8_value = 10}}),
        u8_ty);
    const ir::RegisterIdx whole = owner_.emit(ir::Opcode::TypeCast, u64_ty,
                                              {owner_.to_operand(v, f64_ty)});
    const ir::RegisterIdx whole_f = owner_.emit(
        ir::Opcode::TypeCast, f64_ty, {owner_.to_operand(whole, u64_ty)});
    const ir::RegisterIdx difference = owner_.emit(
        ir::Opcode::FSub, f64_ty,
        {owner_.to_operand(v, f64_ty), owner_.to_operand(whole_f, f64_ty)});
    // At and above 2^53 every value is an integer, and widening the
    // truncation rounds, so the difference is not the fraction there.
    const ir::RegisterIdx integral = owner_.emit(
        ir::Opcode::Ge, boolean,
        {owner_.to_operand(v, f64_ty), f64_imm(9007199254740992.0)});
    ir::RegisterIdx fraction =
        owner_.emit(ir::Opcode::Select, f64_ty,
                    {owner_.to_operand(integral, boolean), f64_imm(0.0),
                     owner_.to_operand(difference, f64_ty)});
    // The fraction is the dyadic rational `M / 2^K`, so its digits and
    // the half-up bit come from one exact product and a shift. Taking a
    // digit at a time by multiplying the `f64` fraction by ten instead
    // carried a rounding error that left the seventeenth place wrong:
    // `{:.17f}` of `0.1` printed `0.10000000000000000` where the value
    // rounds to `0.10000000000000001`.
    const ir::TypeIdx frac_ty =
        owner_.builder.array_type(u8_ty, static_cast<u64>(digits) + 1);
    const ir::RegisterIdx frac_buf =
        owner_.emit(ir::Opcode::Alloca, frac_ty, {owner_.size_one});
    auto u64_imm = [&](u64 literal) {
      return owner_.to_operand(
          owner_.builder.immutable(
              {.type = u64_ty, .data = {.u64_value = literal}}),
          u64_ty);
    };
    // Doubling is exact, and a finite fraction below one reaches an
    // integer `M` below 2^53 after `K` steps; zero is integral at once.
    const ir::RegisterIdx dyadic_addr =
        owner_.emit(ir::Opcode::Alloca, f64_ty, {owner_.size_one});
    owner_.emit_void(ir::Opcode::Store,
                     {owner_.to_operand(fraction, f64_ty),
                      owner_.to_operand(dyadic_addr, f64_ty)});
    const ir::RegisterIdx power_addr =
        owner_.emit(ir::Opcode::Alloca, u64_ty, {owner_.size_one});
    owner_.emit_void(ir::Opcode::Store,
                     {u64_imm(0), owner_.to_operand(power_addr, u64_ty)});
    const ir::BlockIdx dy_head = owner_.reserve_block();
    const ir::BlockIdx dy_body = owner_.reserve_block();
    const ir::BlockIdx dy_exit = owner_.reserve_block();
    owner_.emit_br(dy_head);
    owner_.switch_to(dy_head);
    {
      const ir::RegisterIdx at = owner_.emit(
          ir::Opcode::Load, f64_ty, {owner_.to_operand(dyadic_addr, f64_ty)});
      const ir::RegisterIdx truncated = owner_.emit(
          ir::Opcode::TypeCast, u64_ty, {owner_.to_operand(at, f64_ty)});
      const ir::RegisterIdx widened = owner_.emit(
          ir::Opcode::TypeCast, f64_ty, {owner_.to_operand(truncated, u64_ty)});
      const ir::RegisterIdx integral = owner_.emit(
          ir::Opcode::Eq, boolean,
          {owner_.to_operand(widened, f64_ty), owner_.to_operand(at, f64_ty)});
      owner_.emit_cond_br(owner_.to_operand(integral, boolean), dy_exit,
                          dy_body);
    }
    owner_.switch_to(dy_body);
    {
      const ir::RegisterIdx at = owner_.emit(
          ir::Opcode::Load, f64_ty, {owner_.to_operand(dyadic_addr, f64_ty)});
      const ir::RegisterIdx doubled =
          owner_.emit(ir::Opcode::FMul, f64_ty,
                      {owner_.to_operand(at, f64_ty), f64_imm(2.0)});
      owner_.emit_void(ir::Opcode::Store,
                       {owner_.to_operand(doubled, f64_ty),
                        owner_.to_operand(dyadic_addr, f64_ty)});
      const ir::RegisterIdx power = owner_.emit(
          ir::Opcode::Load, u64_ty, {owner_.to_operand(power_addr, u64_ty)});
      const ir::RegisterIdx next =
          owner_.emit(ir::Opcode::IntAdd, u64_ty,
                      {owner_.to_operand(power, u64_ty), u64_imm(1)});
      owner_.emit_void(ir::Opcode::Store,
                       {owner_.to_operand(next, u64_ty),
                        owner_.to_operand(power_addr, u64_ty)});
      owner_.emit_br(dy_head);
    }
    owner_.switch_to(dy_exit);
    const ir::RegisterIdx scaled_at = owner_.emit(
        ir::Opcode::Load, f64_ty, {owner_.to_operand(dyadic_addr, f64_ty)});
    const ir::RegisterIdx numerator = owner_.emit(
        ir::Opcode::TypeCast, u64_ty, {owner_.to_operand(scaled_at, f64_ty)});
    const ir::RegisterIdx dyadic = owner_.emit(
        ir::Opcode::Load, u64_ty, {owner_.to_operand(power_addr, u64_ty)});
    // `M * 10^digits` in two limbs: `M` is below 2^53 and the constant
    // below 2^57, so every partial product fits one word.
    u64 constant = 1;
    for (u32 i = 0; i < digits; ++i) {
      constant *= 10;
    }
    const ir::RegisterIdx m_low = owner_.emit(
        ir::Opcode::And, u64_ty,
        {owner_.to_operand(numerator, u64_ty), u64_imm(0xFFFFFFFFu)});
    const ir::RegisterIdx m_high =
        owner_.emit(ir::Opcode::LogicalShiftRight, u64_ty,
                    {owner_.to_operand(numerator, u64_ty), u64_imm(32)});
    const ir::RegisterIdx p0 = owner_.emit(
        ir::Opcode::IntMul, u64_ty,
        {owner_.to_operand(m_low, u64_ty), u64_imm(constant & 0xFFFFFFFFu)});
    const ir::RegisterIdx p1 = owner_.emit(
        ir::Opcode::IntMul, u64_ty,
        {owner_.to_operand(m_low, u64_ty), u64_imm(constant >> 32)});
    const ir::RegisterIdx p2 = owner_.emit(
        ir::Opcode::IntMul, u64_ty,
        {owner_.to_operand(m_high, u64_ty), u64_imm(constant & 0xFFFFFFFFu)});
    const ir::RegisterIdx p3 = owner_.emit(
        ir::Opcode::IntMul, u64_ty,
        {owner_.to_operand(m_high, u64_ty), u64_imm(constant >> 32)});
    const ir::RegisterIdx p0_low =
        owner_.emit(ir::Opcode::And, u64_ty,
                    {owner_.to_operand(p0, u64_ty), u64_imm(0xFFFFFFFFu)});
    const ir::RegisterIdx p0_high =
        owner_.emit(ir::Opcode::LogicalShiftRight, u64_ty,
                    {owner_.to_operand(p0, u64_ty), u64_imm(32)});
    const ir::RegisterIdx p1_low =
        owner_.emit(ir::Opcode::And, u64_ty,
                    {owner_.to_operand(p1, u64_ty), u64_imm(0xFFFFFFFFu)});
    const ir::RegisterIdx p1_high =
        owner_.emit(ir::Opcode::LogicalShiftRight, u64_ty,
                    {owner_.to_operand(p1, u64_ty), u64_imm(32)});
    const ir::RegisterIdx p2_low =
        owner_.emit(ir::Opcode::And, u64_ty,
                    {owner_.to_operand(p2, u64_ty), u64_imm(0xFFFFFFFFu)});
    const ir::RegisterIdx p2_high =
        owner_.emit(ir::Opcode::LogicalShiftRight, u64_ty,
                    {owner_.to_operand(p2, u64_ty), u64_imm(32)});
    const ir::RegisterIdx middle0 =
        owner_.emit(ir::Opcode::IntAdd, u64_ty,
                    {owner_.to_operand(p0_high, u64_ty),
                     owner_.to_operand(p1_low, u64_ty)});
    const ir::RegisterIdx middle =
        owner_.emit(ir::Opcode::IntAdd, u64_ty,
                    {owner_.to_operand(middle0, u64_ty),
                     owner_.to_operand(p2_low, u64_ty)});
    const ir::RegisterIdx middle_shifted =
        owner_.emit(ir::Opcode::ShiftLeft, u64_ty,
                    {owner_.to_operand(middle, u64_ty), u64_imm(32)});
    const ir::RegisterIdx product_low =
        owner_.emit(ir::Opcode::Or, u64_ty,
                    {owner_.to_operand(p0_low, u64_ty),
                     owner_.to_operand(middle_shifted, u64_ty)});
    const ir::RegisterIdx middle_high =
        owner_.emit(ir::Opcode::LogicalShiftRight, u64_ty,
                    {owner_.to_operand(middle, u64_ty), u64_imm(32)});
    const ir::RegisterIdx high0 = owner_.emit(
        ir::Opcode::IntAdd, u64_ty,
        {owner_.to_operand(p3, u64_ty), owner_.to_operand(p1_high, u64_ty)});
    const ir::RegisterIdx high1 = owner_.emit(
        ir::Opcode::IntAdd, u64_ty,
        {owner_.to_operand(high0, u64_ty), owner_.to_operand(p2_high, u64_ty)});
    const ir::RegisterIdx product_high =
        owner_.emit(ir::Opcode::IntAdd, u64_ty,
                    {owner_.to_operand(high1, u64_ty),
                     owner_.to_operand(middle_high, u64_ty)});
    // `q = (M * 10^digits) >> (K - 1)`: its low bit is the half-up
    // decision and the rest is the digit string. The wrapped `K` of the
    // integral case falls past every window and answers zero.
    const ir::RegisterIdx split =
        owner_.emit(ir::Opcode::IntSubWrap, u64_ty,
                    {owner_.to_operand(dyadic, u64_ty), u64_imm(1)});
    const ir::RegisterIdx low_window =
        owner_.emit(ir::Opcode::Lt, boolean,
                    {owner_.to_operand(split, u64_ty), u64_imm(64)});
    const ir::RegisterIdx high_window =
        owner_.emit(ir::Opcode::Ge, boolean,
                    {owner_.to_operand(split, u64_ty), u64_imm(64)});
    const ir::RegisterIdx past_windows =
        owner_.emit(ir::Opcode::Ge, boolean,
                    {owner_.to_operand(split, u64_ty), u64_imm(128)});
    const ir::RegisterIdx low_count =
        owner_.emit(ir::Opcode::Select, u64_ty,
                    {owner_.to_operand(low_window, boolean),
                     owner_.to_operand(split, u64_ty), u64_imm(0)});
    const ir::RegisterIdx cross_raw =
        owner_.emit(ir::Opcode::IntSubWrap, u64_ty,
                    {u64_imm(63), owner_.to_operand(split, u64_ty)});
    const ir::RegisterIdx cross_count =
        owner_.emit(ir::Opcode::Select, u64_ty,
                    {owner_.to_operand(low_window, boolean),
                     owner_.to_operand(cross_raw, u64_ty), u64_imm(0)});
    const ir::RegisterIdx above_raw =
        owner_.emit(ir::Opcode::IntSubWrap, u64_ty,
                    {owner_.to_operand(split, u64_ty), u64_imm(64)});
    const ir::RegisterIdx above_count =
        owner_.emit(ir::Opcode::Select, u64_ty,
                    {owner_.to_operand(high_window, boolean),
                     owner_.to_operand(above_raw, u64_ty), u64_imm(0)});
    const ir::RegisterIdx high_count =
        owner_.emit(ir::Opcode::Select, u64_ty,
                    {owner_.to_operand(past_windows, boolean), u64_imm(0),
                     owner_.to_operand(above_count, u64_ty)});
    // A shift by the width is undefined, so the crossing term shifts by
    // `63 - split` and then by one, which drops the same top bits.
    const ir::RegisterIdx crossed =
        owner_.emit(ir::Opcode::ShiftLeft, u64_ty,
                    {owner_.to_operand(product_high, u64_ty),
                     owner_.to_operand(cross_count, u64_ty)});
    const ir::RegisterIdx crossed_again =
        owner_.emit(ir::Opcode::ShiftLeft, u64_ty,
                    {owner_.to_operand(crossed, u64_ty), u64_imm(1)});
    const ir::RegisterIdx low_digits =
        owner_.emit(ir::Opcode::LogicalShiftRight, u64_ty,
                    {owner_.to_operand(product_low, u64_ty),
                     owner_.to_operand(low_count, u64_ty)});
    const ir::RegisterIdx below_path =
        owner_.emit(ir::Opcode::Or, u64_ty,
                    {owner_.to_operand(crossed_again, u64_ty),
                     owner_.to_operand(low_digits, u64_ty)});
    const ir::RegisterIdx above_path =
        owner_.emit(ir::Opcode::LogicalShiftRight, u64_ty,
                    {owner_.to_operand(product_high, u64_ty),
                     owner_.to_operand(high_count, u64_ty)});
    const ir::RegisterIdx within = owner_.emit(
        ir::Opcode::Not, boolean, {owner_.to_operand(past_windows, boolean)});
    const ir::RegisterIdx middle_path =
        owner_.emit(ir::Opcode::And, boolean,
                    {owner_.to_operand(high_window, boolean),
                     owner_.to_operand(within, boolean)});
    const ir::RegisterIdx above_selected =
        owner_.emit(ir::Opcode::Select, u64_ty,
                    {owner_.to_operand(middle_path, boolean),
                     owner_.to_operand(above_path, u64_ty), u64_imm(0)});
    const ir::RegisterIdx quotient =
        owner_.emit(ir::Opcode::Select, u64_ty,
                    {owner_.to_operand(low_window, boolean),
                     owner_.to_operand(below_path, u64_ty),
                     owner_.to_operand(above_selected, u64_ty)});
    const ir::RegisterIdx exact =
        owner_.emit(ir::Opcode::LogicalShiftRight, u64_ty,
                    {owner_.to_operand(quotient, u64_ty), u64_imm(1)});
    const ir::RegisterIdx round_bit =
        owner_.emit(ir::Opcode::And, u64_ty,
                    {owner_.to_operand(quotient, u64_ty), u64_imm(1)});
    // The digit string is the exact value, least significant last.
    ir::RegisterIdx pending = exact;
    for (u32 i = digits; i-- > 0;) {
      const ir::RegisterIdx rest =
          owner_.emit(ir::Opcode::UintRem, u64_ty,
                      {owner_.to_operand(pending, u64_ty), u64_imm(10)});
      const ir::RegisterIdx kept =
          owner_.emit(ir::Opcode::UintDiv, u64_ty,
                      {owner_.to_operand(pending, u64_ty), u64_imm(10)});
      const ir::RegisterIdx narrow = owner_.emit(
          ir::Opcode::TypeCast, u8_ty, {owner_.to_operand(rest, u64_ty)});
      const ir::RegisterIdx glyph =
          owner_.emit(ir::Opcode::IntAdd, u8_ty,
                      {owner_.to_operand(narrow, u8_ty), glyph_zero});
      const ir::RegisterIdx cell =
          owner_.emit(ir::Opcode::GetElementPtr, u8_ty,
                      {owner_.to_operand(frac_buf, frac_ty), owner_.zero_i32,
                       owner_.index_operand(i)});
      owner_.emit_void(ir::Opcode::Store, {owner_.to_operand(glyph, u8_ty),
                                           owner_.to_operand(cell, u8_ty)});
      pending = kept;
    }
    // Half-up: the leftover above the digits carries down through the
    // fraction into the integer part.
    ir::RegisterIdx carry =
        owner_.emit(ir::Opcode::Ne, boolean,
                    {owner_.to_operand(round_bit, u64_ty), u64_imm(0)});
    for (u32 i = digits; i-- > 0;) {
      const ir::RegisterIdx cell =
          owner_.emit(ir::Opcode::GetElementPtr, u8_ty,
                      {owner_.to_operand(frac_buf, frac_ty), owner_.zero_i32,
                       owner_.index_operand(i)});
      const ir::RegisterIdx glyph = owner_.emit(
          ir::Opcode::Load, u8_ty, {owner_.to_operand(cell, u8_ty)});
      const ir::RegisterIdx value =
          owner_.emit(ir::Opcode::IntSub, u8_ty,
                      {owner_.to_operand(glyph, u8_ty), glyph_zero});
      const ir::RegisterIdx one = owner_.emit(
          ir::Opcode::TypeCast, u8_ty, {owner_.to_operand(carry, boolean)});
      const ir::RegisterIdx summed = owner_.emit(
          ir::Opcode::IntAdd, u8_ty,
          {owner_.to_operand(value, u8_ty), owner_.to_operand(one, u8_ty)});
      const ir::RegisterIdx wrapped_ten = owner_.emit(
          ir::Opcode::Eq, boolean, {owner_.to_operand(summed, u8_ty), ten});
      const ir::RegisterIdx wrapped = owner_.emit(
          ir::Opcode::Select, u8_ty,
          {owner_.to_operand(wrapped_ten, boolean),
           owner_.to_operand(owner_.builder.immutable(
                                 {.type = u8_ty, .data = {.u8_value = 0}}),
                             u8_ty),
           owner_.to_operand(summed, u8_ty)});
      const ir::RegisterIdx wrapped_glyph =
          owner_.emit(ir::Opcode::IntAdd, u8_ty,
                      {owner_.to_operand(wrapped, u8_ty), glyph_zero});
      owner_.emit_void(ir::Opcode::Store,
                       {owner_.to_operand(wrapped_glyph, u8_ty),
                        owner_.to_operand(cell, u8_ty)});
      carry = wrapped_ten;
    }
    const ir::RegisterIdx carry_wide = owner_.emit(
        ir::Opcode::TypeCast, u64_ty, {owner_.to_operand(carry, boolean)});
    const ir::RegisterIdx adjusted =
        owner_.emit(ir::Opcode::IntAdd, u64_ty,
                    {owner_.to_operand(whole, u64_ty),
                     owner_.to_operand(carry_wide, u64_ty)});
    ir::RegisterIdx printed = adjusted;
    carry_out = carry;
    if (fold) {
      const ir::OperandIdx one = owner_.to_operand(
          owner_.builder.immutable({.type = u64_ty, .data = {.u64_value = 1}}),
          u64_ty);
      const ir::RegisterIdx is_ten = owner_.emit(
          ir::Opcode::Eq, boolean,
          {owner_.to_operand(adjusted, u64_ty),
           owner_.to_operand(owner_.builder.immutable(
                                 {.type = u64_ty, .data = {.u64_value = 10}}),
                             u64_ty)});
      printed = owner_.emit(ir::Opcode::Select, u64_ty,
                            {owner_.to_operand(is_ten, boolean), one,
                             owner_.to_operand(adjusted, u64_ty)});
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
            owner_.emit(ir::Opcode::GetElementPtr, u8_ty,
                        {owner_.to_operand(frac_buf, frac_ty), owner_.zero_i32,
                         owner_.index_operand(i)});
        const ir::RegisterIdx glyph = owner_.emit(
            ir::Opcode::Load, u8_ty, {owner_.to_operand(cell, u8_ty)});
        const ir::RegisterIdx is_zero =
            owner_.emit(ir::Opcode::Eq, boolean,
                        {owner_.to_operand(glyph, u8_ty), glyph_zero});
        const ir::RegisterIdx at_edge =
            owner_.emit(ir::Opcode::Eq, boolean, {length, usize_imm(i + 1)});
        const ir::RegisterIdx shrink =
            owner_.emit(ir::Opcode::And, boolean,
                        {owner_.to_operand(at_edge, boolean),
                         owner_.to_operand(is_zero, boolean)});
        length = owner_.to_operand(
            owner_.emit(
                ir::Opcode::Select, usize_ty,
                {owner_.to_operand(shrink, boolean), usize_imm(i), length}),
            usize_ty);
      }
    }
    if (digits > 0) {
      const ir::RegisterIdx has_fraction =
          owner_.emit(ir::Opcode::Gt, boolean, {length, usize_imm(0)});
      if (!emit_optional_byte('.', has_fraction)) {
        return false;
      }
      const ir::RegisterIdx run =
          owner_.emit(ir::Opcode::GetElementPtr, u8_ty,
                      {owner_.to_operand(frac_buf, frac_ty), owner_.zero_i32,
                       usize_imm(0)});
      if (!bounded_copy(owner_.to_operand(
                            run, owner_.builder.primitive(ir::TypeTag::Ptr)),
                        length, length)) {
        return false;
      }
    }
    return !owner_.failed;
  };
  // Floats format as decimal magnitudes. `{}` prints up to six
  // fractional digits, rounded half-up and trimmed of trailing zeros,
  // and gives way to a mantissa-and-exponent form outside
  // [1e-4, 1e16); `{:.Nf}` (and its `{:.N}` sugar) fixes the
  // fractional digits and keeps the same cutoffs. A magnitude that
  // does not fit the fixed form is why the exponent exists at all.
  auto emit_float = [&](ir::RegisterIdx value, u8 precision) {
    const ir::TypeIdx f64_ty = owner_.builder.primitive(ir::TypeTag::F64);
    const ir::TypeIdx u64_ty = owner_.builder.primitive(ir::TypeTag::U64);
    const ir::TypeIdx i32_ty = owner_.builder.primitive(ir::TypeTag::I32);
    const bool trim = precision == analyzer::FMT_NO_PRECISION;
    const u32 digits = trim ? 6u : static_cast<u32>(precision);
    auto f64_imm = [&](f64 literal) {
      return owner_.to_operand(
          owner_.builder.immutable(
              {.type = f64_ty, .data = {.f64_value = literal}}),
          f64_ty);
    };
    // `NaN` equals nothing, not even itself; the comparison is
    // ordered, so only its negation reports the value.
    const ir::RegisterIdx self_equal = owner_.emit(
        ir::Opcode::Eq, boolean,
        {owner_.to_operand(value, f64_ty), owner_.to_operand(value, f64_ty)});
    const ir::RegisterIdx not_a_number = owner_.emit(
        ir::Opcode::Not, boolean, {owner_.to_operand(self_equal, boolean)});
    const ir::BlockIdx nan_block = owner_.reserve_block();
    const ir::BlockIdx finite_block = owner_.reserve_block();
    const ir::BlockIdx float_join = owner_.reserve_block();
    owner_.emit_cond_br(owner_.to_operand(not_a_number, boolean), nan_block,
                        finite_block);
    owner_.switch_to(nan_block);
    {
      ir::OperandIdx ptr = ir::OperandIdx::invalid();
      ir::OperandIdx len = ir::OperandIdx::invalid();
      if (!const_str("NaN", ptr, len) || !bounded_copy(ptr, len, len)) {
        return false;
      }
      owner_.emit_br(float_join);
    }
    owner_.switch_to(finite_block);
    const ir::OperandIdx zero = f64_imm(0.0);
    const ir::RegisterIdx negative = owner_.emit(
        ir::Opcode::Lt, boolean, {owner_.to_operand(value, f64_ty), zero});
    const ir::RegisterIdx flipped = owner_.emit(
        ir::Opcode::FSub, f64_ty, {zero, owner_.to_operand(value, f64_ty)});
    const ir::RegisterIdx magnitude = owner_.emit(
        ir::Opcode::Select, f64_ty,
        {owner_.to_operand(negative, boolean),
         owner_.to_operand(flipped, f64_ty), owner_.to_operand(value, f64_ty)});
    if (!emit_optional_byte('-', negative)) {
      return false;
    }
    // Only an infinite magnitude exceeds the largest finite double;
    // the sign piece already sits in front of it.
    const ir::RegisterIdx infinite =
        owner_.emit(ir::Opcode::Gt, boolean,
                    {owner_.to_operand(magnitude, f64_ty),
                     f64_imm(1.7976931348623157e308)});
    const ir::BlockIdx inf_block = owner_.reserve_block();
    const ir::BlockIdx magnitude_block = owner_.reserve_block();
    owner_.emit_cond_br(owner_.to_operand(infinite, boolean), inf_block,
                        magnitude_block);
    owner_.switch_to(inf_block);
    {
      ir::OperandIdx ptr = ir::OperandIdx::invalid();
      ir::OperandIdx len = ir::OperandIdx::invalid();
      if (!const_str("inf", ptr, len) || !bounded_copy(ptr, len, len)) {
        return false;
      }
      owner_.emit_br(float_join);
    }
    owner_.switch_to(magnitude_block);
    // The fixed form covers what the integer digits of the truncation
    // reach; past that, and below the point's first digit, the
    // mantissa and its exponent take over.
    const ir::RegisterIdx big =
        owner_.emit(ir::Opcode::Ge, boolean,
                    {owner_.to_operand(magnitude, f64_ty), f64_imm(1e16)});
    const ir::RegisterIdx nonzero = owner_.emit(
        ir::Opcode::Gt, boolean, {owner_.to_operand(magnitude, f64_ty), zero});
    const ir::RegisterIdx below_point =
        owner_.emit(ir::Opcode::Lt, boolean,
                    {owner_.to_operand(magnitude, f64_ty), f64_imm(1e-4)});
    const ir::RegisterIdx tiny =
        owner_.emit(ir::Opcode::And, boolean,
                    {owner_.to_operand(nonzero, boolean),
                     owner_.to_operand(below_point, boolean)});
    const ir::RegisterIdx scientific = owner_.emit(
        ir::Opcode::Or, boolean,
        {owner_.to_operand(big, boolean), owner_.to_operand(tiny, boolean)});
    const ir::BlockIdx sci_head = owner_.reserve_block();
    const ir::BlockIdx fixed_block = owner_.reserve_block();
    owner_.emit_cond_br(owner_.to_operand(scientific, boolean), sci_head,
                        fixed_block);
    owner_.switch_to(sci_head);
    // Normalize to a mantissa in [1, 10); each step moves the
    // exponent the other way.
    const ir::RegisterIdx m_addr =
        owner_.emit(ir::Opcode::Alloca, f64_ty, {owner_.size_one});
    owner_.emit_void(ir::Opcode::Store, {owner_.to_operand(magnitude, f64_ty),
                                         owner_.to_operand(m_addr, f64_ty)});
    const ir::RegisterIdx e_addr =
        owner_.emit(ir::Opcode::Alloca, i32_ty, {owner_.size_one});
    const ir::OperandIdx zero_e = owner_.to_operand(
        owner_.builder.immutable({.type = i32_ty, .data = {.i64_value = 0}}),
        i32_ty);
    owner_.emit_void(ir::Opcode::Store,
                     {zero_e, owner_.to_operand(e_addr, i32_ty)});
    const ir::BlockIdx norm_head = owner_.reserve_block();
    const ir::BlockIdx norm_body = owner_.reserve_block();
    const ir::BlockIdx norm_exit = owner_.reserve_block();
    owner_.emit_br(norm_head);
    owner_.switch_to(norm_head);
    {
      const ir::RegisterIdx m = owner_.emit(
          ir::Opcode::Load, f64_ty, {owner_.to_operand(m_addr, f64_ty)});
      const ir::RegisterIdx shrink =
          owner_.emit(ir::Opcode::Ge, boolean,
                      {owner_.to_operand(m, f64_ty), f64_imm(10.0)});
      const ir::RegisterIdx grow =
          owner_.emit(ir::Opcode::Lt, boolean,
                      {owner_.to_operand(m, f64_ty), f64_imm(1.0)});
      const ir::RegisterIdx again = owner_.emit(
          ir::Opcode::Select, boolean,
          {owner_.to_operand(big, boolean), owner_.to_operand(shrink, boolean),
           owner_.to_operand(grow, boolean)});
      owner_.emit_cond_br(owner_.to_operand(again, boolean), norm_body,
                          norm_exit);
    }
    owner_.switch_to(norm_body);
    {
      const ir::RegisterIdx m = owner_.emit(
          ir::Opcode::Load, f64_ty, {owner_.to_operand(m_addr, f64_ty)});
      const ir::RegisterIdx factor = owner_.emit(
          ir::Opcode::Select, f64_ty,
          {owner_.to_operand(big, boolean), f64_imm(0.1), f64_imm(10.0)});
      const ir::RegisterIdx moved = owner_.emit(
          ir::Opcode::FMul, f64_ty,
          {owner_.to_operand(m, f64_ty), owner_.to_operand(factor, f64_ty)});
      owner_.emit_void(ir::Opcode::Store, {owner_.to_operand(moved, f64_ty),
                                           owner_.to_operand(m_addr, f64_ty)});
      const ir::RegisterIdx e = owner_.emit(
          ir::Opcode::Load, i32_ty, {owner_.to_operand(e_addr, i32_ty)});
      const ir::RegisterIdx step = owner_.emit(
          ir::Opcode::Select, i32_ty,
          {owner_.to_operand(big, boolean),
           owner_.to_operand(owner_.builder.immutable(
                                 {.type = i32_ty, .data = {.i64_value = 1}}),
                             i32_ty),
           owner_.to_operand(owner_.builder.immutable(
                                 {.type = i32_ty, .data = {.i64_value = -1}}),
                             i32_ty)});
      const ir::RegisterIdx stepped = owner_.emit(
          ir::Opcode::IntAdd, i32_ty,
          {owner_.to_operand(e, i32_ty), owner_.to_operand(step, i32_ty)});
      owner_.emit_void(ir::Opcode::Store, {owner_.to_operand(stepped, i32_ty),
                                           owner_.to_operand(e_addr, i32_ty)});
      owner_.emit_br(norm_head);
    }
    owner_.switch_to(norm_exit);
    {
      const ir::RegisterIdx e = owner_.emit(
          ir::Opcode::Load, i32_ty, {owner_.to_operand(e_addr, i32_ty)});
      auto i32_imm = [&](i64 value) {
        return owner_.to_operand(
            owner_.builder.immutable(
                {.type = i32_ty, .data = {.i64_value = value}}),
            i32_ty);
      };
      // Rebuild the mantissa from the original magnitude and a power of
      // ten built from one table entry (a multiple of sixteen) and an
      // exact product of tens. The normalization loop's repeated tenths
      // accumulate a rounding per decade, which leaves the exponent
      // form's last digits wrong; two scaled products keep the value's
      // own digits.
      const ir::RegisterIdx below_zero = owner_.emit(
          ir::Opcode::Lt, boolean, {owner_.to_operand(e, i32_ty), zero_e});
      const ir::RegisterIdx negated_places = owner_.emit(
          ir::Opcode::IntSub, i32_ty, {zero_e, owner_.to_operand(e, i32_ty)});
      const ir::RegisterIdx places =
          owner_.emit(ir::Opcode::Select, i32_ty,
                      {owner_.to_operand(below_zero, boolean),
                       owner_.to_operand(negated_places, i32_ty),
                       owner_.to_operand(e, i32_ty)});
      // One power of ten, held as a table entry (a multiple of sixteen)
      // and an exact product of tens, since ten to the fifteenth is the
      // last that fits a double exactly.
      auto pow10 = [](u32 exponent) -> f64 {
        std::string text = "1";
        text.append(exponent, '0');
        return std::strtod(text.c_str(), nullptr);
      };
      const ir::RegisterIdx chunk =
          owner_.emit(ir::Opcode::UintDiv, i32_ty,
                      {owner_.to_operand(places, i32_ty), i32_imm(16)});
      const ir::RegisterIdx chunk_scaled =
          owner_.emit(ir::Opcode::IntMul, i32_ty,
                      {owner_.to_operand(chunk, i32_ty), i32_imm(16)});
      const ir::RegisterIdx rest =
          owner_.emit(ir::Opcode::IntSub, i32_ty,
                      {owner_.to_operand(places, i32_ty),
                       owner_.to_operand(chunk_scaled, i32_ty)});
      ir::OperandIdx high = f64_imm(pow10(0));
      for (u32 i = 1; i <= 20; ++i) {
        const ir::RegisterIdx hit =
            owner_.emit(ir::Opcode::Eq, boolean,
                        {owner_.to_operand(chunk, i32_ty), i32_imm(i)});
        const ir::RegisterIdx chosen = owner_.emit(
            ir::Opcode::Select, f64_ty,
            {owner_.to_operand(hit, boolean), f64_imm(pow10(16 * i)), high});
        high = owner_.to_operand(chosen, f64_ty);
      }
      const ir::RegisterIdx low_addr =
          owner_.emit(ir::Opcode::Alloca, f64_ty, {owner_.size_one});
      owner_.emit_void(ir::Opcode::Store,
                       {f64_imm(1.0), owner_.to_operand(low_addr, f64_ty)});
      const ir::RegisterIdx count_addr =
          owner_.emit(ir::Opcode::Alloca, i32_ty, {owner_.size_one});
      owner_.emit_void(ir::Opcode::Store,
                       {zero_e, owner_.to_operand(count_addr, i32_ty)});
      const ir::BlockIdx low_head = owner_.reserve_block();
      const ir::BlockIdx low_body = owner_.reserve_block();
      const ir::BlockIdx low_exit = owner_.reserve_block();
      owner_.emit_br(low_head);
      owner_.switch_to(low_head);
      {
        const ir::RegisterIdx at = owner_.emit(
            ir::Opcode::Load, i32_ty, {owner_.to_operand(count_addr, i32_ty)});
        const ir::RegisterIdx more = owner_.emit(
            ir::Opcode::Lt, boolean,
            {owner_.to_operand(at, i32_ty), owner_.to_operand(rest, i32_ty)});
        owner_.emit_cond_br(owner_.to_operand(more, boolean), low_body,
                            low_exit);
      }
      owner_.switch_to(low_body);
      {
        const ir::RegisterIdx value = owner_.emit(
            ir::Opcode::Load, f64_ty, {owner_.to_operand(low_addr, f64_ty)});
        const ir::RegisterIdx grown =
            owner_.emit(ir::Opcode::FMul, f64_ty,
                        {owner_.to_operand(value, f64_ty), f64_imm(10.0)});
        owner_.emit_void(ir::Opcode::Store,
                         {owner_.to_operand(grown, f64_ty),
                          owner_.to_operand(low_addr, f64_ty)});
        const ir::RegisterIdx at = owner_.emit(
            ir::Opcode::Load, i32_ty, {owner_.to_operand(count_addr, i32_ty)});
        const ir::RegisterIdx next =
            owner_.emit(ir::Opcode::IntAdd, i32_ty,
                        {owner_.to_operand(at, i32_ty), i32_imm(1)});
        owner_.emit_void(ir::Opcode::Store,
                         {owner_.to_operand(next, i32_ty),
                          owner_.to_operand(count_addr, i32_ty)});
        owner_.emit_br(low_head);
      }
      owner_.switch_to(low_exit);
      const ir::RegisterIdx low = owner_.emit(
          ir::Opcode::Load, f64_ty, {owner_.to_operand(low_addr, f64_ty)});
      const ir::RegisterIdx whole = owner_.emit(
          ir::Opcode::FMul, f64_ty, {high, owner_.to_operand(low, f64_ty)});
      // A power past the largest double only arises for a subnormal
      // magnitude, where the loop's own mantissa stands in.
      const ir::RegisterIdx overflowed = owner_.emit(
          ir::Opcode::Gt, boolean,
          {owner_.to_operand(whole, f64_ty), f64_imm(1.7976931348623157e308)});
      const ir::RegisterIdx direct_tiny =
          owner_.emit(ir::Opcode::FMul, f64_ty,
                      {owner_.to_operand(magnitude, f64_ty),
                       owner_.to_operand(whole, f64_ty)});
      const ir::RegisterIdx fallback = owner_.emit(
          ir::Opcode::Load, f64_ty, {owner_.to_operand(m_addr, f64_ty)});
      const ir::RegisterIdx tiny =
          owner_.emit(ir::Opcode::Select, f64_ty,
                      {owner_.to_operand(overflowed, boolean),
                       owner_.to_operand(fallback, f64_ty),
                       owner_.to_operand(direct_tiny, f64_ty)});
      const ir::RegisterIdx divided =
          owner_.emit(ir::Opcode::FDiv, f64_ty,
                      {owner_.to_operand(magnitude, f64_ty),
                       owner_.to_operand(whole, f64_ty)});
      const ir::RegisterIdx mantissa = owner_.emit(
          ir::Opcode::Select, f64_ty,
          {owner_.to_operand(big, boolean), owner_.to_operand(divided, f64_ty),
           owner_.to_operand(tiny, f64_ty)});
      // The loop's count can land one place off at a power of ten, so
      // the recomputed mantissa is pulled back into [1, 10) and the
      // exponent follows.
      const ir::RegisterIdx too_big =
          owner_.emit(ir::Opcode::Ge, boolean,
                      {owner_.to_operand(mantissa, f64_ty), f64_imm(10.0)});
      const ir::RegisterIdx too_small =
          owner_.emit(ir::Opcode::Lt, boolean,
                      {owner_.to_operand(mantissa, f64_ty), f64_imm(1.0)});
      const ir::RegisterIdx halved =
          owner_.emit(ir::Opcode::FMul, f64_ty,
                      {owner_.to_operand(mantissa, f64_ty), f64_imm(0.1)});
      const ir::RegisterIdx doubled =
          owner_.emit(ir::Opcode::FMul, f64_ty,
                      {owner_.to_operand(mantissa, f64_ty), f64_imm(10.0)});
      const ir::RegisterIdx picked =
          owner_.emit(ir::Opcode::Select, f64_ty,
                      {owner_.to_operand(too_small, boolean),
                       owner_.to_operand(doubled, f64_ty),
                       owner_.to_operand(mantissa, f64_ty)});
      const ir::RegisterIdx m =
          owner_.emit(ir::Opcode::Select, f64_ty,
                      {owner_.to_operand(too_big, boolean),
                       owner_.to_operand(halved, f64_ty),
                       owner_.to_operand(picked, f64_ty)});
      const ir::RegisterIdx raised =
          owner_.emit(ir::Opcode::IntAdd, i32_ty,
                      {owner_.to_operand(e, i32_ty), i32_imm(1)});
      const ir::RegisterIdx e_high = owner_.emit(
          ir::Opcode::Select, i32_ty,
          {owner_.to_operand(too_big, boolean),
           owner_.to_operand(raised, i32_ty), owner_.to_operand(e, i32_ty)});
      const ir::RegisterIdx lowered =
          owner_.emit(ir::Opcode::IntSub, i32_ty,
                      {owner_.to_operand(e_high, i32_ty), i32_imm(1)});
      const ir::RegisterIdx base_e =
          owner_.emit(ir::Opcode::Select, i32_ty,
                      {owner_.to_operand(too_small, boolean),
                       owner_.to_operand(lowered, i32_ty),
                       owner_.to_operand(e_high, i32_ty)});
      ir::RegisterIdx folded = ir::RegisterIdx(base::INVALID_IDX);
      if (!emit_magnitude(m, digits, trim, /*fold=*/true, folded)) {
        return false;
      }
      // A mantissa that rounded up to ten is one step of the exponent.
      const ir::RegisterIdx bumped =
          owner_.emit(ir::Opcode::IntAdd, i32_ty,
                      {owner_.to_operand(base_e, i32_ty), i32_imm(1)});
      const ir::RegisterIdx exponent =
          owner_.emit(ir::Opcode::Select, i32_ty,
                      {owner_.to_operand(folded, boolean),
                       owner_.to_operand(bumped, i32_ty),
                       owner_.to_operand(base_e, i32_ty)});
      ir::OperandIdx ptr = ir::OperandIdx::invalid();
      ir::OperandIdx len = ir::OperandIdx::invalid();
      if (!const_str("e", ptr, len) || !bounded_copy(ptr, len, len)) {
        return false;
      }
      const ir::RegisterIdx negative_exponent =
          owner_.emit(ir::Opcode::Lt, boolean,
                      {owner_.to_operand(exponent, i32_ty), zero_e});
      if (!emit_optional_byte('-', negative_exponent)) {
        return false;
      }
      const ir::RegisterIdx negated =
          owner_.emit(ir::Opcode::IntSub, i32_ty,
                      {zero_e, owner_.to_operand(exponent, i32_ty)});
      const ir::RegisterIdx magnitude_exponent =
          owner_.emit(ir::Opcode::Select, i32_ty,
                      {owner_.to_operand(negative_exponent, boolean),
                       owner_.to_operand(negated, i32_ty),
                       owner_.to_operand(exponent, i32_ty)});
      const ir::RegisterIdx wide_exponent =
          owner_.emit(ir::Opcode::TypeCast, u64_ty,
                      {owner_.to_operand(magnitude_exponent, i32_ty)});
      if (!emit_u64_digits(wide_exponent)) {
        return false;
      }
      owner_.emit_br(float_join);
    }
    owner_.switch_to(fixed_block);
    {
      ir::RegisterIdx folded = ir::RegisterIdx(base::INVALID_IDX);
      if (!emit_magnitude(magnitude, digits, trim, /*fold=*/false, folded)) {
        return false;
      }
      owner_.emit_br(float_join);
    }
    owner_.switch_to(float_join);
    return !owner_.failed;
  };
  // A nominal argument formats through `fmt`'s `Display`: the method
  // writes the prefix of its rendering that fits into the remaining
  // destination and returns the total length. The measure pass hands
  // it an empty slice, which is how a growable buffer learns the size;
  // one placeholder may run the method twice.
  auto emit_display_piece =
      [&](diag::Span piece_span, ir::RegisterIdx elem_addr, ir::TypeIdx elem_ty,
          const analyzer::CheckedModule::CallTarget* target) {
        const ir::TypeIdx view_ty = owner_.builder.reference_type(
            owner_.builder.slice_type(u8_ty), true);
        Val receiver{owner_.to_operand(elem_addr, elem_ty), elem_ty, true,
                     false};
        if (measure_only) {
          // An empty view: nothing is written and the total answers.
          const ir::RegisterIdx scratch =
              owner_.emit(ir::Opcode::Alloca, u8_ty, {owner_.size_one});
          Val empty = owner_.build_slice_value(
              owner_.to_operand(scratch,
                                owner_.builder.reference_type(u8_ty, true)),
              usize_imm(0), view_ty, piece_span);
          if (owner_.failed) {
            return false;
          }
          const Val call =
              owner_.emit_spec_call(target, piece_span, receiver, empty,
                                    /*has_argument=*/true);
          if (owner_.failed) {
            return false;
          }
          accumulate(call.op);
          return !owner_.failed;
        }
        const ir::RegisterIdx off =
            owner_.emit(ir::Opcode::Load, usize_ty,
                        {owner_.to_operand(state.off_addr, usize_ty)});
        const ir::RegisterIdx remaining =
            owner_.emit(ir::Opcode::IntSub, usize_ty,
                        {state.capacity, owner_.to_operand(off, usize_ty)});
        const ir::OperandIdx dst = owner_.advance_ptr(
            dst_base, owner_.to_operand(off, usize_ty), piece_span);
        if (owner_.failed) {
          return false;
        }
        Val view = owner_.build_slice_value(
            dst, owner_.to_operand(remaining, usize_ty), view_ty, piece_span);
        if (owner_.failed) {
          return false;
        }
        const Val call =
            owner_.emit_spec_call(target, piece_span, receiver, view,
                                  /*has_argument=*/true);
        if (owner_.failed) {
          return false;
        }
        // The method writes at most what fits; the total is what a
        // destination of its own would have taken.
        const ir::RegisterIdx fits =
            owner_.emit(ir::Opcode::Lt, boolean,
                        {call.op, owner_.to_operand(remaining, usize_ty)});
        const ir::RegisterIdx chunk =
            owner_.emit(ir::Opcode::Select, usize_ty,
                        {owner_.to_operand(fits, boolean), call.op,
                         owner_.to_operand(remaining, usize_ty)});
        advance(owner_.to_operand(chunk, usize_ty));
        accumulate(call.op);
        return !owner_.failed;
      };
  for (const analyzer::FmtPiece& piece : pieces) {
    if (owner_.failed) {
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
        owner_.emit(ir::Opcode::GetElementPtr, elem_ty,
                    {tup_op, owner_.zero_i32, owner_.index_operand(piece.arg)});
    const ir::TypeTag tag = owner_.tag_of(elem_ty);
    if (tag == ir::TypeTag::Str) {
      const ir::RegisterIdx loaded = owner_.emit(
          ir::Opcode::Load, elem_ty, {owner_.to_operand(elem_addr, elem_ty)});
      const ir::RegisterIdx ptr = owner_.emit(
          ir::Opcode::ExtractValue, owner_.builder.primitive(ir::TypeTag::Ptr),
          {owner_.to_operand(loaded, elem_ty), owner_.index_operand(0)});
      const ir::RegisterIdx len = owner_.emit(
          ir::Opcode::ExtractValue, usize_ty,
          {owner_.to_operand(loaded, elem_ty), owner_.index_operand(1)});
      if (!bounded_copy(owner_.to_operand(
                            ptr, owner_.builder.primitive(ir::TypeTag::Ptr)),
                        owner_.to_operand(len, usize_ty),
                        owner_.to_operand(len, usize_ty))) {
        return false;
      }
      continue;
    }
    if (tag == ir::TypeTag::I1) {
      const ir::RegisterIdx loaded = owner_.emit(
          ir::Opcode::Load, elem_ty, {owner_.to_operand(elem_addr, elem_ty)});
      ir::OperandIdx true_ptr = ir::OperandIdx::invalid();
      ir::OperandIdx true_len = ir::OperandIdx::invalid();
      ir::OperandIdx false_ptr = ir::OperandIdx::invalid();
      ir::OperandIdx false_len = ir::OperandIdx::invalid();
      if (!const_str("true", true_ptr, true_len) ||
          !const_str("false", false_ptr, false_len)) {
        return false;
      }
      const ir::RegisterIdx ptr = owner_.emit(
          ir::Opcode::Select, owner_.builder.primitive(ir::TypeTag::Ptr),
          {owner_.to_operand(loaded, elem_ty), true_ptr, false_ptr});
      const ir::RegisterIdx len = owner_.emit(
          ir::Opcode::Select, usize_ty,
          {owner_.to_operand(loaded, elem_ty), true_len, false_len});
      if (!bounded_copy(owner_.to_operand(
                            ptr, owner_.builder.primitive(ir::TypeTag::Ptr)),
                        owner_.to_operand(len, usize_ty),
                        owner_.to_operand(len, usize_ty))) {
        return false;
      }
      continue;
    }
    if (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64) {
      const ir::RegisterIdx loaded = owner_.emit(
          ir::Opcode::Load, elem_ty, {owner_.to_operand(elem_addr, elem_ty)});
      const ir::TypeIdx f64_ty = owner_.builder.primitive(ir::TypeTag::F64);
      const ir::RegisterIdx value =
          tag == ir::TypeTag::F32
              ? owner_.emit(ir::Opcode::TypeCast, f64_ty,
                            {owner_.to_operand(loaded, elem_ty)})
              : loaded;
      if (owner_.failed || !emit_float(value, piece.precision)) {
        return false;
      }
      continue;
    }
    if (tag == ir::TypeTag::Struct || tag == ir::TypeTag::Enum) {
      const analyzer::CheckedModule::CallTarget* target =
          piece.arg < arg_exprs.size()
              ? owner_.call_target(arg_exprs[piece.arg])
              : nullptr;
      if (target == nullptr) {
        owner_.unsupported(span, "argument is not formattable");
        return false;
      }
      if (owner_.failed ||
          !emit_display_piece(span, elem_addr, elem_ty, target)) {
        return false;
      }
      continue;
    }
    // Integers format as decimal through a scratch buffer. A signed
    // value is split into sign and magnitude first: widening with a
    // sign-extending cast would print -1 as twenty digits.
    const ir::RegisterIdx loaded = owner_.emit(
        ir::Opcode::Load, elem_ty, {owner_.to_operand(elem_addr, elem_ty)});
    const ir::TypeIdx u64_ty = owner_.builder.primitive(ir::TypeTag::U64);
    ir::RegisterIdx wide = loaded;
    if (ir::is_signed_integer_type(owner_.tag_of(elem_ty))) {
      // The sign is a runtime property, so the digits come from a
      // select between the value and its magnitude. Negating in the
      // source type gives the two's complement; TypeCast picks zero- or
      // sign-extension from the *source* tag, so both arms reinterpret
      // through the same width as an unsigned type before widening.
      const ir::ImmutableIdx zero =
          owner_.builder.immutable({.type = elem_ty, .data = {.i64_value = 0}});
      // The magnitude comes from a wrapping subtraction: the checked
      // form aborts on the type's minimum, whose magnitude the signed
      // range cannot hold, and the reinterpretation below reads the
      // wrapped bits as the unsigned magnitude it is.
      const ir::RegisterIdx magnitude =
          owner_.emit(ir::Opcode::IntSubWrap, elem_ty,
                      {owner_.to_operand(zero, elem_ty),
                       owner_.to_operand(loaded, elem_ty)});
      const ir::TypeIdx same_width = owner_.builder.primitive(
          ir::unsigned_integer_type(owner_.tag_of(elem_ty)));
      auto as_unsigned = [&](ir::RegisterIdx value) -> ir::RegisterIdx {
        const ir::RegisterIdx same =
            owner_.emit(ir::Opcode::TypeCast, same_width,
                        {owner_.to_operand(value, elem_ty)});
        return owner_.emit(ir::Opcode::TypeCast, u64_ty,
                           {owner_.to_operand(same, same_width)});
      };
      const ir::RegisterIdx positive = as_unsigned(loaded);
      const ir::RegisterIdx abs_value = as_unsigned(magnitude);
      const ir::RegisterIdx is_negative =
          owner_.emit(ir::Opcode::Lt, boolean,
                      {owner_.to_operand(loaded, elem_ty),
                       owner_.to_operand(zero, elem_ty)});
      wide = owner_.emit(ir::Opcode::Select, u64_ty,
                         {owner_.to_operand(is_negative, boolean),
                          owner_.to_operand(abs_value, u64_ty),
                          owner_.to_operand(positive, u64_ty)});
      if (!emit_optional_byte('-', is_negative)) {
        return false;
      }
    } else {
      wide = owner_.emit(ir::Opcode::TypeCast, u64_ty,
                         {owner_.to_operand(loaded, elem_ty)});
    }
    if (!emit_u64_digits(wide)) {
      return false;
    }
  }
  return !owner_.failed;
}

bool Lowerer::Fmt::lower_rest_tuple(std::span<const ast::ExprIdx> args,
                                    usize first,
                                    diag::Span span,
                                    Val& out,
                                    std::vector<ir::TypeIdx>& elem_types,
                                    std::vector<ir::TypeIdx>& arg_types) {
  if (first >= args.size()) {
    out = Val{owner_.size_one, owner_.builder.primitive(ir::TypeTag::Void),
              false, false};
    return true;
  }
  // Tuple element slots are contiguous type-table entries whose shapes
  // match the argument types; a copy is one entry, the same convention
  // tuple literals use.
  std::vector<ir::TypeIdx> slots;
  ir::TypeSeq element_seq;
  for (usize i = first; i < args.size(); ++i) {
    const ir::TypeIdx element = owner_.expr_type(args[i]);
    if (owner_.tag_of(element) == ir::TypeTag::Error) {
      return false;
    }
    const ir::TypeIdx slot = owner_.storage_copy(element);
    slots.push_back(slot);
    arg_types.push_back(element);
    element_seq.push(slot);
  }
  const ir::TypeIdx tuple_type =
      owner_.builder.tuple_type(element_seq.finish());
  const ir::RegisterIdx addr =
      owner_.emit(ir::Opcode::Alloca, tuple_type, {owner_.size_one});
  for (usize i = 0; i < slots.size() && !owner_.failed; ++i) {
    const ir::TypeIdx slot = slots[i];
    Val value = owner_.lower_moved(args[first + i], &slot);
    if (owner_.failed) {
      return false;
    }
    const ir::RegisterIdx gep =
        owner_.emit(ir::Opcode::GetElementPtr, slot,
                    {owner_.to_operand(addr, tuple_type), owner_.zero_i32,
                     owner_.index_operand(static_cast<u32>(i))});
    owner_.emit_void(ir::Opcode::Store,
                     {owner_.use_value(value), owner_.to_operand(gep, slot)});
  }
  if (owner_.failed) {
    return false;
  }
  out = Val{owner_.to_operand(addr, tuple_type), tuple_type, true, false};
  elem_types = std::move(slots);
  (void)span;
  return true;
}

Val Lowerer::Fmt::lower_write(ast::ExprIdx expr,
                              const analyzer::CheckedModule::FnSig& sig) {
  const ast::ExprNode& node = owner_.ast.exprs[expr];
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  if (args.size() < 2) {
    owner_.internal(node.span, "call arity");
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  CompVal fmt_val;
  if (!owner_.comp_lowering_.evaluate(owner_.module, args[0], fmt_val) ||
      fmt_val.value.tag != CompValue::Tag::Str) {
    if (!owner_.failed) {
      owner_.internal(node.span, "format string without value");
    }
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  Val buf = owner_.lower_expr(args[1], nullptr);
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  if (owner_.tag_of(buf.type) != ir::TypeTag::MutRef) {
    owner_.internal(node.span, "buffer without address");
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::TypeIdx buf_pointee =
      owner_.builder.ref_types()[owner_.builder.types()[buf.type].as_ref()]
          .pointee;
  if (owner_.tag_of(buf_pointee) != ir::TypeTag::Array) {
    owner_.internal(node.span, "buffer without address");
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const u64 capacity =
      owner_.builder
          .array_types()[owner_.builder.types()[buf_pointee].as_array()]
          .count;
  // The arguments after the buffer are the format arguments; they are
  // collected into the tuple the pieces read (ADR-0055).
  std::vector<ir::TypeIdx> elem_types;
  std::vector<ir::TypeIdx> arg_types;
  Val tup{owner_.size_one, owner_.error_type(), false, false};
  if (!lower_rest_tuple(args, 2, node.span, tup, elem_types, arg_types)) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const analyzer::FmtTemplate parsed =
      analyzer::parse_format_string(fmt_val.value.str_value);
  if (parsed.error != analyzer::FmtError::None) {
    owner_.unsupported(node.span, "invalid format string");
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  if (parsed.placeholders != elem_types.size()) {
    owner_.unsupported(node.span, "placeholder count does not match arguments");
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  if (!check_arguments(node.span, parsed, elem_types)) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::TypeIdx usize_ty = owner_.usize_type();
  const ir::OperandIdx capacity_op = owner_.to_operand(
      owner_.builder.immutable(
          {.type = usize_ty, .data = {.u64_value = capacity}}),
      usize_ty);
  State state;
  if (!emit_pieces(node.span, parsed.pieces, tup.op, elem_types,
                   args.subspan(2), buf.op, capacity_op, state, false)) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  // The arguments moved into the tuple are the expansion's to retire:
  // nothing else owns them once the pieces ran, and the tuple is not a
  // source binding the drop pass could schedule.
  owner_.emit_collected_drop(tup.op, arg_types, node.span);
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::RegisterIdx written =
      owner_.emit(ir::Opcode::Load, usize_ty,
                  {owner_.to_operand(state.off_addr, usize_ty)});
  const ir::RegisterIdx total =
      owner_.emit(ir::Opcode::Load, usize_ty,
                  {owner_.to_operand(state.tot_addr, usize_ty)});
  const ir::RegisterIdx slot =
      owner_.emit(ir::Opcode::Alloca, sig.ret, {owner_.size_one});
  const ir::TypeIdx outcome_written =
      owner_.field_type_of(sig.ret, 0, node.span);
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::TypeIdx outcome_total = owner_.field_type_of(sig.ret, 1, node.span);
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::RegisterIdx field0 =
      owner_.emit(ir::Opcode::GetElementPtr, outcome_written,
                  {owner_.to_operand(slot, sig.ret), owner_.zero_i32,
                   owner_.index_operand(0)});
  owner_.emit_void(ir::Opcode::Store,
                   {owner_.to_operand(written, outcome_written),
                    owner_.to_operand(field0, outcome_written)});
  const ir::RegisterIdx field1 =
      owner_.emit(ir::Opcode::GetElementPtr, outcome_total,
                  {owner_.to_operand(slot, sig.ret), owner_.zero_i32,
                   owner_.index_operand(1)});
  owner_.emit_void(ir::Opcode::Store,
                   {owner_.to_operand(total, outcome_total),
                    owner_.to_operand(field1, outcome_total)});
  return Val{owner_.to_operand(slot, sig.ret), sig.ret, true, false};
}

Val Lowerer::Fmt::lower_format(ast::ExprIdx expr,
                               const analyzer::CheckedModule::FnSig& sig) {
  const ast::ExprNode& node = owner_.ast.exprs[expr];
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  if (args.size() < 1) {
    owner_.internal(node.span, "call arity");
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  CompVal fmt_val;
  if (!owner_.comp_lowering_.evaluate(owner_.module, args[0], fmt_val) ||
      fmt_val.value.tag != CompValue::Tag::Str) {
    if (!owner_.failed) {
      owner_.internal(node.span, "format string without value");
    }
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  // The arguments after the format string are collected into the tuple
  // the pieces read (ADR-0055).
  std::vector<ir::TypeIdx> elem_types;
  std::vector<ir::TypeIdx> arg_types;
  Val tup{owner_.size_one, owner_.error_type(), false, false};
  if (!lower_rest_tuple(args, 1, node.span, tup, elem_types, arg_types)) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  ir::OperandIdx tup_op = tup.op;
  const analyzer::FmtTemplate parsed =
      analyzer::parse_format_string(fmt_val.value.str_value);
  if (parsed.error != analyzer::FmtError::None) {
    owner_.unsupported(node.span, "invalid format string");
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  if (parsed.placeholders != elem_types.size()) {
    owner_.unsupported(node.span, "placeholder count does not match arguments");
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  if (!check_arguments(node.span, parsed, elem_types)) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::TypeIdx usize_ty = owner_.usize_type();
  const ir::TypeIdx u8_ty = owner_.builder.primitive(ir::TypeTag::U8);
  u32 buf_field = 0;
  u32 len_field = 1;
  u32 cap_field = 2;
  if (!owner_.struct_field_index(sig.ret, "buf", buf_field) ||
      !owner_.struct_field_index(sig.ret, "len", len_field) ||
      !owner_.struct_field_index(sig.ret, "cap", cap_field)) {
    owner_.internal(node.span, "string without fields");
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::TypeIdx buf_field_ty =
      owner_.field_type_of(sig.ret, buf_field, node.span);
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  // Phase one measures: the pieces run with no destination, and the
  // total they accrue is the exact size the heap buffer reserves. The
  // tuple is lowered once up front, so its side effects happen once
  // and both passes read the same values.
  State measured;
  const ir::OperandIdx no_capacity = owner_.to_operand(
      owner_.builder.immutable({.type = usize_ty, .data = {.u64_value = 0}}),
      usize_ty);
  if (!emit_pieces(node.span, parsed.pieces, tup_op, elem_types,
                   args.subspan(1), owner_.size_one, no_capacity, measured,
                   true)) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::RegisterIdx total =
      owner_.emit(ir::Opcode::Load, usize_ty,
                  {owner_.to_operand(measured.tot_addr, usize_ty)});
  const ir::RegisterIdx heap = owner_.emit_heap_alloc(
      u8_ty, buf_field_ty, owner_.to_operand(total, usize_ty), node.span);
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::RegisterIdx slot =
      owner_.emit(ir::Opcode::Alloca, sig.ret, {owner_.size_one});
  const ir::RegisterIdx buf_addr =
      owner_.emit(ir::Opcode::GetElementPtr, buf_field_ty,
                  {owner_.to_operand(slot, sig.ret), owner_.zero_i32,
                   owner_.index_operand(buf_field)});
  owner_.emit_void(ir::Opcode::Store,
                   {owner_.to_operand(heap, buf_field_ty),
                    owner_.to_operand(buf_addr, buf_field_ty)});
  const ir::TypeIdx len_ty =
      owner_.field_type_of(sig.ret, len_field, node.span);
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::TypeIdx cap_ty =
      owner_.field_type_of(sig.ret, cap_field, node.span);
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::RegisterIdx cap_addr =
      owner_.emit(ir::Opcode::GetElementPtr, cap_ty,
                  {owner_.to_operand(slot, sig.ret), owner_.zero_i32,
                   owner_.index_operand(cap_field)});
  owner_.emit_void(ir::Opcode::Store, {owner_.to_operand(total, usize_ty),
                                       owner_.to_operand(cap_addr, cap_ty)});
  // Phase two expands into the reserved buffer. The reservation is the
  // measured total, so nothing truncates; the length stored below is
  // the bytes the second pass wrote.
  State state;
  if (!emit_pieces(node.span, parsed.pieces, tup_op, elem_types,
                   args.subspan(1), owner_.to_operand(heap, buf_field_ty),
                   owner_.to_operand(total, usize_ty), state, false)) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  // The arguments moved into the tuple are the expansion's to retire.
  owner_.emit_collected_drop(tup_op, arg_types, node.span);
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::RegisterIdx written =
      owner_.emit(ir::Opcode::Load, usize_ty,
                  {owner_.to_operand(state.off_addr, usize_ty)});
  const ir::RegisterIdx len_addr =
      owner_.emit(ir::Opcode::GetElementPtr, len_ty,
                  {owner_.to_operand(slot, sig.ret), owner_.zero_i32,
                   owner_.index_operand(len_field)});
  owner_.emit_void(ir::Opcode::Store, {owner_.to_operand(written, len_ty),
                                       owner_.to_operand(len_addr, len_ty)});
  return Val{owner_.to_operand(slot, sig.ret), sig.ret, true, false};
}

namespace {
// The runtime writer a comp `fmt` write names: stdout for `print` and
// `println`, stderr for `eprint` and `eprintln`.
std::string_view io_write_hook(std::string_view name) {
  if (name == "eprintln") {
    return "alcy_eprintln";
  }
  if (name == "eprint") {
    return "alcy_eprint";
  }
  if (name == "println") {
    return "alcy_println";
  }
  return "alcy_print";
}
}  // namespace

Val Lowerer::Fmt::lower_print(ast::ExprIdx expr,
                              const analyzer::CheckedModule::FnSig& sig) {
  const ast::ExprNode& node = owner_.ast.exprs[expr];
  const std::span<const ast::ExprIdx> args =
      node.payload.get<ast::ExprCall>().args;
  if (args.empty()) {
    owner_.internal(node.span, "call arity");
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  CompVal fmt_val;
  if (!owner_.comp_lowering_.evaluate(owner_.module, args[0], fmt_val) ||
      fmt_val.value.tag != CompValue::Tag::Str) {
    if (!owner_.failed) {
      owner_.internal(node.span, "format string without value");
    }
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  // The arguments after the format string are collected into the tuple
  // the pieces read (ADR-0055).
  std::vector<ir::TypeIdx> elem_types;
  std::vector<ir::TypeIdx> arg_types;
  Val tup{owner_.size_one, owner_.error_type(), false, false};
  if (!lower_rest_tuple(args, 1, node.span, tup, elem_types, arg_types)) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const analyzer::FmtTemplate parsed =
      analyzer::parse_format_string(fmt_val.value.str_value);
  if (parsed.error != analyzer::FmtError::None) {
    owner_.unsupported(node.span, "invalid format string");
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  // No placeholders: the text is one compile-time string, so it prints
  // from the constant pool with nothing measured, allocated, or freed.
  if (parsed.placeholders == 0 && args.size() == 1) {
    std::string text;
    for (const analyzer::FmtPiece& piece : parsed.pieces) {
      text.append(piece.literal);
    }
    const ir::TypeIdx str_ty = owner_.builder.primitive(ir::TypeTag::Str);
    const str::StringPoolId id = owner_.intern_copied(text);
    if (id == str::INVALID_STRING_POOL_ID) {
      owner_.internal(node.span, "format text without bytes");
      return Val{owner_.size_one, owner_.error_type(), false, false};
    }
    const ir::ImmutableIdx imm = owner_.builder.immutable(
        {.type = str_ty, .data = {.str_id_value = id}});
    const ir::OperandIdx str_op = owner_.to_operand(imm, str_ty);
    const ir::TypeIdx ptr_ty = owner_.builder.primitive(ir::TypeTag::Ptr);
    const ir::TypeIdx usize_ty = owner_.usize_type();
    const ir::RegisterIdx bytes = owner_.emit(
        ir::Opcode::ExtractValue, ptr_ty, {str_op, owner_.index_operand(0)});
    const ir::RegisterIdx len = owner_.emit(ir::Opcode::ExtractValue, usize_ty,
                                            {str_op, owner_.index_operand(1)});
    const ir::ExternalFunctionIdx write_ext = owner_.declare_external(
        io_write_hook(sig.name), owner_.builder.primitive(ir::TypeTag::Void),
        {ptr_ty, usize_ty});
    owner_.emit_void(
        ir::Opcode::Call,
        {owner_.builder.operand(ir::Operand::from_external_function(
             write_ext, owner_.builder.primitive(ir::TypeTag::Function))),
         owner_.to_operand(bytes, ptr_ty), owner_.to_operand(len, usize_ty)});
    return Val{owner_.size_one, owner_.builder.primitive(ir::TypeTag::Void),
               false, false};
  }
  if (parsed.placeholders != elem_types.size()) {
    owner_.unsupported(node.span, "placeholder count does not match arguments");
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  if (!check_arguments(node.span, parsed, elem_types)) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::TypeIdx usize_ty = owner_.usize_type();
  const ir::TypeIdx u8_ty = owner_.builder.primitive(ir::TypeTag::U8);
  const ir::TypeIdx ptr_ty = owner_.builder.primitive(ir::TypeTag::Ptr);
  const ir::TypeIdx buf_ty = owner_.builder.reference_type(u8_ty, true);
  // Every argument with a fixed width makes the total statically
  // bounded, and a bounded total writes once into a stack buffer:
  // nothing is measured, allocated, or freed. A `str` argument has no
  // static bound, so it keeps the measuring heap path.
  u64 bound = 0;
  bool bounded = true;
  for (const analyzer::FmtPiece& piece : parsed.pieces) {
    if (!piece.is_arg) {
      bound += piece.literal.size();
      continue;
    }
    switch (owner_.tag_of(elem_types[piece.arg])) {
      case ir::TypeTag::I1: bound += 5; break;
      case ir::TypeTag::I8: bound += 4; break;
      case ir::TypeTag::U8: bound += 3; break;
      case ir::TypeTag::I16: bound += 6; break;
      case ir::TypeTag::U16: bound += 5; break;
      case ir::TypeTag::I32: bound += 11; break;
      case ir::TypeTag::U32: bound += 10; break;
      case ir::TypeTag::I64:
      case ir::TypeTag::U64: bound += 20; break;
      default: bounded = false; break;
    }
    if (!bounded) {
      break;
    }
  }
  if (bounded) {
    const ir::TypeIdx usize_ty = owner_.usize_type();
    const ir::RegisterIdx slot =
        owner_.emit(ir::Opcode::Alloca, u8_ty, {owner_.const_usize(bound)});
    State state;
    if (!emit_pieces(node.span, parsed.pieces, tup.op, elem_types,
                     args.subspan(1), owner_.to_operand(slot, buf_ty),
                     owner_.const_usize(bound), state, false)) {
      return Val{owner_.size_one, owner_.error_type(), false, false};
    }
    const ir::RegisterIdx len =
        owner_.emit(ir::Opcode::Load, usize_ty,
                    {owner_.to_operand(state.tot_addr, usize_ty)});
    if (owner_.failed) {
      return Val{owner_.size_one, owner_.error_type(), false, false};
    }
    const ir::ExternalFunctionIdx write_ext = owner_.declare_external(
        io_write_hook(sig.name), owner_.builder.primitive(ir::TypeTag::Void),
        {owner_.builder.primitive(ir::TypeTag::Ptr), usize_ty});
    owner_.emit_void(
        ir::Opcode::Call,
        {owner_.builder.operand(ir::Operand::from_external_function(
             write_ext, owner_.builder.primitive(ir::TypeTag::Function))),
         owner_.to_operand(slot, buf_ty), owner_.to_operand(len, usize_ty)});
    return Val{owner_.size_one, owner_.builder.primitive(ir::TypeTag::Void),
               false, false};
  }
  // Phase one measures; the total is the exact reservation. The tuple
  // is lowered once up front, so its side effects happen once and both
  // passes read the same values.
  State measured;
  const ir::OperandIdx no_capacity = owner_.to_operand(
      owner_.builder.immutable({.type = usize_ty, .data = {.u64_value = 0}}),
      usize_ty);
  if (!emit_pieces(node.span, parsed.pieces, tup.op, elem_types,
                   args.subspan(1), owner_.size_one, no_capacity, measured,
                   true)) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::RegisterIdx total =
      owner_.emit(ir::Opcode::Load, usize_ty,
                  {owner_.to_operand(measured.tot_addr, usize_ty)});
  const ir::RegisterIdx heap = owner_.emit_heap_alloc(
      u8_ty, buf_ty, owner_.to_operand(total, usize_ty), node.span);
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  // Phase two writes the whole measured total, so nothing truncates.
  State state;
  if (!emit_pieces(node.span, parsed.pieces, tup.op, elem_types,
                   args.subspan(1), owner_.to_operand(heap, buf_ty),
                   owner_.to_operand(total, usize_ty), state, false)) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  // The arguments moved into the tuple are the expansion's to retire.
  owner_.emit_collected_drop(tup.op, arg_types, node.span);
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  // The hook writes stdout; println adds the newline in a second write.
  const ir::ExternalFunctionIdx write_ext = owner_.declare_external(
      io_write_hook(sig.name), owner_.builder.primitive(ir::TypeTag::Void),
      {ptr_ty, usize_ty});
  owner_.emit_void(
      ir::Opcode::Call,
      {owner_.builder.operand(ir::Operand::from_external_function(
           write_ext, owner_.builder.primitive(ir::TypeTag::Function))),
       owner_.to_operand(heap, buf_ty), owner_.to_operand(total, usize_ty)});
  // The buffer is the expansion's; hand it back with the size and
  // alignment it was reserved at.
  const ir::RegisterIdx align =
      owner_.emit_type_query(ir::Opcode::TypeAlignOf, u8_ty);
  if (owner_.failed) {
    return Val{owner_.size_one, owner_.error_type(), false, false};
  }
  const ir::ExternalFunctionIdx free_ext = owner_.declare_external(
      "alcy_dealloc", owner_.builder.primitive(ir::TypeTag::Void),
      {ptr_ty, usize_ty, usize_ty});
  owner_.emit_void(
      ir::Opcode::Call,
      {owner_.builder.operand(ir::Operand::from_external_function(
           free_ext, owner_.builder.primitive(ir::TypeTag::Function))),
       owner_.to_operand(heap, buf_ty), owner_.to_operand(total, usize_ty),
       owner_.to_operand(align, usize_ty)});
  return Val{owner_.size_one, owner_.builder.primitive(ir::TypeTag::Void),
             false, false};
}
}  // namespace lowering
