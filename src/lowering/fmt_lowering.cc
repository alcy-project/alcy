// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdlib>
#include <optional>
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

const analyzer::CheckedModule::FnSig* Lowerer::Fmt::find_helper(
    std::string_view name,
    u32& module) const {
  for (u32 i = 0; i < owner_.pkg.modules.size(); ++i) {
    for (const analyzer::CheckedModule::FnSig& sig :
         owner_.pkg.modules[i].functions) {
      // Only the staged fmt package owns these helpers. A user function
      // of the same name must not redirect a format expansion.
      if (sig.name == name && owner_.pkg.tree.is_staged_item("fmt", sig.item)) {
        module = i;
        return &sig;
      }
    }
  }
  return nullptr;
}

bool Lowerer::Fmt::helper_signature_ok(
    const analyzer::CheckedModule::FnSig& sig,
    usize value_count,
    std::optional<ir::TypeTag> value_tag) const {
  if (sig.params.size() != value_count + 2 ||
      owner_.tag_of(sig.ret) != ir::TypeTag::Usize) {
    return false;
  }
  const ir::TypeIdx out = sig.params[value_count];
  const ir::TypeIdx cap = sig.params[value_count + 1];
  if (owner_.tag_of(out) != ir::TypeTag::RawMutPtr ||
      owner_.tag_of(cap) != ir::TypeTag::Usize) {
    return false;
  }
  const ir::TypeIdx pointee =
      owner_.builder.ref_types()[owner_.builder.types()[out].as_ref()].pointee;
  if (owner_.tag_of(pointee) != ir::TypeTag::U8) {
    return false;
  }
  return !value_tag.has_value() || owner_.tag_of(sig.params[0]) == *value_tag;
}

bool Lowerer::Fmt::emit_primitive_helper(diag::Span span,
                                         std::string_view name,
                                         ir::OperandIdx value,
                                         std::optional<ir::TypeTag> value_tag,
                                         State& state) {
  u32 module = 0;
  const analyzer::CheckedModule::FnSig* sig = find_helper(name, module);
  if (sig == nullptr || !helper_signature_ok(*sig, 1, value_tag)) {
    return false;
  }
  const ir::TypeIdx usize_ty = owner_.usize_type();
  const ir::TypeIdx boolean = owner_.builder.primitive(ir::TypeTag::I1);
  const ir::TypeIdx u8_ty = owner_.builder.primitive(ir::TypeTag::U8);
  ir::OperandIdx ptr_op = ir::OperandIdx::invalid();
  ir::OperandIdx cap_op = usize_imm(0);
  if (state.measure_only) {
    const ir::RegisterIdx scratch =
        owner_.emit(ir::Opcode::Alloca, u8_ty, {owner_.size_one});
    ptr_op =
        owner_.to_operand(scratch, owner_.builder.reference_type(u8_ty, true));
  } else {
    const ir::RegisterIdx off =
        owner_.emit(ir::Opcode::Load, usize_ty,
                    {owner_.to_operand(state.off_addr, usize_ty)});
    const ir::RegisterIdx remaining =
        owner_.emit(ir::Opcode::IntSub, usize_ty,
                    {state.capacity, owner_.to_operand(off, usize_ty)});
    const ir::OperandIdx dst = owner_.advance_ptr(
        state.dst_base, owner_.to_operand(off, usize_ty), span);
    if (owner_.failed) {
      return false;
    }
    ptr_op = dst;
    cap_op = owner_.to_operand(remaining, usize_ty);
  }
  if (owner_.failed) {
    return false;
  }
  const ir::FunctionIdx fn =
      owner_.fn_index(module, sig->item, sig->name, sig->params, sig->ret,
                      sig->inst, std::vector<comp::CompVal>{});
  if (!fn.is_valid()) {
    owner_.internal(span, "fmt helper without a function");
    return false;
  }
  std::vector<ir::OperandIdx> ops;
  ops.push_back(owner_.builder.operand(ir::Operand::from_function(
      fn, owner_.builder.primitive(ir::TypeTag::Function))));
  ops.push_back(value);
  ops.push_back(ptr_op);
  ops.push_back(cap_op);
  const ir::RegisterIdx dst = owner_.emit(ir::Opcode::Call, sig->ret, ops);
  if (owner_.failed) {
    return false;
  }
  if (!state.measure_only) {
    const ir::RegisterIdx off =
        owner_.emit(ir::Opcode::Load, usize_ty,
                    {owner_.to_operand(state.off_addr, usize_ty)});
    const ir::RegisterIdx remaining =
        owner_.emit(ir::Opcode::IntSub, usize_ty,
                    {state.capacity, owner_.to_operand(off, usize_ty)});
    const ir::RegisterIdx fits =
        owner_.emit(ir::Opcode::Lt, boolean,
                    {owner_.to_operand(dst, sig->ret),
                     owner_.to_operand(remaining, usize_ty)});
    const ir::RegisterIdx chunk = owner_.emit(
        ir::Opcode::Select, usize_ty,
        {owner_.to_operand(fits, boolean), owner_.to_operand(dst, sig->ret),
         owner_.to_operand(remaining, usize_ty)});
    advance(owner_.to_operand(chunk, usize_ty), state);
  }
  accumulate(owner_.to_operand(dst, sig->ret), state);
  return !owner_.failed;
}

bool Lowerer::Fmt::emit_float_helper(diag::Span span,
                                     ir::OperandIdx value,
                                     u8 precision,
                                     State& state) {
  u32 module = 0;
  const analyzer::CheckedModule::FnSig* sig =
      find_helper("display_f64", module);
  if (sig == nullptr || !helper_signature_ok(*sig, 3, ir::TypeTag::F64)) {
    return false;
  }
  const ir::TypeIdx f64_ty = owner_.builder.primitive(ir::TypeTag::F64);
  const ir::TypeIdx usize_ty = owner_.usize_type();
  const ir::TypeIdx boolean = owner_.builder.primitive(ir::TypeTag::I1);
  const ir::TypeIdx u8_ty = owner_.builder.primitive(ir::TypeTag::U8);
  ir::OperandIdx ptr_op = ir::OperandIdx::invalid();
  ir::OperandIdx cap_op = usize_imm(0);
  if (state.measure_only) {
    const ir::RegisterIdx scratch =
        owner_.emit(ir::Opcode::Alloca, u8_ty, {owner_.size_one});
    ptr_op =
        owner_.to_operand(scratch, owner_.builder.reference_type(u8_ty, true));
  } else {
    const ir::RegisterIdx off =
        owner_.emit(ir::Opcode::Load, usize_ty,
                    {owner_.to_operand(state.off_addr, usize_ty)});
    const ir::RegisterIdx remaining =
        owner_.emit(ir::Opcode::IntSub, usize_ty,
                    {state.capacity, owner_.to_operand(off, usize_ty)});
    ptr_op = owner_.advance_ptr(state.dst_base,
                                owner_.to_operand(off, usize_ty), span);
    cap_op = owner_.to_operand(remaining, usize_ty);
  }
  if (owner_.failed) {
    return false;
  }
  const ir::FunctionIdx fn =
      owner_.fn_index(module, sig->item, sig->name, sig->params, sig->ret,
                      sig->inst, std::vector<comp::CompVal>{});
  if (!fn.is_valid()) {
    owner_.internal(span, "fmt float helper without a function");
    return false;
  }
  const bool trim = precision == analyzer::FMT_NO_PRECISION;
  const ir::OperandIdx digits = owner_.to_operand(
      owner_.builder.immutable(
          {.type = usize_ty,
           .data = {.u64_value = trim ? 6u : static_cast<u64>(precision)}}),
      usize_ty);
  const ir::OperandIdx trim_op = owner_.to_operand(
      owner_.builder.immutable({.type = boolean, .data = {.i1_value = trim}}),
      boolean);
  std::vector<ir::OperandIdx> ops;
  ops.push_back(owner_.builder.operand(ir::Operand::from_function(
      fn, owner_.builder.primitive(ir::TypeTag::Function))));
  ops.push_back(value);
  ops.push_back(digits);
  ops.push_back(trim_op);
  ops.push_back(ptr_op);
  ops.push_back(cap_op);
  const ir::RegisterIdx written = owner_.emit(ir::Opcode::Call, sig->ret, ops);
  if (owner_.failed) {
    return false;
  }
  if (!state.measure_only) {
    const ir::RegisterIdx fits =
        owner_.emit(ir::Opcode::Lt, boolean,
                    {owner_.to_operand(written, sig->ret), cap_op});
    const ir::RegisterIdx chunk =
        owner_.emit(ir::Opcode::Select, usize_ty,
                    {owner_.to_operand(fits, boolean),
                     owner_.to_operand(written, sig->ret), cap_op});
    advance(owner_.to_operand(chunk, usize_ty), state);
  }
  accumulate(owner_.to_operand(written, sig->ret), state);
  (void)f64_ty;
  return !owner_.failed;
}

bool Lowerer::Fmt::emit_pieces(diag::Span span,
                               const std::vector<analyzer::FmtPiece>& pieces,
                               State& state) {
  const ir::TypeIdx boolean = owner_.builder.primitive(ir::TypeTag::I1);
  const ir::TypeIdx usize_ty = owner_.usize_type();
  state.off_addr = owner_.emit(ir::Opcode::Alloca, usize_ty, {owner_.size_one});
  owner_.emit_void(ir::Opcode::Store,
                   {usize_imm(0), owner_.to_operand(state.off_addr, usize_ty)});
  state.tot_addr = owner_.emit(ir::Opcode::Alloca, usize_ty, {owner_.size_one});
  owner_.emit_void(ir::Opcode::Store,
                   {usize_imm(0), owner_.to_operand(state.tot_addr, usize_ty)});
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
      if (!bounded_copy(span, ptr, len, len, state)) {
        return false;
      }
      continue;
    }
    const ir::TypeIdx elem_ty = state.elem_types[piece.arg];
    const ir::RegisterIdx elem_addr = owner_.emit(
        ir::Opcode::GetElementPtr, elem_ty,
        {state.tup_op, owner_.zero_i32, owner_.index_operand(piece.arg)});
    const ir::TypeTag tag = owner_.tag_of(elem_ty);
    if (tag == ir::TypeTag::Str) {
      const ir::RegisterIdx loaded = owner_.emit(
          ir::Opcode::Load, elem_ty, {owner_.to_operand(elem_addr, elem_ty)});
      if (!emit_primitive_helper(span, "display_str",
                                 owner_.to_operand(loaded, elem_ty),
                                 ir::TypeTag::Str, state)) {
        owner_.internal(span, "fmt string helper unavailable");
        return false;
      }
      continue;
    }
    if (tag == ir::TypeTag::I1) {
      const ir::RegisterIdx loaded = owner_.emit(
          ir::Opcode::Load, elem_ty, {owner_.to_operand(elem_addr, elem_ty)});
      if (!emit_primitive_helper(span, "display_bool",
                                 owner_.to_operand(loaded, elem_ty),
                                 ir::TypeTag::I1, state)) {
        owner_.internal(span, "fmt bool helper unavailable");
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
      if (!emit_float_helper(span, owner_.to_operand(value, f64_ty),
                             piece.precision, state)) {
        owner_.internal(span, "fmt float helper unavailable");
        return false;
      }
      continue;
    }
    if (tag == ir::TypeTag::Struct || tag == ir::TypeTag::Enum ||
        tag == ir::TypeTag::Param) {
      const analyzer::CheckedModule::CallTarget* target =
          piece.arg < state.arg_exprs.size()
              ? owner_.display_target(state.arg_exprs[piece.arg])
              : nullptr;
      if (target == nullptr) {
        owner_.unsupported(span, "argument is not formattable");
        return false;
      }
      if (owner_.failed ||
          !emit_display_piece(span, elem_addr, elem_ty, target, state)) {
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
      if (!emit_optional_byte(span, '-', is_negative, state)) {
        return false;
      }
    } else {
      wide = owner_.emit(ir::Opcode::TypeCast, u64_ty,
                         {owner_.to_operand(loaded, elem_ty)});
    }
    if (!emit_primitive_helper(span, "display_u64",
                               owner_.to_operand(wide, u64_ty),
                               ir::TypeTag::U64, state)) {
      owner_.internal(span, "fmt integer helper unavailable");
      return false;
    }
    continue;
  }
  return !owner_.failed;
}

ir::OperandIdx Lowerer::Fmt::usize_imm(u64 value) {
  const ir::TypeIdx usize_ty = owner_.usize_type();
  return owner_.to_operand(
      owner_.builder.immutable(
          {.type = usize_ty, .data = {.u64_value = value}}),
      usize_ty);
}

void Lowerer::Fmt::advance(ir::OperandIdx delta, State& state) {
  const ir::TypeIdx usize_ty = owner_.usize_type();
  const ir::RegisterIdx off =
      owner_.emit(ir::Opcode::Load, usize_ty,
                  {owner_.to_operand(state.off_addr, usize_ty)});
  const ir::RegisterIdx grown = owner_.emit(
      ir::Opcode::IntAdd, usize_ty, {owner_.to_operand(off, usize_ty), delta});
  owner_.emit_void(ir::Opcode::Store,
                   {owner_.to_operand(grown, usize_ty),
                    owner_.to_operand(state.off_addr, usize_ty)});
}

void Lowerer::Fmt::accumulate(ir::OperandIdx full, State& state) {
  const ir::TypeIdx usize_ty = owner_.usize_type();
  const ir::RegisterIdx total =
      owner_.emit(ir::Opcode::Load, usize_ty,
                  {owner_.to_operand(state.tot_addr, usize_ty)});
  const ir::RegisterIdx grown = owner_.emit(
      ir::Opcode::IntAdd, usize_ty, {owner_.to_operand(total, usize_ty), full});
  owner_.emit_void(ir::Opcode::Store,
                   {owner_.to_operand(grown, usize_ty),
                    owner_.to_operand(state.tot_addr, usize_ty)});
}

bool Lowerer::Fmt::bounded_copy(diag::Span span,
                                ir::OperandIdx src,
                                ir::OperandIdx len,
                                ir::OperandIdx full,
                                State& state) {
  const ir::TypeIdx usize_ty = owner_.usize_type();
  const ir::TypeIdx boolean = owner_.builder.primitive(ir::TypeTag::I1);
  if (state.measure_only) {
    accumulate(full, state);
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
  const ir::OperandIdx dst = owner_.advance_ptr(
      state.dst_base, owner_.to_operand(off, usize_ty), span);
  if (owner_.failed) {
    return false;
  }
  owner_.emit_void(ir::Opcode::Memcopy,
                   {dst, src, owner_.to_operand(chunk, usize_ty)});
  advance(owner_.to_operand(chunk, usize_ty), state);
  accumulate(full, state);
  return !owner_.failed;
}

bool Lowerer::Fmt::const_str(const std::string& bytes,
                             ir::OperandIdx& ptr_out,
                             ir::OperandIdx& len_out) {
  const ir::TypeIdx usize_ty = owner_.usize_type();
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
  ptr_out = owner_.to_operand(ptr, owner_.builder.primitive(ir::TypeTag::Ptr));
  len_out = owner_.to_operand(len, usize_ty);
  return true;
}

bool Lowerer::Fmt::emit_optional_byte(diag::Span span,
                                      u8 byte,
                                      ir::RegisterIdx present,
                                      State& state) {
  const ir::TypeIdx usize_ty = owner_.usize_type();
  const ir::TypeIdx u8_ty = owner_.builder.primitive(ir::TypeTag::U8);
  const ir::TypeIdx boolean = owner_.builder.primitive(ir::TypeTag::I1);
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
  return bounded_copy(span, owner_.to_operand(ptr, ptr_ty),
                      owner_.to_operand(len, usize_ty),
                      owner_.to_operand(len, usize_ty), state);
}
// The decimal digits of `wide`, most significant first, through a
// scratch buffer the loop fills from its end backwards.
bool Lowerer::Fmt::emit_display_piece(
    diag::Span piece_span,
    ir::RegisterIdx elem_addr,
    ir::TypeIdx elem_ty,
    const analyzer::CheckedModule::CallTarget* target,
    State& state) {
  const ir::TypeIdx usize_ty = owner_.usize_type();
  const ir::TypeIdx u8_ty = owner_.builder.primitive(ir::TypeTag::U8);
  const ir::TypeIdx boolean = owner_.builder.primitive(ir::TypeTag::I1);
  const ir::TypeIdx view_ty =
      owner_.builder.reference_type(owner_.builder.slice_type(u8_ty), true);
  Val receiver{owner_.to_operand(elem_addr, elem_ty), elem_ty, true, false};
  if (state.measure_only) {
    // An empty view: nothing is written and the total answers.
    const ir::RegisterIdx scratch =
        owner_.emit(ir::Opcode::Alloca, u8_ty, {owner_.size_one});
    Val empty = owner_.build_slice_value(
        owner_.to_operand(scratch, owner_.builder.reference_type(u8_ty, true)),
        usize_imm(0), view_ty, piece_span);
    if (owner_.failed) {
      return false;
    }
    const Val call = owner_.emit_spec_call(target, piece_span, receiver, empty,
                                           /*has_argument=*/true);
    if (owner_.failed) {
      return false;
    }
    accumulate(call.op, state);
    return !owner_.failed;
  }
  const ir::RegisterIdx off =
      owner_.emit(ir::Opcode::Load, usize_ty,
                  {owner_.to_operand(state.off_addr, usize_ty)});
  const ir::RegisterIdx remaining =
      owner_.emit(ir::Opcode::IntSub, usize_ty,
                  {state.capacity, owner_.to_operand(off, usize_ty)});
  const ir::OperandIdx dst = owner_.advance_ptr(
      state.dst_base, owner_.to_operand(off, usize_ty), piece_span);
  if (owner_.failed) {
    return false;
  }
  Val view = owner_.build_slice_value(
      dst, owner_.to_operand(remaining, usize_ty), view_ty, piece_span);
  if (owner_.failed) {
    return false;
  }
  const Val call = owner_.emit_spec_call(target, piece_span, receiver, view,
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
  advance(owner_.to_operand(chunk, usize_ty), state);
  accumulate(call.op, state);
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

}  // namespace lowering
