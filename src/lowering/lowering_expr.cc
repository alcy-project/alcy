// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "base/nesting.h"
#include "comp/comp_value.h"
#include "diag/bag.h"
#include "diag/render.h"
#include "diag/span.h"
#include "fmt/format.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "i18n/messages.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/immutable.h"
#include "ir/instruction_flags.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/type_util.h"
#include "lowering/diag_code.h"
#include "lowering/drop_elaboration.h"
#include "lowering/immediate.h"
#include "lowering/lowerer.h"
#include "lowering/niche.h"
#include "source/source.h"
#include "text/unescape.h"

namespace {

// The width a shift count is taken modulo.
u32 integer_bits(ir::TypeTag tag) {
  switch (tag) {
    case ir::TypeTag::I8:
    case ir::TypeTag::U8: return 8;
    case ir::TypeTag::I16:
    case ir::TypeTag::U16: return 16;
    case ir::TypeTag::I32:
    case ir::TypeTag::U32: return 32;
    default: return 64;
  }
}

}  // namespace

namespace lowering {

void Lowerer::unsupported(diag::Span span, std::string_view what) {
  const u32 index = bag.emit<i18n::Key::LowerUnsupportedConstruct>(
      DiagCode::Unsupported, span, what);
  (void)index;
  failed = true;
}

void Lowerer::internal(diag::Span span, std::string_view what) {
  const u32 index =
      bag.emit<i18n::Key::LowerInternalError>(DiagCode::Internal, span, what);
  (void)index;
  failed = true;
}

void Lowerer::comp_overflow(diag::Span span) {
  const u32 index =
      bag.emit<i18n::Key::LowerCompOverflow>(DiagCode::CompOverflow, span);
  (void)index;
  failed = true;
}

void Lowerer::comp_div_zero(diag::Span span) {
  const u32 index =
      bag.emit<i18n::Key::LowerCompDivisionByZero>(DiagCode::CompDivZero, span);
  (void)index;
  failed = true;
}

void Lowerer::comp_oob(diag::Span span) {
  const u32 index = bag.emit<i18n::Key::LowerCompOutOfBounds>(
      DiagCode::CompOutOfBounds, span);
  (void)index;
  failed = true;
}

void Lowerer::comp_budget(diag::Span span) {
  const u32 index =
      bag.emit<i18n::Key::LowerCompBudget>(DiagCode::CompBudget, span);
  (void)index;
  failed = true;
}

ir::TypeIdx Lowerer::error_type() {
  return builder.error_type();
}

Val Lowerer::stopped_value() {
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  return Val{size_one, builder.never_type(), false, false};
}

bool Lowerer::is_copy(ir::TypeIdx type) const {
  return ir::is_copy_type(builder.state(), type, pkg.needs_drop,
                          pkg.not_movable);
}

ir::TypeTag Lowerer::tag_of(ir::TypeIdx idx) const {
  return builder.state().types[idx].tag;
}

bool Lowerer::is_ref_tag(ir::TypeTag tag) const {
  return tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef ||
         tag == ir::TypeTag::Ptr;
}

ir::TypeTag Lowerer::literal_tag(ast::LiteralIdx value,
                                 const ir::TypeIdx* expected) {
  const ast::Literal& lit = ast.literals[value];
  if (lit.kind == ast::LiteralKind::Bool) {
    return ir::TypeTag::I1;
  }
  if (lit.kind == ast::LiteralKind::String) {
    return ir::TypeTag::Str;
  }
  const bool is_float = lit.kind == ast::LiteralKind::Float;
  if (expected != nullptr) {
    const ir::TypeTag tag = tag_of(*expected);
    const bool matches =
        is_float ? (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64)
                 : (tag == ir::TypeTag::I8 || tag == ir::TypeTag::I16 ||
                    tag == ir::TypeTag::I32 || tag == ir::TypeTag::I64 ||
                    tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
                    tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64);
    if (matches) {
      return tag;
    }
  }
  struct SuffixTag {
    std::string_view suffix;
    ir::TypeTag tag;
  };
  constexpr SuffixTag SUFFIXES[] = {
      {"f32", ir::TypeTag::F32},   {"f64", ir::TypeTag::F64},
      {"isize", ir::TypeTag::I64}, {"usize", ir::TypeTag::U64},
      {"i8", ir::TypeTag::I8},     {"i16", ir::TypeTag::I16},
      {"i32", ir::TypeTag::I32},   {"i64", ir::TypeTag::I64},
      {"u8", ir::TypeTag::U8},     {"u16", ir::TypeTag::U16},
      {"u32", ir::TypeTag::U32},   {"u64", ir::TypeTag::U64},
  };
  for (const SuffixTag& entry : SUFFIXES) {
    if (lit.spelling.size() > entry.suffix.size() &&
        lit.spelling.substr(lit.spelling.size() - entry.suffix.size()) ==
            entry.suffix) {
      if (entry.suffix == "isize") {
        return width == ir::PointerWidth::W64 ? ir::TypeTag::I64
                                              : ir::TypeTag::I32;
      }
      if (entry.suffix == "usize") {
        return width == ir::PointerWidth::W64 ? ir::TypeTag::U64
                                              : ir::TypeTag::U32;
      }
      return entry.tag;
    }
  }
  return is_float ? ir::TypeTag::F64 : ir::TypeTag::I32;
}

Val Lowerer::lower_literal(ast::LiteralIdx lit_idx,
                           const ir::TypeIdx* expected,
                           bool negate) {
  const ast::Literal& lit = ast.literals[lit_idx];
  // A `-` the source wrote in front of the literal; expression negation
  // arrives as its own node, so only a pattern or a folding caller sets
  // this.
  const bool negative = lit.is_negative != negate;
  const ir::TypeTag tag = literal_tag(lit_idx, expected);
  const ir::TypeIdx type = builder.primitive(tag);
  if (tag == ir::TypeTag::Str) {
    const std::string bytes = text::unescape_string(lit.spelling);
    // A literal with no escape denotes the source's own bytes, and a source
    // lives as long as the table reads it; anything else is copied once.
    const std::string_view inner =
        lit.spelling.size() >= 2
            ? lit.spelling.substr(1, lit.spelling.size() - 2)
            : std::string_view{};
    const str::StringPoolId id =
        bytes == inner ? intern_name(inner) : intern_copied(bytes);
    if (id == str::INVALID_STRING_POOL_ID) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::ImmutableIdx imm =
        builder.immutable({.type = type, .data = {.str_id_value = id}});
    return Val{to_operand(imm, type), type, false, false};
  }
  if (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64) {
    std::string digits;
    for (char c : lit.spelling) {
      if (c == '_') {
        continue;
      }
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
          c == '+' || c == '-') {
        digits.push_back(c);
      } else {
        break;
      }
    }
    f64 value = digits.empty() ? 0.0 : std::strtod(digits.c_str(), nullptr);
    if (negative) {
      value = -value;
    }
    ir::Immutable imm{.type = type, .data = {}};
    if (tag == ir::TypeTag::F32) {
      imm.data.f32_value = static_cast<f32>(value);
    } else {
      imm.data.f64_value = static_cast<f64>(value);
    }
    const ir::ImmutableIdx idx = builder.immutable(imm);
    return Val{to_operand(idx, type), type, false, false};
  }
  u64 value = 0;
  if (tag == ir::TypeTag::I1) {
    value = lit.spelling == "true" ? 1 : 0;
  } else {
    value = comp::parse_numeric_value(lit.spelling);
    // Negation wraps, matching the two's-complement release semantics
    // the rest of the literal path already uses.
    if (negative) {
      value = 0 - value;
    }
  }
  return Val{imm_from_u64(tag, type, value), type, false, false};
}

ir::OperandIdx Lowerer::imm_from_u64(ir::TypeTag tag,
                                     ir::TypeIdx type,
                                     u64 value) {
  return lowering::imm_from_u64(builder, tag, type, value);
}

// Address of a place expression. Non-places diagnose: checking
// accepts any inner shape for borrows, lowering needs an origin.
Val Lowerer::place_addr(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  switch (node.kind) {
    case ast::ExprKind::Path: {
      const ast::ExprPath& path = node.payload.get<ast::ExprPath>();
      const std::span<const ast::Ident> segments = ast.paths[path.idx].segments;
      if (segments.size() == 1) {
        if (const Local* local = lookup_local(segments[0].name)) {
          return Val{to_operand(local->addr, local->type), local->type, true,
                     true};
        }
        // A `const` item names no storage of its own, so lending it a
        // reference materializes the value into a temporary and lends
        // that.
        if (const auto* info = lookup_static(module, segments[0].name)) {
          if (info->is_const && info->init.is_valid()) {
            Val value = lower_path(expr, nullptr);
            if (failed || terminated_cur()) {
              return value;
            }
            return address_of(value);
          }
        }
      }
      unsupported(node.span, "borrowed place");
      return Val{size_one, error_type(), true, false};
    }
    case ast::ExprKind::Field: {
      Val base = place_addr(node.payload.get<ast::ExprField>().receiver);
      if (failed) {
        return base;
      }
      return field_addr(base, node.payload.get<ast::ExprField>().name.name,
                        node.span);
    }
    case ast::ExprKind::Index: {
      const ast::ExprIndex& index = node.payload.get<ast::ExprIndex>();
      if (const analyzer::CheckedModule::CallTarget* target = call_target(expr);
          target != nullptr) {
        return lower_spec_index(expr, target, true);
      }
      Val base = place_addr(index.receiver);
      if (failed) {
        return base;
      }
      Val position = lower_expr(index.index, nullptr);
      if (failed) {
        return Val{size_one, error_type(), true, false};
      }
      return checked_index_addr(base, position, node.span);
    }
    case ast::ExprKind::Deref: {
      // A reference local is a place that holds the address; the place
      // `*p` names is the one that address points at. A raw pointer is
      // the address itself and names the same kind of place.
      const ast::ExprIdx inner_expr = node.payload.get<ast::ExprDeref>().inner;
      Val inner = materialize(lower_expr(inner_expr, nullptr));
      if (failed) {
        return Val{size_one, error_type(), true, false};
      }
      const ir::TypeTag tag = tag_of(inner.type);
      if (tag != ir::TypeTag::Ref && tag != ir::TypeTag::MutRef &&
          !ir::is_raw_ptr_type(tag)) {
        internal(node.span, "dereference of a non-reference");
        return Val{size_one, error_type(), true, false};
      }
      const ir::TypeIdx pointee =
          builder.ref_types()[builder.types()[inner.type.idx].as_ref()].pointee;
      // A shared or exclusive reference names a place its taker still
      // owns, unlike a raw pointer, which carries no owner. A reference
      // an unsafe call produced is that call's to name: the contract it
      // asks the caller to keep, not the borrow rule, governs reading
      // what it points at.
      bool borrowed = tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef;
      if (borrowed && unsafe_call_result(inner_expr)) {
        borrowed = false;
      }
      return Val{inner.op, pointee, true, true, borrowed};
    }
    default:
      // A temporary takes a slot in this frame, so a borrow of it names
      // storage: `a.eq(&2)` borrows a materialized operand, and an
      // owning value becomes an unnamed local the drop walk ends at the
      // enclosing block, so the borrow is read and retired once and no
      // reference outlives it.
      {
        Val value = lower_expr(expr, nullptr);
        if (failed || terminated_cur()) {
          return Val{size_one, error_type(), true, false};
        }
        const ir::TypeIdx type = value.type;
        if (tag_of(type) == ir::TypeTag::Error) {
          unsupported(node.span, "borrowed temporary");
          return Val{size_one, error_type(), true, false};
        }
        Val place = address_of(value);
        if (failed) {
          return Val{size_one, error_type(), true, false};
        }
        if (runs_destructor(type)) {
          const ir::Operand& operand = builder.state().operands[place.op.idx];
          if (operand.is<ir::RegisterIdx>()) {
            locals.emplace_back(std::string_view(), operand.as_register(),
                                type);
            record_event(OwnershipEvent::Kind::Bind,
                         static_cast<u32>(locals.size() - 1), {}, node.span);
          }
        }
        return Val{place.op, type, true, true, false};
      }
  }
}

// Whether `expr` is a call to an intrinsic the compiler owns whose
// signature is `unsafe`: only those hand out a reference to storage
// the caller owns, which is what lets the allocator's accessors be
// read as a move. An ordinary `unsafe fn` returns a borrow like any
// other, and ADR-0050 keeps the move rule for it.
bool Lowerer::unsafe_call_result(ast::ExprIdx expr) const {
  // An `unsafe { ... }` block is a spelling of the same call, so the
  // rule sees through it.
  while (ast.exprs[expr].kind == ast::ExprKind::Block) {
    const ast::ExprNode& block_node = ast.exprs[expr];
    if (!block_node.payload.get<ast::ExprBlock>().is_unsafe) {
      return false;
    }
    const ast::Block& block =
        ast.blocks[block_node.payload.get<ast::ExprBlock>().block];
    if (!block.value.is_valid()) {
      return false;
    }
    expr = block.value;
  }
  const ast::ExprNode& node = ast.exprs[expr];
  if (node.kind != ast::ExprKind::Call) {
    return false;
  }
  const ast::ExprIdx callee = node.payload.get<ast::ExprCall>().callee;
  const analyzer::CheckedModule::CallTarget* target = call_target(callee);
  if (target == nullptr || target->is_method) {
    return false;
  }
  const analyzer::CheckedModule::FnSig& sig =
      pkg.modules[target->module].functions[target->index];
  return sig.is_unsafe && ast.items[sig.item].kind == ast::ItemKind::Intrinsic;
}

// Whether `expr` names storage rather than a temporary: a local or a
// projection through one. Reference receivers that name storage are
// borrowed in place, so a method on a field updates the field instead
// of a copy.
bool Lowerer::is_rooted_place(ast::ExprIdx expr) const {
  const ast::ExprNode& node = ast.exprs[expr];
  switch (node.kind) {
    case ast::ExprKind::Path: {
      const ast::ExprPath& path = node.payload.get<ast::ExprPath>();
      const std::span<const ast::Ident> segments = ast.paths[path.idx].segments;
      return segments.size() == 1 && lookup_local(segments[0].name) != nullptr;
    }
    case ast::ExprKind::Field:
      return is_rooted_place(node.payload.get<ast::ExprField>().receiver);
    case ast::ExprKind::Index:
      return is_rooted_place(node.payload.get<ast::ExprIndex>().receiver);
    case ast::ExprKind::Deref:
      // The pointer may come from any expression; the storage it names
      // outlives the call.
      return true;
    default: return false;
  }
}

// Bounds-checked address of base[position]: panics out of bounds.
// The base must be a direct array address; indexing through a
// reference cannot project through codegen's alloca tracking.
Val Lowerer::checked_index_addr(Val base, Val position, diag::Span span) {
  if (base.address && is_slice_ref(base.type)) {
    // Indexing a slice: materialize the fat struct, project its
    // fields, check the runtime length, and offset the buffer. The
    // borrow checker anchors the projection to the loans the value
    // carries and extends the element, so the place stays tied to
    // whatever the view was taken from.
    const Val material = materialize(base);
    ir::OperandIdx bytes = ir::OperandIdx::invalid();
    ir::OperandIdx len = ir::OperandIdx::invalid();
    if (!slice_parts(material, bytes, len)) {
      return Val{size_one, error_type(), true, false};
    }
    const ir::TypeIdx usize_ty = usize_type();
    Val wide = position;
    if (tag_of(position.type) != tag_of(usize_ty)) {
      const ir::RegisterIdx casted =
          emit(ir::Opcode::TypeCast, usize_ty, {use_value(position)});
      if (failed) {
        return Val{size_one, error_type(), true, false};
      }
      wide = Val{to_operand(casted, usize_ty), usize_ty, false, false};
    }
    const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
    const ir::RegisterIdx in_bounds =
        emit(ir::Opcode::Lt, boolean, {use_value(wide), len});
    const ir::BlockIdx ok_block = reserve_block();
    const ir::BlockIdx bad_block = reserve_block();
    emit_cond_br(to_operand(in_bounds, boolean), ok_block, bad_block);
    switch_to(bad_block);
    emit_panic(span, str_operand("index out of bounds"));
    switch_to(ok_block);
    if (failed) {
      return Val{size_one, error_type(), true, false};
    }
    const ir::SliceType& shape =
        builder.state().slice_types
            [builder.state().types[slice_pointee(base.type)].as_slice()];
    // ElemOffset names an element loan: its destination carries the
    // element reference, so the borrow checker extends the buffer's
    // place with an element step instead of covering the whole
    // buffer. Exclusive exactly when the slice itself is.
    const bool exclusive = tag_of(base.type) == ir::TypeTag::MutRef;
    const ir::TypeIdx elem_ref =
        builder.reference_type(shape.element, exclusive);
    const ir::RegisterIdx elem =
        emit(ir::Opcode::ElemOffset, elem_ref, {bytes, use_value(wide)},
             {.inbounds = true});
    if (failed) {
      return Val{size_one, error_type(), true, false};
    }
    // The elements belong to whatever owns the buffer the view reads.
    return Val{to_operand(elem, shape.element), shape.element, true, base.place,
               true};
  }
  if (!base.address || tag_of(base.type) != ir::TypeTag::Array) {
    unsupported(span, "index through reference");
    return Val{size_one, error_type(), true, false};
  }
  const ir::ArrayType& shape =
      builder.state().array_types[builder.state().types[base.type].as_array()];
  // A literal index the array's own length admits needs no check: the
  // address is in bounds by arithmetic, not by test.
  const ir::Operand& position_operand = builder.state().operands[position.op];
  if (position_operand.is<ir::ImmutableIdx>()) {
    const u64 value = builder.state()
                          .immutables[position_operand.as_immutable()]
                          .as_u64_integer(tag_of(position.type));
    if (value < shape.count) {
      const ir::RegisterIdx gep =
          emit(ir::Opcode::GetElementPtr, shape.element,
               {base.op, zero_i32, position.op}, {.inbounds = true});
      return Val{to_operand(gep, shape.element), shape.element, true,
                 base.place, base.borrowed};
    }
  }
  const ir::TypeIdx usize_ty = usize_type();
  Val wide = position;
  if (tag_of(position.type) != tag_of(usize_ty)) {
    const ir::RegisterIdx casted =
        emit(ir::Opcode::TypeCast, usize_ty, {use_value(position)});
    if (failed) {
      return Val{size_one, error_type(), true, false};
    }
    wide = Val{to_operand(casted, usize_ty), usize_ty, false, false};
  }
  const ir::TypeIdx count_ty = usize_ty;
  ir::Immutable imm{.type = count_ty, .data = {}};
  if (tag_of(count_ty) == ir::TypeTag::U64) {
    imm.data.u64_value = shape.count;
  } else {
    imm.data.u32_value = static_cast<u32>(shape.count);
  }
  const ir::OperandIdx count = to_operand(builder.immutable(imm), count_ty);
  const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
  const ir::RegisterIdx in_bounds =
      emit(ir::Opcode::Lt, boolean, {use_value(wide), count});
  const ir::BlockIdx ok_block = reserve_block();
  const ir::BlockIdx bad_block = reserve_block();
  emit_cond_br(to_operand(in_bounds, boolean), ok_block, bad_block);
  switch_to(bad_block);
  emit_panic(span, str_operand("index out of bounds"));
  switch_to(ok_block);
  if (failed) {
    return Val{size_one, error_type(), true, false};
  }
  const ir::RegisterIdx gep =
      emit(ir::Opcode::GetElementPtr, shape.element,
           {base.op, zero_i32, use_value(wide)}, {.inbounds = true});
  return Val{to_operand(gep, shape.element), shape.element, true, base.place,
             base.borrowed};
}

ir::OperandIdx Lowerer::index_operand(u32 index) {
  const ir::TypeIdx i32_ty = builder.primitive(ir::TypeTag::I32);
  ir::Immutable imm{.type = i32_ty, .data = {}};
  imm.data.i32_value = static_cast<i32>(index);
  return to_operand(builder.immutable(imm), i32_ty);
}

bool Lowerer::struct_field_index(ir::TypeIdx type,
                                 std::string_view name,
                                 u32& index_out) {
  const ir::TypeIdx origin = type_origin(type);
  // Where a structure's shape is recorded, by the type it was recorded for.
  // A field access asked by walking every structure in the package, so one
  // access cost the package.
  const auto declaring = structs_.find(origin.idx);
  if (declaring == structs_.end()) {
    return false;
  }
  const auto& fields = declaring->second->fields;
  for (u32 i = 0; i < static_cast<u32>(fields.size()); ++i) {
    if (fields[i] == name) {
      index_out = i;
      return true;
    }
  }
  return false;
}

ir::TypeIdx Lowerer::field_type_of(ir::TypeIdx base,
                                   u32 index,
                                   diag::Span span) {
  const ir::TypeTag tag = tag_of(base);
  if (tag == ir::TypeTag::Struct) {
    const ir::StructType& struct_type =
        builder.state().struct_types[builder.state().types[base].as_struct()];
    if (index < struct_type.fields.size()) {
      return struct_type.fields[index];
    }
  } else if (tag == ir::TypeTag::Tuple) {
    const ir::TupleType& tuple_type =
        builder.state().tuple_types[builder.state().types[base].as_tuple()];
    if (index < tuple_type.elements.size()) {
      return tuple_type.elements[index];
    }
  } else if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
    const ir::TypeIdx pointee =
        builder.state().ref_types[builder.state().types[base].as_ref()].pointee;
    return field_type_of(pointee, index, span);
  }
  internal(span, "field type without declaration");
  return error_type();
}

void Lowerer::bind_pattern(ast::PatternIdx pattern, Val init) {
  if (nesting_.exhausted()) {
    report_nesting(ast.patterns[pattern].span);
    failed = true;
    return;
  }
  const base::NestingScope scope(nesting_);
  const ast::PatternNode& node = ast.patterns[pattern];
  switch (node.kind) {
    case ast::PatternKind::Wildcard:
      // A wildcard binding of a droppable place discards it by ending
      // it, so a destructured field nothing binds still runs its
      // destructor. A wildcard parameter keeps the value for the
      // function's own scope exit.
      if (!binding_param_ && init.address && runs_destructor(init.type)) {
        record_drop_in_place(init.op, init.type, node.span);
      }
      break;
    case ast::PatternKind::Ident:
    case ast::PatternKind::MutIdent: {
      std::string_view name;
      if (node.kind == ast::PatternKind::Ident) {
        name = node.payload.ident.name.name;
      } else {
        name = node.payload.mut_ident.name.name;
      }
      if (tag_of(init.type) == ir::TypeTag::Void) {
        locals.emplace_back(name, ir::RegisterIdx(base::INVALID_IDX),
                            init.type);
        return;
      }
      // A guard sees the binding, but a by-value binding of a non-`Copy`
      // value would already have taken it when the guard runs, and a
      // false guard falls through to the next arm with the value gone.
      // The move is refused; a `match` on a reference binds by reference
      // and carries no such hazard.
      if (guard_arm_ && !is_copy(init.type)) {
        const u32 index = bag.emit<i18n::Key::LowerGuardMovesBinding>(
            DiagCode::GuardMovesBinding, node.span, name);
        (void)index;
        failed = true;
        return;
      }
      const Val material = materialize(init);
      const ir::RegisterIdx addr =
          emit(ir::Opcode::Alloca, init.type, {size_one});
      emit_void(ir::Opcode::Store, {material.op, to_operand(addr, init.type)});
      locals.emplace_back(name, addr, init.type);
      record_event(OwnershipEvent::Kind::Bind,
                   static_cast<u32>(locals.size() - 1), {}, node.span);
      addr_names_.push_back({addr, name, binding_param_});
      return;
    }
    case ast::PatternKind::Tuple: {
      if (node.payload.tuple.path.is_valid()) {
        unsupported(node.span, "variant pattern in lowering");
        return;
      }
      Val base = address_of(init);
      if (failed) {
        return;
      }
      for (u32 i = 0; i < static_cast<u32>(node.payload.tuple.elements.size());
           ++i) {
        const ir::TypeIdx element_type = field_type_of(base.type, i, node.span);
        const ir::RegisterIdx gep =
            emit(ir::Opcode::GetElementPtr, element_type,
                 {base.op, zero_i32, index_operand(i)}, {.inbounds = true});
        Val element{to_operand(gep, element_type), element_type, true,
                    init.place};
        bind_pattern(node.payload.tuple.elements[i], element);
        if (failed) {
          return;
        }
      }
      return;
    }
    case ast::PatternKind::Struct: {
      Val base = address_of(init);
      if (failed) {
        return;
      }
      for (const ast::FieldPattern& field : node.payload.strukt.fields) {
        u32 index = 0;
        if (!struct_field_index(base.type, field.name.name, index)) {
          internal(field.name.span, "pattern field without declaration");
          return;
        }
        const ir::TypeIdx element_type =
            field_type_of(base.type, index, node.span);
        const ir::RegisterIdx gep =
            emit(ir::Opcode::GetElementPtr, element_type,
                 {base.op, zero_i32, index_operand(index)}, {.inbounds = true});
        bind_pattern(field.pattern, Val{to_operand(gep, element_type),
                                        element_type, true, init.place});
        if (failed) {
          return;
        }
      }
      return;
    }
    case ast::PatternKind::Ref: {
      // Through a reference pattern the inner name observes the
      // pointer slot itself.
      const Val material = materialize(init);
      const ir::RegisterIdx addr =
          emit(ir::Opcode::Alloca, init.type, {size_one});
      emit_void(ir::Opcode::Store, {material.op, to_operand(addr, init.type)});
      bind_pattern(node.payload.ref.inner,
                   Val{to_operand(addr, init.type), init.type, true, false});
      return;
    }
    case ast::PatternKind::Literal:
      unsupported(node.span, "refutable pattern in lowering");
      return;
    case ast::PatternKind::Or:
      // Nested alternatives need the distributive expansion
      // `(A | B, x) => (A, x) | (B, x)`, which desugaring does not do.
      // Rejecting here is honest; the top-level form is handled by
      // condition_alternatives and expand_or_arms.
      unsupported(node.span, "or-pattern nested inside another pattern");
      return;
  }
}

Val Lowerer::lower_path(ast::ExprIdx expr, const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::PathIdx path = node.payload.get<ast::ExprPath>().idx;
  const std::span<const ast::Ident> segments = ast.paths[path].segments;
  if (segments.size() == 1) {
    const std::string_view name = segments[0].name;
    // A value-parameter use is a compile-time constant: emit the value
    // its use was checked under, with the annotation's integer type.
    if (const auto* splice = value_splice(expr)) {
      return Val{
          imm_from_u64(tag_of(splice->type), splice->type, splice->value),
          splice->type, false, false};
    }
    if (const Local* local = lookup_local(name)) {
      if (tag_of(local->type) == ir::TypeTag::Void) {
        return Val{size_one, local->type, false, false};
      }
      return Val{to_operand(local->addr, local->type), local->type, true, true};
    }
    // Comp bindings never take runtime addresses; splice the value.
    for (usize i = comp_scope_.size(); i-- > 0;) {
      if (comp_scope_[i].first == name) {
        return comp_lowering_.materialize(comp_scope_[i].second, node.span);
      }
    }
    if (const auto* info = lookup_static(module, name)) {
      if (info->is_const && info->init.is_valid()) {
        // The declared type wins over the context: a `const` read is
        // always the type it was declared with.
        const ir::TypeIdx want =
            info->type.is_valid()
                ? info->type
                : (expected != nullptr ? *expected : error_type());
        if (ast.exprs[info->init].kind == ast::ExprKind::Literal) {
          return lower_literal(
              ast.exprs[info->init].payload.get<ast::ExprLiteral>().value,
              info->type.is_valid() ? &want : expected);
        }
        // Anything else is a compile-time value: evaluate the
        // initializer and splice it.
        CompVal value;
        if (!comp_lowering_.evaluate_item(module, info->init, value)) {
          return Val{size_one, error_type(), false, false};
        }
        return comp_lowering_.materialize(value, node.span);
      }
      unsupported(node.span, "static item in lowering");
      return Val{size_one, error_type(), false, false};
    }
  }
  // A named function coerced to a closure value packs its code
  // with a null environment, like any closure literal.
  if (closure_fn(expr) != nullptr) {
    return lower_closure(expr);
  }
  if (const auto* use = variant_use(path)) {
    // Unit values stand alone; payload constructors need call syntax
    // (checking enforced this).
    const std::vector<ir::TypeIdx> payloads =
        variant_payload(use->enum_type, use->variant);
    if (!payloads.empty()) {
      internal(node.span, "variant without call");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx slot = enum_slot_type(use->enum_type);
    const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, slot, {size_one});
    // A fieldless variant is the payload's spare pattern, so the slot
    // holds the null reference and there is no discriminant to write
    // (ADR-0087).
    const ir::EnumNiche niche = ir::enum_niche(builder.state(), use->enum_type);
    if (niche.has) {
      emit_void(ir::Opcode::Store,
                {niche_sentinel(niche), to_operand(addr, slot)});
      return Val{to_operand(addr, slot), use->enum_type, true, false};
    }
    const ir::RegisterIdx tag =
        emit(ir::Opcode::GetElementPtr, builder.primitive(ir::TypeTag::I32),
             {to_operand(addr, slot), zero_i32, index_operand(0)});
    emit_void(ir::Opcode::Store,
              {disc_operand(use->variant), to_operand(tag, slot)});
    return Val{to_operand(addr, slot), use->enum_type, true, false};
  }
  internal(node.span, "path without lowering");
  return Val{size_one, error_type(), false, false};
}

ir::ExternalFunctionIdx Lowerer::declare_external(
    std::string_view name,
    ir::TypeIdx ret,
    const std::vector<ir::TypeIdx>& params) {
  for (const ExtEntry& entry : exts) {
    if (entry.name == name) {
      return entry.idx;
    }
  }
  ir::TypeSeq seq;
  for (ir::TypeIdx param : params) {
    seq.push(storage_copy(param));
  }
  // A C entry point keeps its own name: it is not ours to encode.
  const str::StringPoolId interned = intern_name(name);
  if (interned == str::INVALID_STRING_POOL_ID) {
    return ir::ExternalFunctionIdx(base::INVALID_IDX);
  }
  const ir::ExternalFunctionIdx idx =
      builder.external_function({.meta = {.return_type = ret,
                                          .param_types = seq.finish(),
                                          .name = interned,
                                          .path = str::EMPTY_STRING_ID,
                                          .kind = ir::SymbolKind::Foreign,
                                          .generics = ir::TypeIdxRange{}},
                                 .calling_conv = ir::CallingConvention::C});
  exts.push_back({name, idx});
  return idx;
}

const analyzer::CheckedModule::VariantUse* Lowerer::variant_use(
    ast::PathIdx path) const {
  return variant_use_in(path, cur_inst_);
}

const analyzer::CheckedModule::VariantUse* Lowerer::variant_use_in(
    ast::PathIdx path,
    u32 inst) const {
  const auto found = variants_.find((static_cast<u64>(path.idx) << 32) |
                                    static_cast<u64>(inst));
  if (found != variants_.end()) {
    return found->second;
  }
  return nullptr;
}

const analyzer::CheckedModule::ValueSplice* Lowerer::value_splice(
    ast::ExprIdx expr) const {
  return value_splice_in(expr, cur_inst_);
}

const analyzer::CheckedModule::ValueSplice* Lowerer::value_splice_in(
    ast::ExprIdx expr,
    u32 inst) const {
  return pkg.modules[module].value_splice_of(expr, inst);
}

const analyzer::CheckedModule::EnumInfo* Lowerer::enum_info(
    ir::TypeIdx type) const {
  const auto found = enums_.find(type.idx);
  if (found != enums_.end()) {
    return found->second;
  }
  return nullptr;
}

// Variant index by trailing name against a known enum type, for
// patterns (checking validated the match).
bool Lowerer::variant_index(ir::TypeIdx enum_type,
                            std::string_view name,
                            u32& index_out) {
  const auto* info = enum_info(enum_type);
  if (info == nullptr) {
    return false;
  }
  for (u32 i = 0; i < static_cast<u32>(info->variants.size()); ++i) {
    if (info->variants[i] == name) {
      index_out = i;
      return true;
    }
  }
  return false;
}

std::vector<ir::TypeIdx> Lowerer::variant_payload(ir::TypeIdx enum_type,
                                                  u32 variant) {
  const ir::EnumType& enum_ty =
      builder.state().enum_types[builder.state().types[enum_type].as_enum()];
  std::vector<ir::TypeIdx> payloads;
  u32 at = enum_ty.variants.head().idx + variant;
  const ir::EnumVariantType& variant_ty =
      builder.state().enum_variant_types[ir::EnumVariantTypeIdx(at)];
  for (ir::TypeIdx field : variant_ty.fields) {
    payloads.push_back(field);
  }
  return payloads;
}

ir::TypeIdxRange Lowerer::variant_fields(ir::TypeIdx enum_type, u32 variant) {
  const ir::EnumType& enum_ty =
      builder.state().enum_types[builder.state().types[enum_type].as_enum()];
  const u32 at = enum_ty.variants.head().idx + variant;
  return builder.state().enum_variant_types[ir::EnumVariantTypeIdx(at)].fields;
}

// Slot for an enum value: an i32 discriminant followed by the payload
// area, appended back to back because a tuple's element types must be
// consecutive in the type table. The area is a byte array on a carrier
// of the published alignment, matching what the emitter builds.
ir::TypeIdx Lowerer::enum_slot_type(ir::TypeIdx enum_type) {
  const auto cached = enum_slot_types_.find(enum_type.idx);
  if (cached != enum_slot_types_.end()) {
    return cached->second;
  }
  // Where the discriminant fits in the payload's spare pattern, the
  // payload is the whole slot (ADR-0087).
  const ir::EnumNiche niche = ir::enum_niche(builder.state(), enum_type);
  if (niche.has) {
    enum_slot_types_.emplace(enum_type.idx, niche.slot);
    return niche.slot;
  }
  const ir::TypeLayout area =
      ir::enum_payload_area(builder.state(), enum_type, width);
  const ir::TypeIdx carrier = payload_carrier(area.align);
  const u64 carrier_bytes = area.align == 0 ? 1 : area.align;
  // Everything the two slot halves are copied from is interned first, so
  // the copies land next to each other in the type table: array_type
  // appends a node of its own, which would otherwise separate them.
  const ir::TypeIdx array =
      builder.array_type(carrier, area.size / carrier_bytes);
  const ir::TypeIdx disc = storage_copy(builder.primitive(ir::TypeTag::I32));
  const ir::TypeIdx bytes = storage_copy(array);
  ir::TypeSeq seq;
  seq.push(disc);
  seq.push(bytes);
  const ir::TypeIdx slot = builder.tuple_type(seq.finish());
  enum_slot_types_.emplace(enum_type.idx, slot);
  return slot;
}

// The narrowest primitive whose alignment is at least `align`, so the
// area is aligned for the widest payload it holds.
ir::TypeIdx Lowerer::payload_carrier(u64 align) {
  switch (align) {
    case 1: return builder.primitive(ir::TypeTag::U8);
    case 2: return builder.primitive(ir::TypeTag::U16);
    case 4: return builder.primitive(ir::TypeTag::U32);
    case 8: return builder.primitive(ir::TypeTag::U64);
    default: return builder.array_type(builder.primitive(ir::TypeTag::U32), 4);
  }
}

Val Lowerer::lower_variant_construct(
    ast::ExprIdx expr,
    const analyzer::CheckedModule::VariantUse* use) {
  const ast::ExprNode& call = ast.exprs[expr];
  const std::vector<ir::TypeIdx> payloads =
      variant_payload(use->enum_type, use->variant);
  if (call.payload.get<ast::ExprCall>().args.size() != payloads.size()) {
    internal(call.span, "variant arity");
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx slot = enum_slot_type(use->enum_type);
  const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, slot, {size_one});
  const ir::EnumNiche niche = ir::enum_niche(builder.state(), use->enum_type);
  if (!niche.has) {
    const ir::RegisterIdx tag =
        emit(ir::Opcode::GetElementPtr, builder.primitive(ir::TypeTag::I32),
             {to_operand(addr, slot), zero_i32, index_operand(0)});
    emit_void(ir::Opcode::Store,
              {disc_operand(use->variant), to_operand(tag, slot)});
  }
  if (is_unit_payload(payloads)) {
    // Lower for effects; nothing is stored.
    for (usize i = 0; i < payloads.size(); ++i) {
      Val value =
          lower_moved(call.payload.get<ast::ExprCall>().args[i], &payloads[i]);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      mark_move(value);
    }
  } else if (!payloads.empty()) {
    // The payload lives in the slot, so a value built here outlives this
    // frame and can be returned. Field addresses come from the offsets
    // ir::field_offset published for this variant.
    const ir::TypeIdxRange fields =
        variant_fields(use->enum_type, use->variant);
    for (usize i = 0; i < payloads.size(); ++i) {
      Val value =
          lower_moved(call.payload.get<ast::ExprCall>().args[i], &payloads[i]);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      // A niche-carrying enum's payload is the slot itself.
      const ir::RegisterIdx field =
          niche.has ? addr
                    : payload_field_addr(
                          Val{to_operand(addr, slot), slot, true, false},
                          use->enum_type, fields, static_cast<u32>(i));
      const ir::OperandIdx stored =
          niche.has && niche.slot != niche.payload
              ? to_operand(
                    emit(ir::Opcode::TypeCast, niche.slot, {use_value(value)}),
                    niche.slot)
              : use_value(value);
      emit_void(ir::Opcode::Store, {stored, to_operand(field, payloads[i])});
    }
  }
  return Val{to_operand(addr, slot), use->enum_type, true, false};
}

// Address of payload field `field` in the slot at `slot_addr`: the
// slot's payload area, then the field's byte offset. Two projections
// because the area is a byte array, so a second index would scale by
// the carrier instead of counting bytes.
ir::RegisterIdx Lowerer::payload_field_addr(Val slot_addr,
                                            ir::TypeIdx enum_type,
                                            ir::TypeIdxRange fields,
                                            u32 field) {
  // A niche-carrying enum's payload is the slot, so the field address
  // is the slot's own address (ADR-0087). Readers relabel it with the
  // payload type; the register already carries that label.
  if (ir::enum_niche(builder.state(), enum_type).has) {
    const ir::Operand& op = builder.state().operands[slot_addr.op.idx];
    if (op.is<ir::RegisterIdx>()) {
      return op.as_register();
    }
    return emit(ir::Opcode::TypeCast, slot_addr.type, {slot_addr.op});
  }
  const ir::TypeIdx byte = builder.primitive(ir::TypeTag::U8);
  const ir::RegisterIdx area = emit(ir::Opcode::GetElementPtr, byte,
                                    {slot_addr.op, zero_i32, index_operand(1)});
  return emit(
      ir::Opcode::GetElementPtr, byte,
      {to_operand(area, byte), index_operand(static_cast<u32>(ir::field_offset(
                                   builder.state(), fields, field, width)))});
}

ir::RegisterIdx Lowerer::payload_field_slot_addr(Val slot_addr,
                                                 ir::TypeIdx enum_type,
                                                 ir::TypeIdxRange fields,
                                                 u32 field) {
  // The payload of a niche-carrying enum is the slot, and the slot's
  // register already carries the payload's type: the reference is that
  // address relabelled (ADR-0087).
  if (ir::enum_niche(builder.state(), enum_type).has) {
    const ir::TypeIdx ptr = builder.reference_type(fields[field], false);
    return emit(ir::Opcode::TypeCast, ptr, {slot_addr.op});
  }
  const ir::TypeIdx byte = builder.primitive(ir::TypeTag::U8);
  const ir::RegisterIdx at =
      payload_field_addr(slot_addr, enum_type, fields, field);
  const ir::TypeIdx ptr = builder.reference_type(fields[field], false);
  return emit(ir::Opcode::TypeCast, ptr, {to_operand(at, byte)});
}

ir::OperandIdx Lowerer::disc_operand(u32 discriminant) {
  const ir::TypeIdx i32_ty = builder.primitive(ir::TypeTag::I32);
  ir::Immutable imm{.type = i32_ty, .data = {}};
  imm.data.i32_value = static_cast<i32>(discriminant);
  return to_operand(builder.immutable(imm), i32_ty);
}

Val Lowerer::lower_closure(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ir::TypeIdx type = expr_type(expr);
  // A literal's own type is the nominal whose one field is its
  // environment; a context that wants a bare function type gets the
  // view, which borrows the value. A coerced named function has no
  // environment at all (ADR-0064).
  const bool as_view = tag_of(type) == ir::TypeTag::Func;
  // A closure literal has a checked record with its captures; a
  // coerced named function has none, because it captures nothing.
  const analyzer::CheckedModule::ClosureLit* lit = closure_lit(expr);
  if (lit == nullptr && closure_fn(expr) == nullptr) {
    internal(node.span, "closure without checking");
    return Val{size_one, error_type(), false, false};
  }
  // Both forms share one signature: the literal's recorded function
  // type, or the function type a named function coerces from.
  const ir::TypeIdx sig_type = lit == nullptr ? type : lit->func_type;
  if (!sig_type.is_valid() || tag_of(sig_type) != ir::TypeTag::Func) {
    internal(node.span, "closure without type");
    return Val{size_one, error_type(), false, false};
  }
  // Copied out before emitting anything: interning below may move
  // the tables the shape borrows.
  std::vector<ir::TypeIdx> params;
  const ir::FuncType& shape =
      builder.state().func_types[builder.state().types[sig_type].as_func()];
  params.reserve(shape.params.size());
  for (ir::TypeIdx param : shape.params) {
    params.push_back(param);
  }
  const ir::TypeIdx ret = shape.ret;
  // The environment the synthetic function receives is a reference
  // to the tuple the creation site builds; a closure without
  // captures receives none.
  using Capture = analyzer::CheckedModule::ClosureLit::Capture;
  const std::vector<Capture> no_captures;
  const std::vector<Capture>& captures =
      lit == nullptr ? no_captures : lit->captures;
  const ir::TypeIdx env_ty = closure_env_type(captures);
  const ir::TypeIdx env_ref_ty = captures.empty()
                                     ? ir::TypeIdx::invalid()
                                     : builder.reference_type(env_ty, false);
  const ir::FunctionIdx fn =
      closure_fn_index(module, expr, params, ret, cur_inst_, env_ref_ty);
  if (!fn.is_valid()) {
    return Val{size_one, error_type(), false, false};
  }
  // The environment both forms lay out: one field per capture, a
  // reference or a copied value.
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  const ir::TypeIdx code_ty = builder.primitive(ir::TypeTag::Function);
  // Ends one capture into the environment tuple that starts at
  // `env_at`: a move hands the value over, a borrow names the captured
  // local.
  auto store_captures = [&](ir::OperandIdx env_at) -> bool {
    for (usize i = 0; i < captures.size() && !failed; ++i) {
      const Capture& capture = captures[i];
      const Local* local = lookup_local(capture.name);
      if (local == nullptr) {
        internal(node.span, "capture without local");
        return false;
      }
      const ir::TypeIdx field_ty =
          capture.mode == ast::CaptureMode::Move
              ? capture.type
              : builder.reference_type(capture.type,
                                       capture.mode == ast::CaptureMode::Mut);
      const ir::RegisterIdx field = emit(ir::Opcode::GetElementPtr, field_ty,
                                         {env_at, zero_i32, index_operand(i)});
      ir::OperandIdx value(base::INVALID_IDX);
      if (capture.mode == ast::CaptureMode::Move) {
        // A Copy value: what the closure holds is its own.
        Val place{to_operand(local->addr, capture.type), capture.type, true,
                  true};
        value = use_value(place);
      } else {
        const ir::RegisterIdx loan =
            emit(ir::Opcode::Borrow, field_ty,
                 {to_operand(local->addr, capture.type)});
        value = to_operand(loan, field_ty);
      }
      emit_void(ir::Opcode::Store, {value, to_operand(field, field_ty)});
    }
    return !failed;
  };
  if (!as_view) {
    // The value owns its environment: one field, holding the tuple the
    // captures lay out, and ending the value ends them.
    const ir::TypeIdx self_type =
        lit == nullptr ? ir::TypeIdx::invalid() : lit->self_type;
    if (!self_type.is_valid()) {
      internal(node.span, "closure without a nominal type");
      return Val{size_one, error_type(), false, false};
    }
    const ir::RegisterIdx addr =
        emit(ir::Opcode::Alloca, self_type, {size_one});
    const ir::RegisterIdx env =
        emit(ir::Opcode::GetElementPtr, env_ty,
             {to_operand(addr, self_type), zero_i32, index_operand(0)});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    if (!captures.empty() && !store_captures(to_operand(env, env_ty))) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(addr, self_type), self_type, true, false};
  }
  // The view packs code with its environment, the way the function
  // type lays out. A closure with no captures keeps a null slot.
  const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, type, {size_one});
  const ir::RegisterIdx code =
      emit(ir::Opcode::GetElementPtr, code_ty,
           {to_operand(addr, type), zero_i32, index_operand(0)});
  emit_void(ir::Opcode::Store,
            {builder.operand(ir::Operand::from_function(fn, code_ty)),
             to_operand(code, code_ty)});
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx env =
      emit(ir::Opcode::GetElementPtr, ptr_ty,
           {to_operand(addr, type), zero_i32, index_operand(1)});
  if (!captures.empty()) {
    // The environment: the borrows and copies here are what the view
    // carries, and borrowing the slot itself is what keeps the closure
    // from outliving the frame. An environment that owns anything
    // becomes a named local, so the drop pass ends it with the scope
    // the closure was written in (ADR-0064).
    const ir::RegisterIdx slot = emit(ir::Opcode::Alloca, env_ty, {size_one});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    if (runs_destructor(env_ty)) {
      locals.emplace_back("$closure", slot, env_ty);
      // The captures are in the slot by now, so the place owns what
      // it holds; without the event the walk reads the slot as never
      // owned and the scope's end leaves the captures behind.
      record_event(OwnershipEvent::Kind::Bind,
                   static_cast<u32>(locals.size() - 1), {}, node.span);
    }
    if (!store_captures(to_operand(slot, env_ty))) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::RegisterIdx slot_ref =
        emit(ir::Opcode::Borrow, builder.reference_type(env_ty, false),
             {to_operand(slot, env_ty)});
    emit_void(ir::Opcode::Store,
              {to_operand(slot_ref, ptr_ty), to_operand(env, ptr_ty)});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(addr, type), type, true, false};
  }
  const ir::RegisterIdx null =
      emit(ir::Opcode::TypeCast, ptr_ty, {const_usize(0)});
  emit_void(ir::Opcode::Store,
            {to_operand(null, ptr_ty), to_operand(env, ptr_ty)});
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  return Val{to_operand(addr, type), type, true, false};
}

Val Lowerer::lower_indirect_call(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
  Val callee = lower_expr(call.callee, nullptr);
  if (failed || terminated_cur()) {
    return stopped_value();
  }
  // A closure value's implementation is the function the lowering
  // registered for its literal (ADR-0116). A view would carry the
  // value's own address as its environment; the direct call hands that
  // address over as the first argument instead, so a call on a closure
  // nominal has no view and no indirect call.
  const ClosureEntry* closure = tag_of(callee.type) == ir::TypeTag::Func
                                    ? nullptr
                                    : closure_entry(callee.type);
  const bool direct = closure_has_code(closure);
  if (!direct && tag_of(callee.type) != ir::TypeTag::Func) {
    // A closure value calls through the view its nominal makes.
    callee = closure_view(callee);
    if (failed || tag_of(callee.type) == ir::TypeTag::Error) {
      return Val{size_one, error_type(), false, false};
    }
  }
  const ir::TypeIdx shape_type = direct ? closure->func : callee.type;
  std::vector<ir::TypeIdx> params;
  const ir::FuncType& shape =
      builder.state().func_types[builder.state().types[shape_type].as_func()];
  params.reserve(shape.params.size());
  for (ir::TypeIdx param : shape.params) {
    params.push_back(param);
  }
  const ir::TypeIdx ret = shape.ret;
  if (call.args.size() != params.size()) {
    internal(node.span, "call arity");
    return Val{size_one, error_type(), false, false};
  }
  std::vector<ir::OperandIdx> ops;
  if (direct) {
    // The environment the registered function takes first: the value's
    // own address, the pointer a view would carry. An environment
    // reference goes through the argument path so the borrow checker
    // records the loan a call takes on the closure - and refuses a
    // closure a move already gave away; a captureless closure keeps
    // the opaque pointer, which reads nothing.
    const Val place = callee.address ? callee : address_of(callee);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const ir::TypeIdx env_param = fns[closure->fn.idx].params[0];
    const ir::TypeTag env_tag = tag_of(env_param);
    ops.push_back(builder.operand(ir::Operand::from_function(
        closure->fn, builder.primitive(ir::TypeTag::Function))));
    if (env_tag == ir::TypeTag::Ref || env_tag == ir::TypeTag::MutRef) {
      ops.push_back(arg_for(place, env_param));
    } else {
      // A captureless closure's environment is the opaque pointer,
      // which reads nothing: the value's address goes as it is.
      ops.push_back(place.op);
    }
  } else {
    ops.push_back(use_value(callee));
  }
  for (usize i = 0; i < call.args.size(); ++i) {
    Val arg = lower_moved(call.args[i], &params[i]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    ops.push_back(arg_for(arg, params[i]));
  }
  if (tag_of(ret) == ir::TypeTag::Never) {
    emit_void(ir::Opcode::Call, ops);
    emit_void(ir::Opcode::Unreachable, {});
    return Val{size_one, ret, false, false};
  }
  if (tag_of(ret) == ir::TypeTag::Void) {
    emit_void(ir::Opcode::Call, ops);
    return Val{size_one, ret, false, false};
  }
  const ir::RegisterIdx dst = emit(ir::Opcode::Call, ret, ops);
  return Val{to_operand(dst, ret), ret, false, false};
}

Val Lowerer::lower_call(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
  // Intrinsics by name (checking rejected shadowing definitions).
  if (ast.exprs[call.callee].kind == ast::ExprKind::Path) {
    const ast::ExprPath& path =
        ast.exprs[call.callee].payload.get<ast::ExprPath>();
    const ast::PathIdx path_idx = path.idx;
    if (ast.paths[path_idx].segments.size() == 1) {
      const std::string_view name = ast.paths[path_idx].segments[0].name;
      if (name == "panic" && lookup_local(name) == nullptr &&
          lookup_static(module, name) == nullptr) {
        bool shadowed = false;
        for (const auto& checked : pkg.modules) {
          for (const auto& fn : checked.functions) {
            if (fn.name == name) {
              shadowed = true;
              break;
            }
          }
          if (shadowed) {
            break;
          }
        }
        if (!shadowed) {
          return lower_panic(expr);
        }
      }
    }
  }
  const analyzer::CheckedModule::CallTarget* target = call_target(call.callee);
  if (target == nullptr) {
    // A call through a function value carries the value as its
    // callee; the signature comes from the value's type.
    if (indirect_call(call.callee) != nullptr) {
      return lower_indirect_call(expr);
    }
    // Checking records free, associated, and method callees; the
    // remainder is variant construction.
    if (ast.exprs[call.callee].kind == ast::ExprKind::Path) {
      const ast::ExprPath& path =
          ast.exprs[call.callee].payload.get<ast::ExprPath>();
      const ast::PathIdx path_idx = path.idx;
      if (const auto* use = variant_use(path_idx)) {
        return lower_variant_construct(expr, use);
      }
    }
    internal(node.span, "call without target");
    return Val{size_one, error_type(), false, false};
  }
  if (target->is_method) {
    return lower_associated_call(expr);
  }
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  const analyzer::CheckedModule::FnSig& sig = def.functions[target->index];
  if (ast.items[sig.item].kind == ast::ItemKind::Intrinsic) {
    return lower_intrinsic_call(
        expr, sig, fn_instance_args(target->module, target->index));
  }
  if (target->module < pkg.tree.modules.size() &&
      pkg.tree.modules[target->module]->is_summary) {
    return lower_summary_call(expr, sig.name, sig.params, sig.ret, sig.item,
                              target->module, call.args, std::nullopt);
  }
  if (sig.is_extern) {
    return lower_extern_call(expr, sig);
  }
  if (sig.name == "write" && pkg.tree.is_staged_item("fmt", sig.item)) {
    return fmt_lowering_.lower_write(expr, sig);
  }
  if (sig.name == "format" && pkg.tree.is_staged_item("fmt", sig.item)) {
    return fmt_lowering_.lower_format(expr, sig);
  }
  if ((sig.name == "print" || sig.name == "println" || sig.name == "eprint" ||
       sig.name == "eprintln") &&
      pkg.tree.is_staged_item("io", sig.item)) {
    return fmt_lowering_.lower_print(expr, sig);
  }
  const std::vector<u32> comp = comp_positions(sig.item);
  std::vector<CompVal> comp_args;
  for (u32 pos : comp) {
    CompVal arg;
    if (!comp_lowering_.evaluate(module, call.args[pos], arg)) {
      return Val{size_one, error_type(), false, false};
    }
    comp_args.push_back(std::move(arg));
  }
  const ir::FunctionIdx fn =
      fn_index(target->module, sig.item, sig.name, sig.params, sig.ret,
               callee_inst(target), std::move(comp_args));
  if (!fn.is_valid()) {
    return Val{size_one, error_type(), false, false};
  }
  if (call.args.size() != sig.params.size()) {
    internal(node.span, "call arity");
    return Val{size_one, error_type(), false, false};
  }
  std::vector<ir::OperandIdx> ops;
  ops.push_back(builder.operand(ir::Operand::from_function(
      fn, builder.primitive(ir::TypeTag::Function))));
  usize comp_at = 0;
  for (usize i = 0; i < call.args.size(); ++i) {
    if (comp_at < comp.size() && comp[comp_at] == i) {
      ++comp_at;
      continue;
    }
    Val arg = lower_moved(call.args[i], &sig.params[i]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    ops.push_back(arg_for(arg, sig.params[i]));
  }
  if (tag_of(sig.ret) == ir::TypeTag::Never) {
    emit_void(ir::Opcode::Call, ops);
    emit_void(ir::Opcode::Unreachable, {});
    return Val{size_one, sig.ret, false, false};
  }
  if (tag_of(sig.ret) == ir::TypeTag::Void) {
    emit_void(ir::Opcode::Call, ops);
    return Val{size_one, sig.ret, false, false};
  }
  const ir::RegisterIdx dst = emit(ir::Opcode::Call, sig.ret, ops);
  return Val{to_operand(dst, sig.ret), sig.ret, false, false};
}

Val Lowerer::lower_associated_call(ast::ExprIdx expr) {
  const ast::ExprNode& call = ast.exprs[expr];
  const analyzer::CheckedModule::CallTarget* target =
      call_target(call.payload.get<ast::ExprCall>().callee);
  if (target == nullptr || !target->is_method) {
    internal(call.span, "call without target");
    return Val{size_one, error_type(), false, false};
  }
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  const analyzer::CheckedModule::MethodInfo& info = def.methods[target->index];
  if (info.receiver != analyzer::CheckedModule::ReceiverKind::None) {
    internal(call.span, "method without receiver");
    return Val{size_one, error_type(), false, false};
  }
  const std::span<const ast::ExprIdx> args =
      call.payload.get<ast::ExprCall>().args;
  if (target->module < pkg.tree.modules.size() &&
      pkg.tree.modules[target->module]->is_summary) {
    return lower_summary_call(expr, info.name, info.params, info.ret, info.item,
                              target->module, args, std::nullopt);
  }
  const std::vector<u32> comp = comp_positions(info.item);
  std::vector<CompVal> comp_args;
  for (u32 pos : comp) {
    CompVal arg;
    if (!comp_lowering_.evaluate(module, args[pos], arg)) {
      return Val{size_one, error_type(), false, false};
    }
    comp_args.push_back(std::move(arg));
  }
  const ir::FunctionIdx fn =
      fn_index(target->module, info.item, info.name, info.params, info.ret,
               callee_inst(target), std::move(comp_args), ir::SymbolKind::Assoc,
               /*extra_generics=*/{}, info.self_type);
  if (!fn.is_valid()) {
    internal(call.span, "call without function");
    return Val{size_one, error_type(), false, false};
  }
  // Recorded params already exclude the receiver: associated
  // functions lower like free functions.
  if (args.size() != info.params.size()) {
    internal(call.span, "call arity");
    return Val{size_one, error_type(), false, false};
  }
  std::vector<ir::OperandIdx> ops;
  ops.push_back(builder.operand(ir::Operand::from_function(
      fn, builder.primitive(ir::TypeTag::Function))));
  usize comp_at = 0;
  for (usize i = 0; i < args.size(); ++i) {
    if (comp_at < comp.size() && comp[comp_at] == i) {
      ++comp_at;
      continue;
    }
    Val arg = lower_moved(args[i], &info.params[i]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    ops.push_back(arg_for(arg, info.params[i]));
  }
  if (tag_of(info.ret) == ir::TypeTag::Void) {
    emit_void(ir::Opcode::Call, ops);
    return Val{size_one, info.ret, false, false};
  }
  const ir::RegisterIdx dst = emit(ir::Opcode::Call, info.ret, ops);
  return Val{to_operand(dst, info.ret), info.ret, false, false};
}

// Calls through an intrinsic declaration: known names map to
// runtime hooks or IR operations. The one undeclared legacy name,
// `panic`, still arrives through lower_panic.
ir::RegisterIdx Lowerer::emit_heap_alloc(ir::TypeIdx elem,
                                         ir::TypeIdx ref_ty,
                                         ir::OperandIdx count,
                                         diag::Span span) {
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  const ir::TypeIdx usize_ty = usize_type();
  // The runtime counts bytes; the compiler supplies the element size
  // and alignment it reserved them at.
  const ir::RegisterIdx bytes = emit_type_query(ir::Opcode::TypeSizeOf, elem);
  const ir::RegisterIdx align = emit_type_query(ir::Opcode::TypeAlignOf, elem);
  if (failed) {
    return ir::RegisterIdx(base::INVALID_IDX);
  }
  const ir::RegisterIdx total =
      emit(ir::Opcode::IntMul, usize_ty, {to_operand(bytes, usize_ty), count});
  if (failed) {
    return ir::RegisterIdx(base::INVALID_IDX);
  }
  // A byte count that wrapped would reserve a smaller block than the
  // caller believes it owns; a zero-sized element cannot overflow.
  {
    const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
    const ir::RegisterIdx no_bytes = emit(
        ir::Opcode::Eq, boolean, {to_operand(bytes, usize_ty), const_usize(0)});
    const ir::BlockIdx check_block = reserve_block();
    const ir::BlockIdx checked_block = reserve_block();
    emit_cond_br(to_operand(no_bytes, boolean), checked_block, check_block);
    switch_to(check_block);
    const ir::RegisterIdx limit =
        emit(ir::Opcode::UintDiv, usize_ty,
             {const_usize(~0ull), to_operand(bytes, usize_ty)});
    const ir::RegisterIdx too_big =
        emit(ir::Opcode::Gt, boolean, {count, to_operand(limit, usize_ty)});
    const ir::BlockIdx panic_block = reserve_block();
    emit_cond_br(to_operand(too_big, boolean), panic_block, checked_block);
    switch_to(panic_block);
    emit_panic(span, str_operand("capacity overflow"));
    switch_to(checked_block);
    if (failed) {
      return ir::RegisterIdx(base::INVALID_IDX);
    }
  }
  const ir::ExternalFunctionIdx ext =
      declare_external("alcy_alloc", ptr_ty, {usize_ty, usize_ty});
  const ir::RegisterIdx result =
      emit(ir::Opcode::Call, ptr_ty,
           {builder.operand(ir::Operand::from_external_function(
                ext, builder.primitive(ir::TypeTag::Function))),
            to_operand(total, usize_ty), to_operand(align, usize_ty)});
  if (failed) {
    return ir::RegisterIdx(base::INVALID_IDX);
  }
  // The caller's `&mut` return is a reference; the runtime pointer
  // needs its pointee label for later element offsets.
  return emit(ir::Opcode::TypeCast, ref_ty, {to_operand(result, ptr_ty)});
}

// A call into a package a summary answers for: the callee's code lives
// in the dependency's own artifact, so the call names a symbol the
// linker resolves, and only the surface's own types - primitives and
// references over them - pass this way (ADR-0078).

Val Lowerer::lower_summary_call(ast::ExprIdx expr,
                                std::string_view name,
                                std::span<const ir::TypeIdx> params,
                                ir::TypeIdx ret,
                                ast::ItemIdx item,
                                u32 module,
                                std::span<const ast::ExprIdx> args,
                                std::optional<Val> receiver) {
  const ast::ExprNode& node = ast.exprs[expr];
  const usize count = args.size() + (receiver.has_value() ? 1 : 0);
  if (count != params.size()) {
    internal(node.span, "summary call arity");
    return Val{size_one, error_type(), false, false};
  }
  // The symbol the dependency's build wrote for this item, which the
  // summary carried with it. The callee's own item is exact, which a
  // name cannot be where two impls carry one; a name is the fallback.
  std::string_view symbol;
  if (module < pkg.tree.modules.size()) {
    std::string_view by_name;
    for (const analyzer::SummarySymbol& entry :
         pkg.tree.modules[module]->summary_symbols) {
      if (item.is_valid() && entry.item == item) {
        symbol = strings.get(entry.symbol);
        break;
      }
      if (by_name.empty() && strings.get(entry.name) == name) {
        by_name = strings.get(entry.symbol);
      }
    }
    if (symbol.empty()) {
      symbol = by_name;
    }
  }
  if (symbol.empty()) {
    internal(node.span, "summary call without a symbol");
    return Val{size_one, error_type(), false, false};
  }
  // The declaration names the symbol as it stands; the parameter shapes
  // are the external table's, which unwrap to the plain value the
  // callee's own artifact takes.
  const std::vector<ir::TypeIdx> declared(params.begin(), params.end());
  const ir::ExternalFunctionIdx callee =
      declare_external(symbol, ret, declared);
  if (!callee.is_valid()) {
    return Val{size_one, error_type(), false, false};
  }
  std::vector<ir::OperandIdx> ops;
  ops.push_back(builder.operand(ir::Operand::from_external_function(
      callee, builder.primitive(ir::TypeTag::Function))));
  usize at = 0;
  if (receiver.has_value()) {
    ops.push_back(arg_for(*receiver, params[0]));
    at = 1;
  }
  for (usize i = 0; i < args.size(); ++i) {
    Val arg = lower_moved(args[i], &params[at + i]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    // The argument crosses as the value the callee takes, the same way
    // a C entry point's does.
    ops.push_back(arg_for(arg, params[at + i]));
  }
  const ir::RegisterIdx result = ret == builder.primitive(ir::TypeTag::Void)
                                     ? ir::RegisterIdx(base::INVALID_IDX)
                                     : emit(ir::Opcode::Call, ret, ops);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  if (!result.is_valid()) {
    return Val{size_one, error_type(), false, false};
  }
  return Val{to_operand(result, ret), ret, false, false};
}

// An `extern "C"` call: the declaration names the symbol the linker
// resolves, arguments pass by value, and the gate was checked at the
// call site (ADR-0051).
Val Lowerer::lower_extern_call(ast::ExprIdx expr,
                               const analyzer::CheckedModule::FnSig& sig) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
  if (call.args.size() != sig.params.size()) {
    internal(node.span, "extern call arity");
    return Val{size_one, error_type(), false, false};
  }
  const ir::ExternalFunctionIdx ext =
      declare_external(sig.name, sig.ret, sig.params);
  if (!ext.is_valid()) {
    return Val{size_one, error_type(), false, false};
  }
  std::vector<ir::OperandIdx> ops;
  ops.push_back(builder.operand(ir::Operand::from_external_function(
      ext, builder.primitive(ir::TypeTag::Function))));
  for (usize i = 0; i < call.args.size(); ++i) {
    Val arg = lower_moved(call.args[i], &sig.params[i]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    ops.push_back(arg_for(arg, sig.params[i]));
  }
  if (tag_of(sig.ret) == ir::TypeTag::Void) {
    emit_void(ir::Opcode::Call, ops);
    return Val{size_one, sig.ret, false, false};
  }
  const ir::RegisterIdx dst = emit(ir::Opcode::Call, sig.ret, ops);
  return Val{to_operand(dst, sig.ret), sig.ret, false, false};
}

Val Lowerer::lower_intrinsic_call(ast::ExprIdx expr,
                                  const analyzer::CheckedModule::FnSig& sig,
                                  const std::vector<ir::TypeIdx>& type_args) {
  const ast::ExprNode& node = ast.exprs[expr];
  const std::string_view name = sig.name;
  if (name == "panic") {
    return lower_panic(expr);
  }
  if (name == "sys_write") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 2) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val fd = lower_expr(call.args[0], nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    Val buf = lower_expr(call.args[1], nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const Val material = materialize(buf);
    const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
    const ir::TypeIdx usize_ty = usize_type();
    const ir::RegisterIdx bytes =
        emit(ir::Opcode::ExtractValue, ptr_ty, {material.op, index_operand(0)});
    const ir::RegisterIdx len = emit(ir::Opcode::ExtractValue, usize_ty,
                                     {material.op, index_operand(1)});
    const ir::ExternalFunctionIdx ext = declare_external(
        "alcy_sys_write", builder.primitive(ir::TypeTag::Void),
        {builder.primitive(ir::TypeTag::I32), ptr_ty, usize_ty});
    emit_void(
        ir::Opcode::Call,
        {builder.operand(ir::Operand::from_external_function(
             ext, builder.primitive(ir::TypeTag::Function))),
         use_value(fd), to_operand(bytes, ptr_ty), to_operand(len, usize_ty)});
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }
  if (name == "memcopy") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 3) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val dst = lower_expr(call.args[0], nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    Val src = lower_expr(call.args[1], nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    Val len = lower_expr(call.args[2], nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    // Both pointers are the same storage type, so the element's
    // alignment is the copy's: the backend may use it, and does not
    // have to.
    ir::InstructionFlags flags;
    if (tag_of(dst.type) == ir::TypeTag::MutRef) {
      const ir::TypeIdx pointee =
          builder.ref_types()[builder.types()[dst.type.idx].as_ref()].pointee;
      const u64 align =
          ir::type_layout(builder.state(), type_origin(pointee), width).align;
      u32 log2 = 0;
      while (log2 < 15 && (static_cast<u64>(1) << log2) < align) {
        ++log2;
      }
      if ((static_cast<u64>(1) << log2) == align) {
        flags.mem_align_log2 = static_cast<u8>(log2);
      }
    }
    emit_void(ir::Opcode::Memcopy,
              {use_value(dst), use_value(src), use_value(len)}, flags);
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }
  if (name == "drop_in_place") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 1 || type_args.size() != 1) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val ptr = lower_expr(call.args[0], nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const Val material = materialize(ptr);
    // The end runs wherever the pass visits this point; the pointer is
    // the caller's guarantee that the value is there.
    record_drop_in_place(material.op, type_args[0], node.span);
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }
  if (name == "slice_at") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 2) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val s = lower_expr(call.args[0], nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    Val index = lower_expr(call.args[1], &sig.params[1]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const Val material = materialize(s);
    ir::OperandIdx bytes = ir::OperandIdx::invalid();
    ir::OperandIdx len = ir::OperandIdx::invalid();
    if (!slice_parts(material, bytes, len)) {
      unsupported(node.span, "slice_at receiver");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx slice_ty = slice_pointee(material.type);
    const ir::SliceType& shape =
        builder.state().slice_types[builder.state().types[slice_ty].as_slice()];
    const ir::TypeIdx elem_ref = builder.reference_type(shape.element, false);
    const ir::RegisterIdx element =
        emit(ir::Opcode::ElemOffset, elem_ref, {bytes, use_value(index)});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(element, elem_ref), elem_ref, false, false};
  }
  if (name == "slice_at_mut") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 2) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val s = lower_expr(call.args[0], nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    Val index = lower_expr(call.args[1], &sig.params[1]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const Val material = materialize(s);
    ir::OperandIdx bytes = ir::OperandIdx::invalid();
    ir::OperandIdx len = ir::OperandIdx::invalid();
    if (!slice_parts(material, bytes, len)) {
      unsupported(node.span, "slice_at_mut receiver");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx slice_ty = slice_pointee(material.type);
    const ir::SliceType& shape =
        builder.state().slice_types[builder.state().types[slice_ty].as_slice()];
    const ir::TypeIdx elem_ref = builder.reference_type(shape.element, true);
    const ir::RegisterIdx element =
        emit(ir::Opcode::ElemOffset, elem_ref, {bytes, use_value(index)});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(element, elem_ref), elem_ref, false, false};
  }
  if (name == "read") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 1) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val ptr = lower_expr(call.args[0], &sig.params[0]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const Val material = materialize(ptr);
    const ir::RegisterIdx value =
        emit(ir::Opcode::Load, sig.ret, {material.op});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(value, sig.ret), sig.ret, false, false};
  }
  if (name == "str_len" || name == "str_byte" || name == "str_slice") {
    return lower_str_intrinsic(expr, name);
  }
  if (name == "arg") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 1) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx str_ty = builder.primitive(ir::TypeTag::Str);
    const ir::TypeIdx i64_ty = builder.primitive(ir::TypeTag::I64);
    Val index = lower_expr(call.args[0], &sig.params[0]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const ir::ExternalFunctionIdx ext =
        declare_external("alcy_arg", str_ty, {i64_ty});
    const ir::RegisterIdx dst =
        emit(ir::Opcode::Call, str_ty,
             {builder.operand(ir::Operand::from_external_function(
                  ext, builder.primitive(ir::TypeTag::Function))),
              arg_for(index, sig.params[0])});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(dst, str_ty), str_ty, false, false};
  }
  if (name == "arg_count") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (!call.args.empty()) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx i64_ty = builder.primitive(ir::TypeTag::I64);
    const ir::ExternalFunctionIdx ext =
        declare_external("alcy_arg_count", i64_ty, {});
    const ir::RegisterIdx dst =
        emit(ir::Opcode::Call, i64_ty,
             {builder.operand(ir::Operand::from_external_function(
                 ext, builder.primitive(ir::TypeTag::Function)))});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(dst, i64_ty), i64_ty, false, false};
  }
  if (name == "file_size" || name == "read_file_into" ||
      name == "write_file_all") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    const usize arity =
        name == "file_size" ? 1 : (name == "read_file_into" ? 3 : 2);
    if (call.args.size() != arity) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
    const ir::TypeIdx usize_ty = usize_type();
    const ir::TypeIdx i64_ty = builder.primitive(ir::TypeTag::I64);
    Val path = lower_expr(call.args[0], &sig.params[0]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const ir::OperandIdx path_val = arg_for(path, sig.params[0]);
    const ir::RegisterIdx bytes =
        emit(ir::Opcode::ExtractValue, ptr_ty, {path_val, index_operand(0)});
    const ir::RegisterIdx len =
        emit(ir::Opcode::ExtractValue, usize_ty, {path_val, index_operand(1)});
    std::vector<ir::OperandIdx> ops;
    if (name == "file_size") {
      const ir::ExternalFunctionIdx ext =
          declare_external("alcy_file_size", i64_ty, {ptr_ty, usize_ty});
      ops.push_back(builder.operand(ir::Operand::from_external_function(
          ext, builder.primitive(ir::TypeTag::Function))));
      ops.push_back(to_operand(bytes, ptr_ty));
      ops.push_back(to_operand(len, usize_ty));
    } else if (name == "write_file_all") {
      Val data = lower_expr(call.args[1], &sig.params[1]);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      const ir::OperandIdx data_val = arg_for(data, sig.params[1]);
      const ir::RegisterIdx data_bytes =
          emit(ir::Opcode::ExtractValue, ptr_ty, {data_val, index_operand(0)});
      const ir::RegisterIdx data_len = emit(ir::Opcode::ExtractValue, usize_ty,
                                            {data_val, index_operand(1)});
      const ir::ExternalFunctionIdx ext = declare_external(
          "alcy_write_file", i64_ty, {ptr_ty, usize_ty, ptr_ty, usize_ty});
      ops.push_back(builder.operand(ir::Operand::from_external_function(
          ext, builder.primitive(ir::TypeTag::Function))));
      ops.push_back(to_operand(bytes, ptr_ty));
      ops.push_back(to_operand(len, usize_ty));
      ops.push_back(to_operand(data_bytes, ptr_ty));
      ops.push_back(to_operand(data_len, usize_ty));
    } else {
      Val out = lower_expr(call.args[1], &sig.params[1]);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      Val out_len = lower_expr(call.args[2], &sig.params[2]);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      const ir::ExternalFunctionIdx ext = declare_external(
          "alcy_read_file_into", i64_ty, {ptr_ty, usize_ty, ptr_ty, usize_ty});
      ops.push_back(builder.operand(ir::Operand::from_external_function(
          ext, builder.primitive(ir::TypeTag::Function))));
      ops.push_back(to_operand(bytes, ptr_ty));
      ops.push_back(to_operand(len, usize_ty));
      ops.push_back(arg_for(out, sig.params[1]));
      ops.push_back(arg_for(out_len, sig.params[2]));
    }
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::RegisterIdx dst = emit(ir::Opcode::Call, i64_ty, ops);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(dst, i64_ty), i64_ty, false, false};
  }
  if (name == "elem_ptr" || name == "elem_ref" || name == "ptr_offset" ||
      name == "ptr_offset_mut") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 2) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    std::vector<ir::OperandIdx> args;
    for (usize i = 0; i < 2; ++i) {
      Val arg = lower_moved(call.args[i], &sig.params[i]);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      args.push_back(arg_for(arg, sig.params[i]));
    }
    const ir::RegisterIdx result = emit(ir::Opcode::ElemOffset, sig.ret, args);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(result, sig.ret), sig.ret, false, false};
  }
  if (name == "uninit_write" || name == "uninit_assume" ||
      name == "uninit_ref") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    const usize arity = name == "uninit_write" ? 2 : 1;
    if (call.args.size() != arity) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val slot = lower_expr(call.args[0], &sig.params[0]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const ir::OperandIdx slot_op = arg_for(slot, sig.params[0]);
    if (name == "uninit_assume" || name == "uninit_ref") {
      // The wrapper is representation-transparent, so releasing the
      // value is a relabel of the same address.
      const ir::RegisterIdx held =
          emit(ir::Opcode::TypeCast, sig.ret, {slot_op});
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      return Val{to_operand(held, sig.ret), sig.ret, false, false};
    }
    Val value = lower_moved(call.args[1], &sig.params[1]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    emit_void(ir::Opcode::Store, {use_value(value), slot_op});
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }
  if (name == "size_of" || name == "align_of") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (!call.args.empty() || type_args.size() != 1) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    const ir::RegisterIdx measured = emit_type_query(
        name == "size_of" ? ir::Opcode::TypeSizeOf : ir::Opcode::TypeAlignOf,
        type_args[0]);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(measured, usize_type()), usize_type(), false, false};
  }
  if (name == "wrapping_add" || name == "wrapping_sub" ||
      name == "wrapping_mul") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 2 || type_args.size() != 1) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx type = type_args[0];
    Val left = lower_expr(call.args[0], &type);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    Val right = lower_expr(call.args[1], &type);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const ir::Opcode op = name == "wrapping_add"   ? ir::Opcode::IntAddWrap
                          : name == "wrapping_sub" ? ir::Opcode::IntSubWrap
                                                   : ir::Opcode::IntMulWrap;
    const ir::RegisterIdx result =
        emit(op, type, {use_value(left), use_value(right)});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(result, type), type, false, false};
  }
  if (name == "alloc" || name == "dealloc") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    const usize arity = name == "alloc" ? 1 : 2;
    if (call.args.size() != arity || type_args.size() != 1) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
    const ir::TypeIdx usize_ty = usize_type();
    std::vector<ir::OperandIdx> args;
    for (usize i = 0; i < arity; ++i) {
      Val arg = lower_moved(call.args[i], &sig.params[i]);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      args.push_back(arg_for(arg, sig.params[i]));
    }
    if (name == "dealloc") {
      // The runtime counts bytes; the compiler supplies the element
      // size and alignment it reserved them at.
      const ir::RegisterIdx bytes =
          emit_type_query(ir::Opcode::TypeSizeOf, type_args[0]);
      const ir::RegisterIdx align =
          emit_type_query(ir::Opcode::TypeAlignOf, type_args[0]);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      const ir::RegisterIdx total = emit(
          ir::Opcode::IntMul, usize_ty, {to_operand(bytes, usize_ty), args[1]});
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      const ir::ExternalFunctionIdx ext =
          declare_external("alcy_dealloc", builder.primitive(ir::TypeTag::Void),
                           {ptr_ty, usize_ty, usize_ty});
      emit_void(
          ir::Opcode::Call,
          {builder.operand(ir::Operand::from_external_function(
               ext, builder.primitive(ir::TypeTag::Function))),
           args[0], to_operand(total, usize_ty), to_operand(align, usize_ty)});
      return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
    }
    const ir::RegisterIdx labelled =
        emit_heap_alloc(type_args[0], sig.ret, args[0], node.span);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(labelled, sig.ret), sig.ret, false, false};
  }
  if (name == "slice_len") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 1 || type_args.size() != 1) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val receiver = lower_moved(call.args[0], &sig.params[0]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const Val material = materialize(receiver);
    ir::OperandIdx bytes = ir::OperandIdx::invalid();
    ir::OperandIdx len = ir::OperandIdx::invalid();
    if (!slice_parts(material, bytes, len)) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{len, usize_type(), false, false};
  }
  if (name == "slice_from_parts" || name == "slice_from_parts_mut") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 2 || type_args.size() != 1) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val ptr = lower_expr(call.args[0], &sig.params[0]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    Val len = lower_expr(call.args[1], &sig.params[1]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    return build_slice_value(use_value(ptr), use_value(len), sig.ret,
                             node.span);
  }
  if (name == "str_from_parts") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 2) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val ptr = lower_expr(call.args[0], nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    Val len = lower_expr(call.args[1], nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const ir::TypeIdx str_ty = builder.primitive(ir::TypeTag::Str);
    const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
    const ir::TypeIdx usize_ty = usize_type();
    const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, str_ty, {size_one});
    const ir::RegisterIdx field0 =
        emit(ir::Opcode::GetElementPtr, ptr_ty,
             {to_operand(addr, str_ty), zero_i32, index_operand(0)});
    emit_void(ir::Opcode::Store, {use_value(ptr), to_operand(field0, ptr_ty)});
    const ir::RegisterIdx field1 =
        emit(ir::Opcode::GetElementPtr, usize_ty,
             {to_operand(addr, str_ty), zero_i32, index_operand(1)});
    emit_void(ir::Opcode::Store,
              {use_value(len), to_operand(field1, usize_ty)});
    return Val{to_operand(addr, str_ty), str_ty, true, false};
  }
  internal(node.span, "unknown intrinsic");
  return Val{size_one, error_type(), false, false};
}

ir::TypeIdx Lowerer::usize_type() {
  return builder.primitive(width == ir::PointerWidth::W64 ? ir::TypeTag::U64
                                                          : ir::TypeTag::U32);
}

// Extracts the (bytes, len) pair from a materialized str value.
bool Lowerer::str_parts(Val str,
                        ir::OperandIdx& bytes_out,
                        ir::OperandIdx& len_out) {
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  const ir::TypeIdx usize_ty = usize_type();
  const ir::RegisterIdx bytes =
      emit(ir::Opcode::ExtractValue, ptr_ty, {str.op, index_operand(0)});
  if (failed) {
    return false;
  }
  const ir::RegisterIdx len =
      emit(ir::Opcode::ExtractValue, usize_ty, {str.op, index_operand(1)});
  if (failed) {
    return false;
  }
  bytes_out = to_operand(bytes, ptr_ty);
  len_out = to_operand(len, usize_ty);
  return true;
}

// Advances a byte pointer by an integer offset through int
// arithmetic: GEP only tracks alloca sites, never derived pointers.
ir::OperandIdx Lowerer::advance_ptr(ir::OperandIdx ptr,
                                    ir::OperandIdx offset,
                                    diag::Span span) {
  const ir::TypeIdx usize_ty = usize_type();
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  const ir::RegisterIdx as_int = emit(ir::Opcode::TypeCast, usize_ty, {ptr});
  if (failed) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::RegisterIdx sum = emit(ir::Opcode::IntAdd, usize_ty,
                                   {to_operand(as_int, usize_ty), offset});
  if (failed) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  const ir::RegisterIdx bumped =
      emit(ir::Opcode::TypeCast, ptr_ty, {to_operand(sum, usize_ty)});
  if (failed) {
    return ir::OperandIdx(base::INVALID_IDX);
  }
  (void)span;
  return to_operand(bumped, ptr_ty);
}

// Extracts the (bytes, len) pair from a materialized slice value,
// the fat struct a `&[T]` carries. Mirrors str_parts exactly; the
// borrow checker anchors both fields to the loans the value carries.
bool Lowerer::slice_parts(Val slice,
                          ir::OperandIdx& bytes_out,
                          ir::OperandIdx& len_out) {
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  const ir::TypeIdx usize_ty = usize_type();
  const ir::RegisterIdx bytes =
      emit(ir::Opcode::ExtractValue, ptr_ty, {slice.op, index_operand(0)});
  if (failed) {
    return false;
  }
  const ir::RegisterIdx len =
      emit(ir::Opcode::ExtractValue, usize_ty, {slice.op, index_operand(1)});
  if (failed) {
    return false;
  }
  bytes_out = to_operand(bytes, ptr_ty);
  len_out = to_operand(len, usize_ty);
  return true;
}

// Builds the fat struct for a slice view: an alloca holding the base
// pointer and the length. Stores union the base's loans into the
// value, so the view stays tied to whatever it was taken from.
Val Lowerer::build_slice_value(ir::OperandIdx base,
                               ir::OperandIdx len,
                               ir::TypeIdx ref_ty,
                               diag::Span span) {
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  const ir::TypeIdx usize_ty = usize_type();
  const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, ref_ty, {size_one});
  const ir::RegisterIdx field0 =
      emit(ir::Opcode::GetElementPtr, ptr_ty,
           {to_operand(addr, ref_ty), zero_i32, index_operand(0)});
  emit_void(ir::Opcode::Store, {base, to_operand(field0, ptr_ty)});
  const ir::RegisterIdx field1 =
      emit(ir::Opcode::GetElementPtr, usize_ty,
           {to_operand(addr, ref_ty), zero_i32, index_operand(1)});
  emit_void(ir::Opcode::Store, {len, to_operand(field1, usize_ty)});
  // The fat struct rides by value, matching the parameter convention:
  // a `&[T]` is the pair itself, not a pointer to one.
  const ir::RegisterIdx fat =
      emit(ir::Opcode::Load, ref_ty, {to_operand(addr, ref_ty)});
  (void)span;
  return Val{to_operand(fat, ref_ty), ref_ty, false, false};
}

ir::OperandIdx Lowerer::const_usize(u64 value) {
  const ir::TypeIdx usize_ty = usize_type();
  ir::Immutable imm{.type = usize_ty, .data = {}};
  if (tag_of(usize_ty) == ir::TypeTag::U64) {
    imm.data.u64_value = value;
  } else {
    imm.data.u32_value = static_cast<u32>(value);
  }
  return to_operand(builder.immutable(imm), usize_ty);
}

ir::OperandIdx Lowerer::int_operand(u64 value, ir::TypeIdx type) {
  ir::Immutable imm{.type = type, .data = {}};
  if (integer_bits(tag_of(type)) == 64) {
    imm.data.u64_value = value;
  } else {
    imm.data.u32_value = static_cast<u32>(value);
  }
  return to_operand(builder.immutable(imm), type);
}

bool Lowerer::is_range_type(ir::TypeIdx type) {
  const ir::TypeIdx origin = type_origin(type);
  if (tag_of(origin) != ir::TypeTag::Struct) {
    return false;
  }
  u32 index = 0;
  return struct_field_index(origin, "start", index) &&
         struct_field_index(origin, "end", index);
}

// The half-open `[start, width)` a range value names. Each endpoint is
// decoded from its `Bound` tag: the tag and payload loads happen
// unconditionally, and `Unbounded` discards the payload with a `select`
// rather than a branch, since its slot is never written. Exactly one of
// the endpoint's value and its limit carries the `+1` that inclusion
// needs; both are the compiler's own glue and wrap (ADR-0065), so a
// checked build never aborts on the slot an `Unbounded` endpoint leaves
// unwritten.
bool Lowerer::run_bounds(Val range,
                         ir::OperandIdx len,
                         RunBounds& out,
                         diag::Span span) {
  u32 start_index = 0;
  u32 end_index = 0;
  if (!struct_field_index(range.type, "start", start_index) ||
      !struct_field_index(range.type, "end", end_index)) {
    internal(span, "range without endpoints");
    return false;
  }
  Val start_slot = field_addr(range, "start", span);
  Val end_slot = field_addr(range, "end", span);
  if (failed) {
    return false;
  }
  const ir::TypeIdx bound_ty = type_origin(start_slot.type);
  u32 included_v = 0;
  u32 excluded_v = 0;
  u32 unbounded_v = 0;
  if (!variant_index(bound_ty, "Included", included_v) ||
      !variant_index(bound_ty, "Excluded", excluded_v) ||
      !variant_index(bound_ty, "Unbounded", unbounded_v)) {
    internal(span, "bound without variants");
    return false;
  }
  const std::vector<ir::TypeIdx> payloads =
      variant_payload(bound_ty, included_v);
  if (payloads.size() != 1) {
    internal(span, "bound arity");
    return false;
  }
  const ir::TypeIdx element = payloads[0];
  const ir::TypeIdx usize_ty = usize_type();
  const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
  const ir::OperandIdx one = const_usize(1);

  auto require = [&](ir::RegisterIdx good) {
    const ir::BlockIdx ok_block = reserve_block();
    const ir::BlockIdx bad_block = reserve_block();
    emit_cond_br(to_operand(good, boolean), ok_block, bad_block);
    switch_to(bad_block);
    emit_panic(span, str_operand("slice out of bounds"));
    switch_to(ok_block);
  };

  auto decode = [&](Val slot, bool is_start) -> Val {
    const Val tag = load_disc(slot);
    const ir::RegisterIdx is_unbounded = emit(
        ir::Opcode::Eq, boolean, {use_value(tag), disc_operand(unbounded_v)});
    const ir::RegisterIdx is_included = emit(
        ir::Opcode::Eq, boolean, {use_value(tag), disc_operand(included_v)});
    const ir::RegisterIdx is_excluded = emit(
        ir::Opcode::Eq, boolean, {use_value(tag), disc_operand(excluded_v)});
    const ir::RegisterIdx payload_addr = payload_field_addr(
        slot, bound_ty, variant_fields(bound_ty, included_v), 0);
    const ir::RegisterIdx payload =
        emit(ir::Opcode::Load, element, {to_operand(payload_addr, element)});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val wide{to_operand(payload, element), element, false, false};
    if (tag_of(element) != tag_of(usize_ty)) {
      const ir::RegisterIdx casted =
          emit(ir::Opcode::TypeCast, usize_ty, {use_value(wide)});
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      wide = Val{to_operand(casted, usize_ty), usize_ty, false, false};
    }
    // An included end and an excluded start exclude the element one
    // past the bound, so their value is the bound plus one and the
    // limit is the length; the other spellings put the one on the
    // limit instead. Both adds are the compiler's own glue, and an
    // `Unbounded` endpoint's payload slot is never written, so they
    // wrap: the source asked for neither, and a checked build must not
    // abort on a slot the decode reads and discards.
    const ir::RegisterIdx use_bump = is_start ? is_excluded : is_included;
    const ir::RegisterIdx bumped =
        emit(ir::Opcode::IntAddWrap, usize_ty, {use_value(wide), one});
    const ir::RegisterIdx bounded =
        emit(ir::Opcode::Select, usize_ty,
             {to_operand(use_bump, boolean), to_operand(bumped, usize_ty),
              use_value(wide)});
    // The bound is an index into the run, and `len` itself names the
    // end, so it is in range exactly when it does not exceed `len`. The
    // `+1` an included end or an excluded start carries is already in
    // `bounded`, so one comparison serves both spellings. Reading the
    // bound against `len + 1` instead rejected the last element an
    // inclusive end names, and read a length that wrapped.
    const ir::RegisterIdx within =
        emit(ir::Opcode::Le, boolean, {to_operand(bounded, usize_ty), len});
    const ir::RegisterIdx ok =
        emit(ir::Opcode::Or, boolean,
             {to_operand(is_unbounded, boolean), to_operand(within, boolean)});
    require(ok);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::OperandIdx empty_side = is_start ? const_usize(0) : len;
    const ir::RegisterIdx value =
        emit(ir::Opcode::Select, usize_ty,
             {to_operand(is_unbounded, boolean), empty_side,
              to_operand(bounded, usize_ty)});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(value, usize_ty), usize_ty, false, false};
  };

  const Val start = decode(start_slot, true);
  const Val end = decode(end_slot, false);
  if (failed) {
    return false;
  }
  // An included end names an element, so its run inverts one earlier:
  // `3..=2` decodes as `3..<3`, and only the tag tells the two apart.
  // The run is ordered when the start is not past the end, and not equal
  // to it either when the end names an element. Both bounds are bounded
  // by `len` above, so the width below cannot wrap.
  const Val end_tag = load_disc(end_slot);
  const ir::RegisterIdx end_included = emit(
      ir::Opcode::Eq, boolean, {use_value(end_tag), disc_operand(included_v)});
  const ir::RegisterIdx past =
      emit(ir::Opcode::Gt, boolean, {use_value(start), use_value(end)});
  const ir::RegisterIdx equal =
      emit(ir::Opcode::Eq, boolean, {use_value(start), use_value(end)});
  const ir::RegisterIdx both =
      emit(ir::Opcode::And, boolean,
           {to_operand(end_included, boolean), to_operand(equal, boolean)});
  const ir::RegisterIdx inverted =
      emit(ir::Opcode::Or, boolean,
           {to_operand(past, boolean), to_operand(both, boolean)});
  const ir::RegisterIdx ordered = emit(
      ir::Opcode::Select, boolean,
      {to_operand(inverted, boolean), bool_operand(false), bool_operand(true)});
  require(ordered);
  if (failed) {
    return false;
  }
  const ir::RegisterIdx width =
      emit(ir::Opcode::IntSub, usize_ty, {use_value(end), use_value(start)});
  if (failed) {
    return false;
  }
  out.start = use_value(start);
  out.width = to_operand(width, usize_ty);
  return true;
}

bool Lowerer::store_bound(ast::ExprIdx expr,
                          Val range_slot,
                          std::string_view field,
                          ast::ExprIdx endpoint,
                          bool included) {
  const ast::ExprNode& node = ast.exprs[expr];
  Val slot = field_addr(range_slot, field, node.span);
  if (failed) {
    return false;
  }
  const ir::TypeIdx bound_ty = type_origin(slot.type);
  const ir::TypeIdx i32_ty = builder.primitive(ir::TypeTag::I32);
  const ir::RegisterIdx tag_addr = emit(ir::Opcode::GetElementPtr, i32_ty,
                                        {slot.op, zero_i32, index_operand(0)});
  const std::string_view variant_name =
      endpoint.is_valid() ? (included ? "Included" : "Excluded") : "Unbounded";
  u32 variant = 0;
  if (!variant_index(bound_ty, variant_name, variant)) {
    internal(node.span, "bound without declaration");
    return false;
  }
  emit_void(ir::Opcode::Store,
            {disc_operand(variant), to_operand(tag_addr, bound_ty)});
  if (!endpoint.is_valid()) {
    return !failed;
  }
  const std::vector<ir::TypeIdx> payloads = variant_payload(bound_ty, variant);
  if (payloads.size() != 1) {
    internal(node.span, "bound arity");
    return false;
  }
  Val value = lower_expr(endpoint, &payloads[0]);
  if (failed || terminated_cur()) {
    return false;
  }
  const ir::RegisterIdx payload_addr =
      payload_field_addr(slot, bound_ty, variant_fields(bound_ty, variant), 0);
  emit_void(ir::Opcode::Store,
            {use_value(value), to_operand(payload_addr, payloads[0])});
  return !failed;
}

Val Lowerer::lower_range(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprRange& range = node.payload.get<ast::ExprRange>();
  const ir::TypeIdx range_ty = expr_type(expr);
  if (!is_range_type(range_ty)) {
    internal(node.span, "range without type");
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, range_ty, {size_one});
  Val slot{to_operand(addr, range_ty), range_ty, true, false};
  if (!store_bound(expr, slot, "start", range.start, true)) {
    return stopped_value();
  }
  if (!store_bound(expr, slot, "end", range.end, range.inclusive)) {
    return stopped_value();
  }
  return slot;
}

Val Lowerer::lower_subslice(Val base,
                            Val range,
                            bool exclusive,
                            diag::Span span) {
  const ir::TypeTag tag = tag_of(base.type);
  if (tag == ir::TypeTag::Array) {
    if (!base.address) {
      base = address_of(base);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
    }
    const ir::ArrayType& shape =
        builder.state()
            .array_types[builder.state().types[base.type].as_array()];
    RunBounds bounds;
    if (!run_bounds(range, const_usize(shape.count), bounds, span)) {
      return Val{size_one, error_type(), false, false};
    }
    // The address comes first and the loan second, exactly as for an
    // element borrow: the offset names a place inside the array, so
    // the borrow covers the whole array while the run is live.
    const ir::RegisterIdx element =
        emit(ir::Opcode::GetElementPtr, shape.element,
             {base.op, zero_i32, bounds.start}, {.inbounds = true});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx elem_ref =
        builder.reference_type(shape.element, exclusive);
    const ir::RegisterIdx loan = emit(ir::Opcode::Borrow, elem_ref,
                                      {to_operand(element, shape.element)});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx view =
        builder.reference_type(builder.slice_type(shape.element), exclusive);
    return build_slice_value(to_operand(loan, elem_ref), bounds.width, view,
                             span);
  }
  if (is_slice_ref(base.type)) {
    const bool element_exclusive = tag_of(base.type) == ir::TypeTag::MutRef;
    const Val material = materialize(base);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    ir::OperandIdx bytes = ir::OperandIdx::invalid();
    ir::OperandIdx len = ir::OperandIdx::invalid();
    if (!slice_parts(material, bytes, len)) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::SliceType& shape =
        builder.state().slice_types
            [builder.state().types[slice_pointee(base.type)].as_slice()];
    RunBounds bounds;
    if (!run_bounds(range, len, bounds, span)) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx elem_ref =
        builder.reference_type(shape.element, element_exclusive);
    const ir::RegisterIdx element =
        emit(ir::Opcode::ElemOffset, elem_ref, {bytes, bounds.start});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return build_slice_value(to_operand(element, elem_ref), bounds.width,
                             base.type, span);
  }
  if (tag == ir::TypeTag::Str) {
    const Val material = materialize(base);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    ir::OperandIdx bytes = ir::OperandIdx::invalid();
    ir::OperandIdx len = ir::OperandIdx::invalid();
    if (!str_parts(material, bytes, len)) {
      return Val{size_one, error_type(), false, false};
    }
    RunBounds bounds;
    if (!run_bounds(range, len, bounds, span)) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::TypeIdx byte_ref =
        builder.reference_type(builder.primitive(ir::TypeTag::U8), false);
    const ir::RegisterIdx byte =
        emit(ir::Opcode::ElemOffset, byte_ref, {bytes, bounds.start});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return build_slice_value(to_operand(byte, byte_ref), bounds.width,
                             builder.primitive(ir::TypeTag::Str), span);
  }
  internal(span, "run of a non-container");
  return Val{size_one, error_type(), false, false};
}

Val Lowerer::lower_str_intrinsic(ast::ExprIdx expr, std::string_view name) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
  if (call.args.empty()) {
    internal(node.span, "intrinsic arity");
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx str_ty = builder.primitive(ir::TypeTag::Str);
  const ir::TypeIdx usize_ty = usize_type();
  const ir::TypeIdx u8_ty = builder.primitive(ir::TypeTag::U8);
  const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
  Val receiver = lower_expr(call.args[0], &str_ty);
  if (failed || terminated_cur()) {
    return stopped_value();
  }
  const Val material = materialize(receiver);
  ir::OperandIdx bytes = ir::OperandIdx::invalid();
  ir::OperandIdx len = ir::OperandIdx::invalid();
  if (!str_parts(material, bytes, len)) {
    return Val{size_one, error_type(), false, false};
  }
  if (name == "str_len") {
    return Val{len, usize_ty, false, false};
  }
  auto lower_index = [&](ast::ExprIdx arg, ir::OperandIdx& out) {
    Val value = lower_expr(arg, &usize_ty);
    if (failed) {
      return false;
    }
    out = use_value(value);
    return true;
  };
  if (name == "str_byte") {
    if (call.args.size() != 2) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    ir::OperandIdx index = ir::OperandIdx::invalid();
    if (!lower_index(call.args[1], index)) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::RegisterIdx in_bounds =
        emit(ir::Opcode::Lt, boolean, {index, len});
    const ir::BlockIdx ok_block = reserve_block();
    const ir::BlockIdx bad_block = reserve_block();
    emit_cond_br(to_operand(in_bounds, boolean), ok_block, bad_block);
    switch_to(bad_block);
    emit_panic(node.span, str_operand("index out of bounds"));
    switch_to(ok_block);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::OperandIdx addr = advance_ptr(bytes, index, node.span);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::RegisterIdx loaded = emit(ir::Opcode::Load, u8_ty, {addr});
    return Val{to_operand(loaded, u8_ty), u8_ty, false, false};
  }
  if (name == "str_slice") {
    if (call.args.size() != 3) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    ir::OperandIdx start = ir::OperandIdx::invalid();
    if (!lower_index(call.args[1], start)) {
      return Val{size_one, error_type(), false, false};
    }
    ir::OperandIdx end = ir::OperandIdx::invalid();
    if (!lower_index(call.args[2], end)) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::RegisterIdx ordered = emit(ir::Opcode::Gt, boolean, {start, end});
    const ir::RegisterIdx bounded = emit(ir::Opcode::Gt, boolean, {end, len});
    const ir::RegisterIdx bad =
        emit(ir::Opcode::Or, boolean,
             {to_operand(ordered, boolean), to_operand(bounded, boolean)});
    const ir::BlockIdx ok_block = reserve_block();
    const ir::BlockIdx bad_block = reserve_block();
    emit_cond_br(to_operand(bad, boolean), bad_block, ok_block);
    switch_to(bad_block);
    emit_panic(node.span, str_operand("slice out of bounds"));
    switch_to(ok_block);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    // A byte reference at `start`: the element offset names a place
    // inside the source buffer, so the loan the view carries reaches
    // the result str the way it reaches the one `str_from_parts`
    // builds. Advancing by integer arithmetic would leave the register
    // loan-free, and a slice would then outlive the growth that frees
    // the buffer under it.
    const ir::TypeIdx byte_ref = builder.reference_type(u8_ty, false);
    const ir::RegisterIdx sub_ref =
        emit(ir::Opcode::ElemOffset, byte_ref, {bytes, start});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::RegisterIdx width =
        emit(ir::Opcode::IntSub, usize_ty, {end, start});
    const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, str_ty, {size_one});
    const ir::RegisterIdx field0 =
        emit(ir::Opcode::GetElementPtr, builder.primitive(ir::TypeTag::Ptr),
             {to_operand(addr, str_ty), zero_i32, index_operand(0)});
    emit_void(ir::Opcode::Store,
              {to_operand(sub_ref, builder.primitive(ir::TypeTag::Ptr)),
               to_operand(field0, builder.primitive(ir::TypeTag::Ptr))});
    const ir::RegisterIdx field1 =
        emit(ir::Opcode::GetElementPtr, usize_ty,
             {to_operand(addr, str_ty), zero_i32, index_operand(1)});
    emit_void(ir::Opcode::Store,
              {to_operand(width, usize_ty), to_operand(field1, usize_ty)});
    const ir::RegisterIdx loaded =
        emit(ir::Opcode::Load, str_ty, {to_operand(addr, str_ty)});
    return Val{to_operand(loaded, str_ty), str_ty, false, false};
  }
  internal(node.span, "unknown intrinsic");
  return Val{size_one, error_type(), false, false};
}

Val Lowerer::lower_panic(ast::ExprIdx expr) {
  const ast::ExprNode& call = ast.exprs[expr];
  if (call.payload.get<ast::ExprCall>().args.size() != 1) {
    internal(call.span, "intrinsic arity");
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
  Val arg = lower_expr(call.payload.get<ast::ExprCall>().args[0], &str);
  if (failed || terminated_cur()) {
    return stopped_value();
  }
  const Val material = materialize(arg);
  // Fat strings cross the ABI as (bytes, len).
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  const ir::TypeIdx usize_ty = usize_type();
  const ir::RegisterIdx bytes =
      emit(ir::Opcode::ExtractValue, ptr_ty, {material.op, index_operand(0)});
  const ir::RegisterIdx len =
      emit(ir::Opcode::ExtractValue, usize_ty, {material.op, index_operand(1)});

  // `panic` is the one name-based fallback left: it aborts through the
  // runtime. `print` and `println` are io's declarations, expanded by
  // package (ADR-0055).
  emit_panic_registers(call.span, bytes, len);
  return Val{size_one, builder.never_type(), false, false};
}

// The slice behind a slice reference, or invalid when the type is
// not one. The fat struct rides by value everywhere below the type
// checker; the reference wrapper is borrowck-only.
ir::TypeIdx Lowerer::slice_pointee(ir::TypeIdx type) const {
  const ir::TypeTag tag = tag_of(type);
  if (tag != ir::TypeTag::Ref && tag != ir::TypeTag::MutRef) {
    return ir::TypeIdx(base::INVALID_IDX);
  }
  const ir::TypeIdx pointee =
      builder.ref_types()[builder.types()[type].as_ref()].pointee;
  if (tag_of(pointee) != ir::TypeTag::Slice) {
    return ir::TypeIdx(base::INVALID_IDX);
  }
  return pointee;
}

// Whether a parameter wants a fat slice pointer: a reference whose
// pointee is a slice. Array arguments decay into these at call
// boundaries, and slice values already have the shape.
bool Lowerer::is_slice_ref(ir::TypeIdx param) const {
  return slice_pointee(param).is_valid();
}

ir::OperandIdx Lowerer::arg_for(Val arg, ir::TypeIdx param) {
  // A closure value where a bare function type is expected hands over
  // the view, which borrows the environment the value owns.
  if (tag_of(param) == ir::TypeTag::Func &&
      tag_of(arg.type) != ir::TypeTag::Func) {
    return use_value(closure_view(arg));
  }
  if (!is_ref_tag(tag_of(param))) {
    return use_value(arg);
  }
  // Array-to-slice decay: `&[T; N]` where `&[T]` is expected builds the
  // fat pointer, keeping the array's length as the length. The borrow
  // on the array keeps it alive; the stores below carry that loan into
  // the view. A value already of reference-to-array shape builds from
  // itself, carrying the loans its flow already has.
  if (is_slice_ref(param)) {
    if (arg.address && tag_of(arg.type) == ir::TypeTag::Array) {
      const ir::ArrayType& shape =
          builder.state()
              .array_types[builder.state().types[arg.type].as_array()];
      const bool exclusive = tag_of(param) == ir::TypeTag::MutRef;
      const ir::TypeIdx ref = builder.reference_type(arg.type, exclusive);
      const ir::RegisterIdx loan = emit(ir::Opcode::Borrow, ref, {arg.op});
      if (failed) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      const ir::TypeIdx usize_ty = usize_type();
      ir::Immutable imm{.type = usize_ty, .data = {}};
      if (tag_of(usize_ty) == ir::TypeTag::U64) {
        imm.data.u64_value = shape.count;
      } else {
        imm.data.u32_value = static_cast<u32>(shape.count);
      }
      Val fat = build_slice_value(to_operand(loan, ref),
                                  to_operand(builder.immutable(imm), usize_ty),
                                  param, cur_span_);
      if (failed) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
      return fat.op;
    }
    Val refv = arg;
    if (refv.address) {
      refv = materialize(refv);
      if (failed) {
        return ir::OperandIdx(base::INVALID_IDX);
      }
    }
    if (tag_of(refv.type) == ir::TypeTag::Ref ||
        tag_of(refv.type) == ir::TypeTag::MutRef) {
      const ir::TypeIdx pointee =
          builder.ref_types()[builder.types()[refv.type].as_ref()].pointee;
      if (tag_of(pointee) == ir::TypeTag::Array) {
        const ir::ArrayType& shape =
            builder.state()
                .array_types[builder.state().types[pointee].as_array()];
        const ir::TypeIdx usize_ty = usize_type();
        ir::Immutable imm{.type = usize_ty, .data = {}};
        if (tag_of(usize_ty) == ir::TypeTag::U64) {
          imm.data.u64_value = shape.count;
        } else {
          imm.data.u32_value = static_cast<u32>(shape.count);
        }
        Val fat = build_slice_value(
            refv.op, to_operand(builder.immutable(imm), usize_ty), param,
            cur_span_);
        if (failed) {
          return ir::OperandIdx(base::INVALID_IDX);
        }
        return fat.op;
      }
    }
  }
  // A reborrow is implicit at an argument and at a receiver, so the
  // place is loaned here whether or not the source wrote a `&`. Without
  // the instruction the borrow checker has nothing to record, and a
  // `&Self -> &T` accessor hands back a pointer no loan covers.
  const auto loan_place = [&](const Val& place, bool exclusive) {
    const ir::TypeIdx ref = builder.reference_type(place.type, exclusive);
    const ir::RegisterIdx loan = emit(ir::Opcode::Borrow, ref, {place.op});
    return Val{to_operand(loan, ref), ref, false, false};
  };
  if (arg.address) {
    if (is_ref_tag(tag_of(arg.type))) {
      return materialize(arg).op;
    }
    return loan_place(arg, tag_of(param) == ir::TypeTag::MutRef).op;
  }
  if (is_ref_tag(tag_of(arg.type))) {
    return arg.op;
  }
  return loan_place(address_of(arg), tag_of(param) == ir::TypeTag::MutRef).op;
}

Val Lowerer::load_disc(Val slot_addr) {
  const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
  const ir::EnumNiche niche = ir::enum_niche(builder.state(), slot_addr.type);
  if (niche.has) {
    const ir::RegisterIdx disc =
        niche_discriminant(builder, niche, slot_addr.op, i32,
                           [&](ir::Opcode op, ir::TypeIdx type,
                               const std::vector<ir::OperandIdx>& ops) {
                             return emit(op, type, ops);
                           });
    return Val{to_operand(disc, i32), i32, false, false};
  }
  const ir::RegisterIdx gep =
      emit(ir::Opcode::GetElementPtr, i32,
           {slot_addr.op, zero_i32, index_operand(0)}, {.inbounds = true});
  const ir::RegisterIdx loaded =
      emit(ir::Opcode::Load, i32, {to_operand(gep, i32)});
  return Val{to_operand(loaded, i32), i32, false, false};
}

// The operand that spells a niche-carrying enum's fieldless variant:
// a null reference, or the byte above one when the payload is a bool.
ir::OperandIdx Lowerer::niche_sentinel(const ir::EnumNiche& niche) {
  if (niche.slot == niche.payload) {
    return null_reference(niche.payload);
  }
  const ir::TypeIdx u8 = builder.primitive(ir::TypeTag::U8);
  return imm_from_u64(ir::TypeTag::U8, u8, niche.sentinel);
}

// A null reference of the given reference type: the payload's spare
// pattern, which every fieldless variant of a niche-carrying enum
// spells (ADR-0087).
ir::OperandIdx Lowerer::null_reference(ir::TypeIdx ref_type) {
  const ir::RegisterIdx null_ref =
      emit_null_reference(builder, ref_type,
                          [&](ir::Opcode op, ir::TypeIdx type,
                              const std::vector<ir::OperandIdx>& ops) {
                            return emit(op, type, ops);
                          });
  return to_operand(null_ref, ref_type);
}

// Unit payloads (`()` fields) carry no data; construction stores
// nothing and bindings receive a Void value.
bool Lowerer::is_unit_payload(const std::vector<ir::TypeIdx>& payloads) {
  if (payloads.empty()) {
    return false;
  }
  for (ir::TypeIdx field : payloads) {
    if (tag_of(field) != ir::TypeTag::Void) {
      return false;
    }
  }
  return true;
}

Val Lowerer::void_value() {
  return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
}

// Value of payload field i of the enum at slot_addr. The slot holds the
// payload's bytes, so the field address comes from the offset
// ir::field_offset publishes.
Val Lowerer::load_payload_field(Val slot_addr,
                                ir::TypeIdx enum_type,
                                ir::TypeIdxRange fields,
                                u32 field) {
  const ir::TypeIdx field_type = fields[field];
  const ir::EnumNiche niche = ir::enum_niche(builder.state(), enum_type);
  const ir::RegisterIdx field_gep =
      payload_field_addr(slot_addr, enum_type, fields, field);
  if (niche.has && niche.slot != niche.payload) {
    // The slot holds a byte where the payload is a bool: read the byte
    // and narrow it (ADR-0087).
    const ir::RegisterIdx byte =
        emit(ir::Opcode::Load, niche.slot, {to_operand(field_gep, niche.slot)});
    const ir::RegisterIdx narrowed =
        emit(ir::Opcode::TypeCast, field_type, {to_operand(byte, niche.slot)});
    return Val{to_operand(narrowed, field_type), field_type, false, false};
  }
  const ir::RegisterIdx loaded =
      emit(ir::Opcode::Load, field_type, {to_operand(field_gep, field_type)});
  return Val{to_operand(loaded, field_type), field_type, false, false};
}

void Lowerer::emit_br(ir::BlockIdx target) {
  emit_void(ir::Opcode::Br,
            {builder.operand(ir::Operand::from_block(
                target, builder.primitive(ir::TypeTag::Void)))});
}

void Lowerer::emit_cond_br(ir::OperandIdx cond,
                           ir::BlockIdx then_block,
                           ir::BlockIdx else_block) {
  const ir::TypeIdx void_ty = builder.primitive(ir::TypeTag::Void);
  emit_void(
      ir::Opcode::CondBr,
      {cond, builder.operand(ir::Operand::from_block(then_block, void_ty)),
       builder.operand(ir::Operand::from_block(else_block, void_ty))});
}

ir::OperandIdx Lowerer::bool_operand(bool value) {
  const ir::TypeIdx i1 = builder.primitive(ir::TypeTag::I1);
  ir::Immutable imm{.type = i1, .data = {}};
  imm.data.i1_value = value;
  return to_operand(builder.immutable(imm), i1);
}

void Lowerer::emit_panic(diag::Span span, ir::OperandIdx message) {
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  const ir::TypeIdx usize_ty = usize_type();
  const ir::RegisterIdx bytes =
      emit(ir::Opcode::ExtractValue, ptr_ty, {message, index_operand(0)});
  const ir::RegisterIdx len =
      emit(ir::Opcode::ExtractValue, usize_ty, {message, index_operand(1)});
  emit_panic_registers(span, bytes, len);
}

void Lowerer::emit_panic_registers(diag::Span span,
                                   ir::RegisterIdx bytes,
                                   ir::RegisterIdx len) {
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  const ir::TypeIdx usize_ty = usize_type();
  const ir::OperandIdx where = where_operand(span);
  const ir::RegisterIdx where_bytes =
      emit(ir::Opcode::ExtractValue, ptr_ty, {where, index_operand(0)});
  const ir::RegisterIdx where_len =
      emit(ir::Opcode::ExtractValue, usize_ty, {where, index_operand(1)});
  const ir::ExternalFunctionIdx ext = declare_external(
      "alcy_panic", builder.never_type(), {ptr_ty, usize_ty, ptr_ty, usize_ty});
  emit_void(ir::Opcode::Call,
            {builder.operand(ir::Operand::from_external_function(
                 ext, builder.primitive(ir::TypeTag::Function))),
             to_operand(bytes, ptr_ty), to_operand(len, usize_ty),
             to_operand(where_bytes, ptr_ty), to_operand(where_len, usize_ty)});
  emit_void(ir::Opcode::Unreachable, {});
}

ir::OperandIdx Lowerer::where_operand(diag::Span span) {
  if (sources == nullptr || span.file == source::UNKNOWN_FILE) {
    return str_operand("");
  }
  const std::optional<std::string_view> name = sources->name(span.file);
  const std::optional<std::string_view> bytes = sources->bytes(span.file);
  if (!name.has_value() || !bytes.has_value()) {
    return str_operand("");
  }
  const diag::LineColumn at = diag::line_column(*bytes, span.offset);
  const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
  const str::StringPoolId id =
      intern_copied(fmt::format("{}:{}:{}", *name, at.line, at.column));
  const ir::ImmutableIdx imm =
      builder.immutable({.type = str, .data = {.str_id_value = id}});
  return to_operand(imm, str);
}

ir::OperandIdx Lowerer::str_operand(std::string_view message) {
  const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
  const ir::ImmutableIdx imm = builder.immutable(
      {.type = str, .data = {.str_id_value = intern_name(message)}});
  return to_operand(imm, str);
}

Val Lowerer::lower_method_call(ast::ExprIdx expr) {
  const ast::ExprNode& method = ast.exprs[expr];
  const analyzer::CheckedModule::CallTarget* target = call_target(expr);
  if (target == nullptr || !target->is_method) {
    internal(method.span, "method call without target");
    return Val{size_one, error_type(), false, false};
  }
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  const analyzer::CheckedModule::MethodInfo& info = def.methods[target->index];
  // A reference receiver borrows the place it names; anything else
  // evaluates to a value first.
  const ast::ExprIdx receiver_expr =
      method.payload.get<ast::ExprMethodCall>().receiver;
  // A reference receiver borrows the place it names, including a
  // temporary: `place_addr` gives one a slot in this frame, which the
  // drop walk ends at the enclosing block.
  // A range index names a view, not a place: `s[run]` already reaches
  // the run through a reference, so a reference receiver takes that
  // value instead of the address of an element. Reading it as a place
  // built an element access out of a Range value.
  const bool receiver_is_run =
      ast.exprs[receiver_expr].kind == ast::ExprKind::Index &&
      is_range_type(expr_type(
          ast.exprs[receiver_expr].payload.get<ast::ExprIndex>().index));
  Val receiver = is_ref_tag(tag_of(info.params[0])) && !receiver_is_run
                     ? place_addr(receiver_expr)
                     : lower_expr(receiver_expr, nullptr);
  // A by-value receiver that names a field moves it out of its owner,
  // so the owner's drop skips it; a local already carries the marker.
  if (!is_ref_tag(tag_of(info.params[0]))) {
    mark_field_move(receiver_expr);
  }
  if (failed || terminated_cur()) {
    return stopped_value();
  }
  const std::span<const ast::ExprIdx> method_args =
      method.payload.get<ast::ExprMethodCall>().args;
  if (target->module < pkg.tree.modules.size() &&
      pkg.tree.modules[target->module]->is_summary) {
    return lower_summary_call(expr, info.name, info.params, info.ret, info.item,
                              target->module, method_args, receiver);
  }
  const std::vector<u32> comp = comp_positions(info.item);
  if (!comp.empty() && comp[0] == 0) {
    unsupported(method.span, "comp method receiver");
    return Val{size_one, error_type(), false, false};
  }
  std::vector<CompVal> comp_args;
  for (u32 pos : comp) {
    CompVal arg;
    if (!comp_lowering_.evaluate(module, method_args[pos - 1], arg)) {
      return Val{size_one, error_type(), false, false};
    }
    comp_args.push_back(std::move(arg));
  }
  ir::FunctionIdx fn =
      fn_index(target->module, info.item, info.name, info.params, info.ret,
               callee_inst(target), std::move(comp_args),
               ir::SymbolKind::Method, info.method_args, info.self_type);
  if (!fn.is_valid()) {
    return Val{size_one, error_type(), false, false};
  }
  std::vector<ir::OperandIdx> ops;
  ops.push_back(builder.operand(ir::Operand::from_function(
      fn, builder.primitive(ir::TypeTag::Function))));
  ops.push_back(arg_for(receiver, info.params[0]));
  usize comp_at = 0;
  for (usize i = 0; i < method_args.size(); ++i) {
    if (comp_at < comp.size() && comp[comp_at] == i + 1) {
      ++comp_at;
      continue;
    }
    Val arg = lower_moved(method_args[i], &info.params[i + 1]);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    ops.push_back(arg_for(arg, info.params[i + 1]));
  }
  if (tag_of(info.ret) == ir::TypeTag::Void) {
    emit_void(ir::Opcode::Call, ops);
    return Val{size_one, info.ret, false, false};
  }
  const ir::RegisterIdx dst = emit(ir::Opcode::Call, info.ret, ops);
  return Val{to_operand(dst, info.ret), info.ret, false, false};
}

Val Lowerer::lower_spec_index(ast::ExprIdx expr,
                              const analyzer::CheckedModule::CallTarget* target,
                              bool as_place) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprIndex& index = node.payload.get<ast::ExprIndex>();
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  const analyzer::CheckedModule::MethodInfo& info = def.methods[target->index];
  // The canonical declaration has no comp parameter, so the call is an
  // ordinary one.
  if (!comp_positions(info.item).empty()) {
    internal(node.span, "comp index method");
    return Val{size_one, error_type(), as_place, false};
  }
  // A reference receiver borrows the place it names; anything else
  // evaluates to a value first, exactly like a method call.
  Val receiver =
      is_ref_tag(tag_of(info.params[0])) && is_rooted_place(index.receiver)
          ? place_addr(index.receiver)
          : lower_expr(index.receiver, nullptr);
  if (failed) {
    return Val{size_one, error_type(), as_place, false};
  }
  const ir::FunctionIdx fn = fn_index(
      target->module, info.item, info.name, info.params, info.ret,
      callee_inst(target), std::vector<CompVal>{}, ir::SymbolKind::Method,
      /*extra_generics=*/{}, info.self_type);
  if (!fn.is_valid()) {
    return Val{size_one, error_type(), as_place, false};
  }
  Val arg = lower_expr(index.index, &info.params[1]);
  if (failed) {
    return Val{size_one, error_type(), as_place, false};
  }
  std::vector<ir::OperandIdx> ops;
  ops.push_back(builder.operand(ir::Operand::from_function(
      fn, builder.primitive(ir::TypeTag::Function))));
  ops.push_back(arg_for(receiver, info.params[0]));
  ops.push_back(arg_for(arg, info.params[1]));
  const ir::RegisterIdx dst = emit(ir::Opcode::Call, info.ret, ops);
  const ir::TypeTag ret_tag = tag_of(info.ret);
  if (ret_tag != ir::TypeTag::Ref && ret_tag != ir::TypeTag::MutRef) {
    internal(node.span, "index method without a reference result");
    return Val{size_one, error_type(), as_place, false};
  }
  // The returned reference is the element's address, the way a
  // dereference names the place a pointer points at, and the element
  // belongs to whoever owns the receiver.
  const ir::TypeIdx element =
      builder.ref_types()[builder.types()[info.ret.idx].as_ref()].pointee;
  const Val place{to_operand(dst, info.ret), element, true, true, true};
  return as_place ? place : materialize(place);
}

Val Lowerer::emit_spec_call(const analyzer::CheckedModule::CallTarget* target,
                            diag::Span span,
                            Val receiver,
                            Val argument,
                            bool has_argument) {
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  const analyzer::CheckedModule::MethodInfo& info = def.methods[target->index];
  if (!comp_positions(info.item).empty()) {
    internal(span, "comp operator method");
    return Val{size_one, error_type(), false, false};
  }
  const ir::FunctionIdx fn = fn_index(
      target->module, info.item, info.name, info.params, info.ret,
      callee_inst(target), std::vector<CompVal>{}, ir::SymbolKind::Method,
      /*extra_generics=*/{}, info.self_type);
  if (!fn.is_valid()) {
    return Val{size_one, error_type(), false, false};
  }
  std::vector<ir::OperandIdx> ops;
  ops.push_back(builder.operand(ir::Operand::from_function(
      fn, builder.primitive(ir::TypeTag::Function))));
  ops.push_back(arg_for(receiver, info.params[0]));
  if (has_argument) {
    ops.push_back(arg_for(argument, info.params[1]));
  }
  const ir::RegisterIdx dst = emit(ir::Opcode::Call, info.ret, ops);
  return Val{to_operand(dst, info.ret), info.ret, false, false};
}

Val Lowerer::lower_spec_binary(
    ast::ExprIdx expr,
    const analyzer::CheckedModule::CallTarget* target) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprBinary& bin = node.payload.get<ast::ExprBinary>();
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  const analyzer::CheckedModule::MethodInfo& info = def.methods[target->index];
  // `eq` takes both operands by shared reference, so a rooted place is
  // borrowed and anything else is materialized into one; an arithmetic
  // method reads them by value.
  Val receiver = is_ref_tag(tag_of(info.params[0])) && is_rooted_place(bin.lhs)
                     ? place_addr(bin.lhs)
                     : lower_moved(bin.lhs, nullptr);
  if (failed || terminated_cur()) {
    return stopped_value();
  }
  Val argument = is_ref_tag(tag_of(info.params[1])) && is_rooted_place(bin.rhs)
                     ? place_addr(bin.rhs)
                     : lower_moved(bin.rhs, nullptr);
  if (failed || terminated_cur()) {
    return stopped_value();
  }
  const Val result = emit_spec_call(target, node.span, receiver, argument,
                                    /*has_argument=*/true);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  if (bin.op == ast::BinaryOp::NotEq) {
    const ir::RegisterIdx negated =
        emit(ir::Opcode::Not, result.type, {result.op});
    return Val{to_operand(negated, result.type), result.type, false, false};
  }
  return result;
}

Val Lowerer::lower_spec_unary(
    ast::ExprIdx expr,
    const analyzer::CheckedModule::CallTarget* target) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprUnary& unary = node.payload.get<ast::ExprUnary>();
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  const analyzer::CheckedModule::MethodInfo& info = def.methods[target->index];
  Val receiver =
      is_ref_tag(tag_of(info.params[0])) && is_rooted_place(unary.inner)
          ? place_addr(unary.inner)
          : lower_moved(unary.inner, nullptr);
  if (failed || terminated_cur()) {
    return stopped_value();
  }
  const Val argument{size_one, error_type(), false, false};
  return emit_spec_call(target, node.span, receiver, argument,
                        /*has_argument=*/false);
}

Val Lowerer::lower_spec_ordering(
    ast::ExprIdx expr,
    const analyzer::CheckedModule::CallTarget* target,
    ast::BinaryOp op) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprBinary& bin = node.payload.get<ast::ExprBinary>();
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  const analyzer::CheckedModule::MethodInfo& info = def.methods[target->index];
  Val receiver = is_ref_tag(tag_of(info.params[0])) && is_rooted_place(bin.lhs)
                     ? place_addr(bin.lhs)
                     : lower_moved(bin.lhs, nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  Val argument = is_ref_tag(tag_of(info.params[1])) && is_rooted_place(bin.rhs)
                     ? place_addr(bin.rhs)
                     : lower_moved(bin.rhs, nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const Val call = emit_spec_call(target, node.span, receiver, argument,
                                  /*has_argument=*/true);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  // The result is an `Option<Ordering>`: the option's discriminant
  // says whether the pair compares at all, and the payload's own
  // discriminant names the outcome.
  Val slot = call.address ? call : address_of(call);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx option_ty = call.type;
  const ir::TypeIdx option_origin = type_origin(option_ty);
  u32 some = 0;
  if (!variant_index(option_origin, "Some", some)) {
    internal(node.span, "comparison without Option::Some");
    return Val{size_one, error_type(), false, false};
  }
  const std::vector<ir::TypeIdx> payloads =
      variant_payload(option_origin, some);
  if (payloads.size() != 1) {
    internal(node.span, "comparison payload arity");
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx ordering_ty = type_origin(payloads[0]);
  u32 less = 0;
  u32 equal = 0;
  u32 greater = 0;
  if (!variant_index(ordering_ty, "Less", less) ||
      !variant_index(ordering_ty, "Equal", equal) ||
      !variant_index(ordering_ty, "Greater", greater)) {
    internal(node.span, "comparison without an ordering");
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
  const Val tag = load_disc(slot);
  const ir::RegisterIdx is_some =
      emit(ir::Opcode::Eq, boolean, {use_value(tag), disc_operand(some)});
  // The payload area is byte-addressed; an `Ordering` sits at its
  // start, its own discriminant first.
  const ir::TypeIdx i32_ty = builder.primitive(ir::TypeTag::I32);
  const ir::TypeIdx byte_ty = builder.primitive(ir::TypeTag::U8);
  const ir::RegisterIdx ordering_addr = payload_field_addr(
      slot, option_origin, variant_fields(option_origin, some), 0);
  const ir::RegisterIdx disc_addr =
      emit(ir::Opcode::GetElementPtr, byte_ty,
           {to_operand(ordering_addr, byte_ty), index_operand(0)});
  const ir::RegisterIdx disc =
      emit(ir::Opcode::Load, i32_ty, {to_operand(disc_addr, i32_ty)});
  const Val outcome{to_operand(disc, i32_ty), i32_ty, false, false};
  const ir::RegisterIdx is_less =
      emit(ir::Opcode::Eq, boolean, {use_value(outcome), disc_operand(less)});
  const ir::RegisterIdx is_equal =
      emit(ir::Opcode::Eq, boolean, {use_value(outcome), disc_operand(equal)});
  const ir::RegisterIdx is_greater = emit(
      ir::Opcode::Eq, boolean, {use_value(outcome), disc_operand(greater)});
  ir::RegisterIdx within = is_equal;
  switch (op) {
    case ast::BinaryOp::Lt: within = is_less; break;
    case ast::BinaryOp::Gt: within = is_greater; break;
    case ast::BinaryOp::LtEq:
      within =
          emit(ir::Opcode::Or, boolean,
               {to_operand(is_less, boolean), to_operand(is_equal, boolean)});
      break;
    case ast::BinaryOp::GtEq:
      within = emit(
          ir::Opcode::Or, boolean,
          {to_operand(is_greater, boolean), to_operand(is_equal, boolean)});
      break;
    default: break;
  }
  // A `None` makes every comparison false.
  const ir::RegisterIdx result =
      emit(ir::Opcode::And, boolean,
           {to_operand(is_some, boolean), to_operand(within, boolean)});
  return Val{to_operand(result, boolean), boolean, false, false};
}

// Address of `base.name`: sees through references to the nominal
// carrying the field. Works for values and places alike.
Val Lowerer::field_addr(Val base, std::string_view name, diag::Span span) {
  ir::OperandIdx ptr_op = size_one;
  ir::TypeIdx struct_ty = error_type();
  const ir::TypeTag tag = tag_of(base.type);
  // A field of a place reached through a reference is borrowed too: the
  // value it holds belongs to whoever owns the referent.
  bool borrowed = base.borrowed;
  if (base.address && !is_ref_tag(tag)) {
    ptr_op = base.op;
    struct_ty = base.type;
  } else if (base.address || is_ref_tag(tag)) {
    const Val material = materialize(base);
    ptr_op = material.op;
    struct_ty = builder.state()
                    .ref_types[builder.state().types[base.type].as_ref()]
                    .pointee;
    if (tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef) {
      borrowed = true;
    }
  } else {
    const Val spilled = address_of(base);
    ptr_op = spilled.op;
    struct_ty = base.type;
  }
  u32 index = 0;
  if (tag_of(struct_ty) == ir::TypeTag::Tuple) {
    bool digits = !name.empty();
    for (char c : name) {
      if (c < '0' || c > '9') {
        digits = false;
        break;
      }
      index = index * 10 + static_cast<u32>(c - '0');
    }
    if (!digits) {
      internal(span, "field without declaration");
      return Val{size_one, error_type(), true, false};
    }
  } else if (!struct_field_index(struct_ty, name, index)) {
    internal(span, "field without declaration");
    return Val{size_one, error_type(), true, false};
  }
  const ir::TypeIdx field_type = field_type_of(struct_ty, index, span);
  const ir::RegisterIdx gep =
      emit(ir::Opcode::GetElementPtr, field_type,
           {ptr_op, zero_i32, index_operand(index)}, {.inbounds = true});
  return Val{to_operand(gep, field_type), field_type, true, base.place,
             borrowed};
}

Val Lowerer::lower_struct(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ir::TypeIdx struct_type = expr_type(expr);
  if (tag_of(struct_type) != ir::TypeTag::Struct) {
    internal(node.span, "struct without type");
    return Val{size_one, error_type(), false, false};
  }
  const ir::RegisterIdx addr =
      emit(ir::Opcode::Alloca, struct_type, {size_one});
  u32 field_count = 0;
  // The same table `struct_field_index` reads: a structure's shape is recorded
  // where the structure is, so the walk this replaced asked the package how
  // many fields one structure has.
  const auto shape = structs_.find(struct_type.idx);
  if (shape != structs_.end()) {
    field_count = static_cast<u32>(shape->second->fields.size());
  }
  std::vector<bool> seen(field_count, false);
  for (const ast::ExprFieldInit& field :
       node.payload.get<ast::ExprStruct>().init) {
    u32 index = 0;
    if (!struct_field_index(struct_type, field.name.name, index)) {
      internal(field.name.span, "field without declaration");
      return Val{size_one, error_type(), false, false};
    }
    seen[index] = true;
    const ir::TypeIdx element_type =
        field_type_of(struct_type, index, ast.exprs[field.value].span);
    Val value = lower_moved(field.value, &element_type);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const ir::RegisterIdx gep =
        emit(ir::Opcode::GetElementPtr, element_type,
             {to_operand(addr, struct_type), zero_i32, index_operand(index)});
    emit_void(ir::Opcode::Store,
              {use_value(value), to_operand(gep, element_type)});
  }
  if (node.payload.get<ast::ExprStruct>().base_expr.is_valid()) {
    Val base =
        lower_expr(node.payload.get<ast::ExprStruct>().base_expr, nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    Val base_addr = address_of(base);
    // Each base field ends in its own place: one the initializer did
    // not write moves into the result, and one it did is dropped where
    // it stands. The base is then fully spent, so its own drop has
    // nothing left to end.
    const bool base_place = base_addr.place;
    for (u32 i = 0; i < static_cast<u32>(seen.size()); ++i) {
      const ir::TypeIdx element_type = field_type_of(struct_type, i, node.span);
      const ir::RegisterIdx src =
          emit(ir::Opcode::GetElementPtr, element_type,
               {base_addr.op, zero_i32, index_operand(i)}, {.inbounds = true});
      const ir::OperandIdx src_op = to_operand(src, element_type);
      if (seen[i]) {
        // The initializer wrote this field, so the base's copy ends here.
        if (runs_destructor(element_type)) {
          record_drop_in_place(src_op, element_type, node.span);
        }
        continue;
      }
      const ir::RegisterIdx loaded =
          emit(ir::Opcode::Load, element_type, {src_op});
      const ir::RegisterIdx dst =
          emit(ir::Opcode::GetElementPtr, element_type,
               {to_operand(addr, struct_type), zero_i32, index_operand(i)},
               {.inbounds = true});
      emit_void(ir::Opcode::Store, {to_operand(loaded, element_type),
                                    to_operand(dst, element_type)});
      // The field moves out of the base into the result.
      mark_move(Val{src_op, element_type, true, base_place});
    }
  }
  return Val{to_operand(addr, struct_type), struct_type, true, false};
}

Val Lowerer::lower_tuple(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprTuple& tuple = node.payload.get<ast::ExprTuple>();
  if (tuple.elements.empty()) {
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }
  const ir::TypeIdx tuple_type = expr_type(expr);
  if (tag_of(tuple_type) != ir::TypeTag::Tuple) {
    internal(node.span, "tuple without type");
    return Val{size_one, error_type(), false, false};
  }
  // A copy, not a reference: lowering an element can intern a type and
  // grow the tuple table under it.
  const ir::TupleType shape =
      builder.state().tuple_types[builder.state().types[tuple_type].as_tuple()];
  const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, tuple_type, {size_one});
  for (u32 i = 0; i < static_cast<u32>(tuple.elements.size()) && !failed; ++i) {
    const ir::TypeIdx* element_expected = nullptr;
    ir::TypeIdx element_type = error_type();
    if (i < shape.elements.size()) {
      element_type = shape.elements[i];
      element_expected = &element_type;
    }
    Val value = lower_moved(tuple.elements[i], element_expected);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const ir::TypeIdx stored =
        i < shape.elements.size() ? shape.elements[i] : value.type;
    const ir::RegisterIdx gep =
        emit(ir::Opcode::GetElementPtr, stored,
             {to_operand(addr, tuple_type), zero_i32, index_operand(i)});
    emit_void(ir::Opcode::Store, {use_value(value), to_operand(gep, stored)});
  }
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  return Val{to_operand(addr, tuple_type), tuple_type, true, false};
}

Val Lowerer::lower_array(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprArray& array = node.payload.get<ast::ExprArray>();
  const ir::TypeIdx array_type = expr_type(expr);
  if (tag_of(array_type) != ir::TypeTag::Array) {
    internal(node.span, "array without type");
    return Val{size_one, error_type(), false, false};
  }
  // A copy, not a reference: lowering an element can intern a type and
  // grow the array table under it.
  const ir::ArrayType shape =
      builder.state().array_types[builder.state().types[array_type].as_array()];
  const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, array_type, {size_one});
  if (array.repeat.is_valid()) {
    if (shape.count == 0) {
      return Val{to_operand(addr, array_type), array_type, true, false};
    }
    Val value = lower_moved(array.repeat, &shape.element);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const ir::OperandIdx stored = use_value(value);
    for (u64 i = 0; i < shape.count && !failed; ++i) {
      const ir::RegisterIdx gep = emit(ir::Opcode::GetElementPtr, shape.element,
                                       {to_operand(addr, array_type), zero_i32,
                                        index_operand(static_cast<u32>(i))},
                                       {.inbounds = true});
      emit_void(ir::Opcode::Store, {stored, to_operand(gep, shape.element)});
    }
  } else {
    if (shape.count != array.elements.size()) {
      internal(node.span, "array arity");
      return Val{size_one, error_type(), false, false};
    }
    for (u32 i = 0; i < static_cast<u32>(array.elements.size()) && !failed;
         ++i) {
      Val value = lower_moved(array.elements[i], &shape.element);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      const ir::RegisterIdx gep =
          emit(ir::Opcode::GetElementPtr, shape.element,
               {to_operand(addr, array_type), zero_i32, index_operand(i)},
               {.inbounds = true});
      emit_void(ir::Opcode::Store,
                {use_value(value), to_operand(gep, shape.element)});
    }
  }
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  return Val{to_operand(addr, array_type), array_type, true, false};
}

ir::Opcode Lowerer::int_binop(ast::BinaryOp op, ir::TypeTag tag) {
  switch (op) {
    case ast::BinaryOp::Add: return ir::Opcode::IntAdd;
    case ast::BinaryOp::Sub: return ir::Opcode::IntSub;
    case ast::BinaryOp::Mul: return ir::Opcode::IntMul;
    case ast::BinaryOp::Div:
      return tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
                     tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64
                 ? ir::Opcode::UintDiv
                 : ir::Opcode::IntDiv;
    case ast::BinaryOp::Mod:
      return tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
                     tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64
                 ? ir::Opcode::UintRem
                 : ir::Opcode::IntRem;
    case ast::BinaryOp::BitAnd: return ir::Opcode::And;
    case ast::BinaryOp::BitOr: return ir::Opcode::Or;
    case ast::BinaryOp::BitXor: return ir::Opcode::Xor;
    case ast::BinaryOp::Shl: return ir::Opcode::ShiftLeft;
    case ast::BinaryOp::Shr:
      return tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
                     tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64
                 ? ir::Opcode::LogicalShiftRight
                 : ir::Opcode::ArithmeticShiftRight;
    default: return ir::Opcode::Noop;
  }
}

Val Lowerer::lower_int_division(diag::Span span,
                                ast::BinaryOp op,
                                Val lhs,
                                Val rhs,
                                ir::TypeTag tag) {
  const bool unsigned_op = tag == ir::TypeTag::U8 || tag == ir::TypeTag::U16 ||
                           tag == ir::TypeTag::U32 || tag == ir::TypeTag::U64;
  const ir::Opcode div_op = int_binop(op, tag);
  const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
  const ir::OperandIdx zero = int_operand(0, lhs.type);
  // A zero divisor panics here rather than reaching a backend's
  // division by zero, which would be undefined and differ per target.
  const ir::RegisterIdx nonzero =
      emit(ir::Opcode::Ne, boolean, {use_value(rhs), zero});
  const ir::BlockIdx ok_block = reserve_block();
  const ir::BlockIdx bad_block = reserve_block();
  emit_cond_br(to_operand(nonzero, boolean), ok_block, bad_block);
  switch_to(bad_block);
  emit_panic(span, str_operand("division by zero"));
  switch_to(ok_block);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  if (unsigned_op) {
    const ir::RegisterIdx dst =
        emit(div_op, lhs.type, {use_value(lhs), use_value(rhs)});
    return Val{to_operand(dst, lhs.type), lhs.type, false, false};
  }
  // `MIN / -1` would overflow; the language wraps it, matching
  // compile-time evaluation, so the divisor rides as one and the
  // wrapping negation (or the zero remainder) is selected instead.
  const ir::OperandIdx minus_one = int_operand(~static_cast<u64>(0), lhs.type);
  const ir::RegisterIdx negated =
      emit(ir::Opcode::Eq, boolean, {use_value(rhs), minus_one});
  const ir::RegisterIdx safe = emit(
      ir::Opcode::Select, lhs.type,
      {to_operand(negated, boolean), int_operand(1, lhs.type), use_value(rhs)});
  const ir::RegisterIdx quotient =
      emit(div_op, lhs.type, {use_value(lhs), to_operand(safe, lhs.type)});
  const ir::OperandIdx wrapped =
      op == ast::BinaryOp::Div
          ? to_operand(
                emit(ir::Opcode::IntSubWrap, lhs.type, {zero, use_value(lhs)}),
                lhs.type)
          : zero;
  const ir::RegisterIdx result = emit(
      ir::Opcode::Select, lhs.type,
      {to_operand(negated, boolean), wrapped, to_operand(quotient, lhs.type)});
  return Val{to_operand(result, lhs.type), lhs.type, false, false};
}

// The element-wise comparison the checker admitted for == and != on a
// tuple or array. A leaf is a scalar or a thin address the backend
// compares; an aggregate recurses over its fields or elements, and
// every leaf comparison has to hold.
Val Lowerer::emit_element_eq(Val lhs, Val rhs, diag::Span span) {
  const ir::TypeTag tag = tag_of(lhs.type);
  const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
  if (tag != ir::TypeTag::Tuple && tag != ir::TypeTag::Array) {
    const ir::RegisterIdx cmp =
        emit(ir::Opcode::Eq, boolean, {use_value(lhs), use_value(rhs)});
    return Val{to_operand(cmp, boolean), boolean, false, false};
  }
  const Val left = address_of(lhs);
  const Val right = address_of(rhs);
  if (failed) {
    return Val{size_one, boolean, false, false};
  }
  const bool is_array = tag == ir::TypeTag::Array;
  const u32 count =
      is_array
          ? static_cast<u32>(
                builder.state()
                    .array_types[builder.state().types[lhs.type].as_array()]
                    .count)
          : static_cast<u32>(
                builder.state()
                    .tuple_types[builder.state().types[lhs.type].as_tuple()]
                    .elements.size());
  ir::OperandIdx result = bool_operand(true);
  for (u32 i = 0; i < count && !failed; ++i) {
    const ir::TypeIdx field =
        is_array ? builder.state()
                       .array_types[builder.state().types[lhs.type].as_array()]
                       .element
                 : builder.state()
                       .tuple_types[builder.state().types[lhs.type].as_tuple()]
                       .elements[i];
    const ir::RegisterIdx left_field =
        emit(ir::Opcode::GetElementPtr, field,
             {left.op, zero_i32, index_operand(i)}, {.inbounds = true});
    const ir::RegisterIdx right_field =
        emit(ir::Opcode::GetElementPtr, field,
             {right.op, zero_i32, index_operand(i)}, {.inbounds = true});
    if (failed) {
      break;
    }
    const Val left_val{to_operand(left_field, field), field, true, left.place};
    const Val right_val{to_operand(right_field, field), field, true,
                        right.place};
    const Val cmp = emit_element_eq(left_val, right_val, span);
    if (failed) {
      break;
    }
    const ir::RegisterIdx both =
        emit(ir::Opcode::And, boolean, {result, cmp.op});
    result = to_operand(both, boolean);
  }
  return Val{result, boolean, false, false};
}

Val Lowerer::lower_binary(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprBinary& bin = node.payload.get<ast::ExprBinary>();
  if (const analyzer::CheckedModule::CallTarget* target = call_target(expr);
      target != nullptr) {
    if (bin.op == ast::BinaryOp::Gt || bin.op == ast::BinaryOp::Lt ||
        bin.op == ast::BinaryOp::GtEq || bin.op == ast::BinaryOp::LtEq) {
      return lower_spec_ordering(expr, target, bin.op);
    }
    return lower_spec_binary(expr, target);
  }
  if (bin.op == ast::BinaryOp::Pow) {
    unsupported(node.span, "power operator");
    return Val{size_one, error_type(), false, false};
  }
  // `&&` and `||` are control flow: the right side runs only when the
  // left did not decide. A result slot takes whichever branch ran.
  if (bin.op == ast::BinaryOp::And || bin.op == ast::BinaryOp::Or) {
    const bool is_and = bin.op == ast::BinaryOp::And;
    Val lhs = lower_expr(bin.lhs, nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
    Val slot = result_slot(boolean, node.span);
    store_result(slot, lhs);
    const ir::BlockIdx rhs_block = reserve_block();
    const ir::BlockIdx end_block = reserve_block();
    emit_cond_br(use_value(lhs), is_and ? rhs_block : end_block,
                 is_and ? end_block : rhs_block);
    switch_to(rhs_block);
    Val rhs = lower_expr(bin.rhs, nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    store_result(slot, rhs);
    emit_br(end_block);
    switch_to(end_block);
    return materialize(slot);
  }
  // A bare literal defaults its width; lower the non-literal
  // side first so literals coerce to it (checking unified them).
  const bool lhs_literal = ast.exprs[bin.lhs].kind == ast::ExprKind::Literal;
  const bool rhs_literal = ast.exprs[bin.rhs].kind == ast::ExprKind::Literal;
  Val lhs = Val{size_one, error_type(), false, false};
  Val rhs = Val{size_one, error_type(), false, false};
  if (!lhs_literal && !rhs_literal) {
    lhs = lower_moved(bin.lhs, nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    rhs = lower_moved(bin.rhs, nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
  } else if (!lhs_literal) {
    lhs = lower_moved(bin.lhs, nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    rhs = lower_expr(bin.rhs, &lhs.type);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
  } else if (!rhs_literal) {
    rhs = lower_moved(bin.rhs, nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    lhs = lower_expr(bin.lhs, &rhs.type);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
  } else {
    // Both sides are literals, so neither carries the unified type: the
    // checker's result is the only source for it. Lowering the left
    // with no hint would take its own default (`1 + 2i64` would narrow
    // to i32) and emit an operand the return type cannot hold.
    const ir::TypeIdx unified = expr_type(expr);
    lhs = lower_expr(bin.lhs, &unified);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    rhs = lower_expr(bin.rhs, &unified);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
  }
  const ir::TypeTag tag = tag_of(lhs.type);
  // The checker admits == and != on a tuple or array when its elements
  // compare directly; the walk below is that comparison.
  if ((bin.op == ast::BinaryOp::Eq || bin.op == ast::BinaryOp::NotEq) &&
      (tag == ir::TypeTag::Tuple || tag == ir::TypeTag::Array)) {
    Val equal = emit_element_eq(lhs, rhs, node.span);
    if (failed) {
      return stopped_value();
    }
    if (bin.op == ast::BinaryOp::NotEq) {
      const ir::RegisterIdx negated =
          emit(ir::Opcode::Not, equal.type, {equal.op});
      equal = Val{to_operand(negated, equal.type), equal.type, false, false};
    }
    return equal;
  }
  if (bin.op == ast::BinaryOp::Eq || bin.op == ast::BinaryOp::NotEq ||
      bin.op == ast::BinaryOp::Gt || bin.op == ast::BinaryOp::Lt ||
      bin.op == ast::BinaryOp::GtEq || bin.op == ast::BinaryOp::LtEq) {
    ir::Opcode op = ir::Opcode::Eq;
    switch (bin.op) {
      case ast::BinaryOp::Eq: op = ir::Opcode::Eq; break;
      case ast::BinaryOp::NotEq: op = ir::Opcode::Ne; break;
      case ast::BinaryOp::Gt: op = ir::Opcode::Gt; break;
      case ast::BinaryOp::Lt: op = ir::Opcode::Lt; break;
      case ast::BinaryOp::GtEq: op = ir::Opcode::Ge; break;
      default: op = ir::Opcode::Le; break;
    }
    const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
    const ir::RegisterIdx dst =
        emit(op, boolean, {use_value(lhs), use_value(rhs)});
    return Val{to_operand(dst, boolean), boolean, false, false};
  }
  if (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64) {
    ir::Opcode op = ir::Opcode::FDiv;
    switch (bin.op) {
      case ast::BinaryOp::Add: op = ir::Opcode::FAdd; break;
      case ast::BinaryOp::Sub: op = ir::Opcode::FSub; break;
      case ast::BinaryOp::Mul: op = ir::Opcode::FMul; break;
      case ast::BinaryOp::Div: op = ir::Opcode::FDiv; break;
      default:
        // Checking admits only these four on a float; anything else
        // reaching here would silently become a division.
        unsupported(node.span, "float operator");
        return Val{size_one, error_type(), false, false};
    }
    const ir::RegisterIdx dst =
        emit(op, lhs.type, {use_value(lhs), use_value(rhs)});
    return Val{to_operand(dst, lhs.type), lhs.type, false, false};
  }
  // A shift count rides modulo the type's width, so an overshift has
  // one result instead of a backend-defined one.
  if (bin.op == ast::BinaryOp::Shl || bin.op == ast::BinaryOp::Shr) {
    const ir::RegisterIdx amount =
        emit(ir::Opcode::And, lhs.type,
             {use_value(rhs), int_operand(integer_bits(tag) - 1, lhs.type)});
    const ir::RegisterIdx dst =
        emit(int_binop(bin.op, tag), lhs.type,
             {use_value(lhs), to_operand(amount, lhs.type)});
    return Val{to_operand(dst, lhs.type), lhs.type, false, false};
  }
  if (bin.op == ast::BinaryOp::Div || bin.op == ast::BinaryOp::Mod) {
    return lower_int_division(node.span, bin.op, lhs, rhs, tag);
  }
  const ir::Opcode op = int_binop(bin.op, tag);
  const ir::RegisterIdx dst =
      emit(op, lhs.type, {use_value(lhs), use_value(rhs)});
  return Val{to_operand(dst, lhs.type), lhs.type, false, false};
}

// Stores an arm value into a result slot (callers skip Void).
void Lowerer::store_result(Val slot, Val value) {
  emit_void(ir::Opcode::Store, {use_value(value), slot.op});
}

// Result slot for joining control-flow values (none for Void or
// Never, whose arms all diverge).
Val Lowerer::result_slot(ir::TypeIdx type, diag::Span span) {
  if (tag_of(type) == ir::TypeTag::Void || tag_of(type) == ir::TypeTag::Never) {
    return Val{size_one, type, false, false};
  }
  const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, type, {size_one});
  (void)span;
  return Val{to_operand(addr, type), type, true, false};
}

// Tests one match arm against a scrutinee address. On success the
// pattern binds at the start of body_block and lowering continues
// there; on failure control jumps to fail_block. Or-patterns expand
// into sibling arms beforehand.
// Whether bind_pattern alone can place a pattern: an irrefutable
// binding, a wildcard, or a destructuring of those. A literal, a
// variant, or an alternative needs a test instead.
// Whether a match scrutinee reads through a reference: a place
// reached by a dereference holds borrowed content.
bool Lowerer::scrutinee_borrowed(ast::ExprIdx expr) const {
  const ast::ExprNode& node = ast.exprs[expr];
  switch (node.kind) {
    case ast::ExprKind::Deref: return true;
    case ast::ExprKind::Field:
      return scrutinee_borrowed(node.payload.get<ast::ExprField>().receiver);
    case ast::ExprKind::Index:
      return scrutinee_borrowed(node.payload.get<ast::ExprIndex>().receiver);
    default: return false;
  }
}

bool Lowerer::pattern_bindable(ast::PatternIdx pattern) const {
  const ast::PatternNode& node = ast.patterns[pattern];
  switch (node.kind) {
    case ast::PatternKind::Wildcard:
    case ast::PatternKind::Ident:
    case ast::PatternKind::MutIdent: return true;
    case ast::PatternKind::Ref: return pattern_bindable(node.payload.ref.inner);
    case ast::PatternKind::Tuple: {
      if (node.payload.tuple.path.is_valid()) {
        return false;
      }
      for (ast::PatternIdx element : node.payload.tuple.elements) {
        if (!pattern_bindable(element)) {
          return false;
        }
      }
      return true;
    }
    case ast::PatternKind::Struct: {
      for (const ast::FieldPattern& field : node.payload.strukt.fields) {
        if (!pattern_bindable(field.pattern)) {
          return false;
        }
      }
      return true;
    }
    case ast::PatternKind::Literal:
    case ast::PatternKind::Or: return false;
  }
  return false;
}

// Tests a run of sub-patterns against their places in order. On the
// first failure it branches to fail; when every test passes it
// branches to body. The caller reaches this in a block that already
// computed the places, and each test runs in the block the previous
// one left current.
void Lowerer::lower_arm_sequence(
    const std::span<const ast::PatternIdx>& patterns,
    const std::vector<Val>& places,
    ir::BlockIdx body,
    ir::BlockIdx fail) {
  for (usize i = 0; i < patterns.size() && !failed; ++i) {
    const ir::BlockIdx next = i + 1 == patterns.size() ? body : reserve_block();
    // A projected field carries a contiguity copy; the test needs the
    // type it stands for (the enum a variant names, the pointee a
    // reference pattern reads).
    const ir::TypeIdx type = type_origin(places[i].type);
    lower_arm_test(patterns[i],
                   Val{places[i].op, type, places[i].address, places[i].place},
                   type, next, fail);
  }
  if (patterns.empty() && !failed) {
    emit_br(body);
    switch_to(body);
  }
}

void Lowerer::lower_arm_test(ast::PatternIdx pattern,
                             Val scrut_addr,
                             ir::TypeIdx scrut_type,
                             ir::BlockIdx body_block,
                             ir::BlockIdx fail_block) {
  const ast::PatternNode& node = ast.patterns[pattern];
  const diag::Span span = node.span;
  // A variant pattern against a reference scrutinee reads through it:
  // the reference is loaded and the variant is matched in what it names.
  if (node.kind == ast::PatternKind::Tuple &&
      node.payload.tuple.path.is_valid()) {
    const ir::TypeTag scrutinee_tag = tag_of(scrut_type);
    if (scrutinee_tag == ir::TypeTag::Ref ||
        scrutinee_tag == ir::TypeTag::MutRef) {
      const ir::TypeIdx pointee =
          builder.state()
              .ref_types[builder.state().types[scrut_type].as_ref()]
              .pointee;
      Val pointer = materialize(scrut_addr);
      if (failed) {
        return;
      }
      scrut_addr = Val{pointer.op, pointee, true, true};
      scrut_type = pointee;
    }
  }
  switch (node.kind) {
    case ast::PatternKind::Wildcard:
      emit_br(body_block);
      switch_to(body_block);
      return;
    case ast::PatternKind::Ident:
    case ast::PatternKind::MutIdent: {
      if (arm_borrowed_) {
        // A field behind a reference is borrowed, not moved: the
        // place address is already the reference value. The operand
        // carries the place's own type, so it restamps itself as the
        // reference: a backend that reads the operand's type stores
        // the pointer's width, not the pointee's.
        const ir::TypeIdx ref_ty = builder.reference_type(scrut_type, false);
        ir::OperandIdx value = scrut_addr.op;
        const ir::Operand& named = builder.state().operands[value.idx];
        if (named.is<ir::RegisterIdx>()) {
          value = to_operand(named.as_register(), ref_ty);
        }
        bind_pattern(pattern, Val{value, ref_ty, false, false});
        if (failed) {
          return;
        }
        emit_br(body_block);
        switch_to(body_block);
        return;
      }
      bind_pattern(pattern, materialize(scrut_addr));
      if (failed) {
        return;
      }
      emit_br(body_block);
      switch_to(body_block);
      return;
    }
    case ast::PatternKind::Literal: {
      Val expected = lower_literal(node.payload.literal.value, &scrut_type);
      if (failed) {
        return;
      }
      Val actual = materialize(scrut_addr);
      const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
      const ir::RegisterIdx test =
          emit(ir::Opcode::Eq, boolean, {actual.op, expected.op});
      emit_cond_br(to_operand(test, boolean), body_block, fail_block);
      switch_to(body_block);
      return;
    }
    case ast::PatternKind::Tuple: {
      if (!node.payload.tuple.path.is_valid()) {
        // Plain tuple destructuring (checking validated shape).
        const ir::TupleType& shape =
            builder.state()
                .tuple_types[builder.state().types[scrut_type].as_tuple()];
        const std::span<const ast::PatternIdx> elements =
            node.payload.tuple.elements;
        bool bindable = true;
        bool has_or = false;
        for (ast::PatternIdx element : elements) {
          bindable = bindable && pattern_bindable(element);
          has_or = has_or || ast.patterns[element].kind == ast::PatternKind::Or;
        }
        if ((bindable || has_or) && !arm_borrowed_) {
          // Every field is a binding, or an alternative still needs
          // its nesting diagnostic from bind_pattern.
          for (u32 i = 0; i < static_cast<u32>(elements.size()) && !failed;
               ++i) {
            if (arm_borrowed_ &&
                ast.patterns[elements[i]].kind == ast::PatternKind::Wildcard) {
              // A borrowed field is the owner to end and nothing to
              // bind.
              continue;
            }
            const ir::RegisterIdx gep =
                emit(ir::Opcode::GetElementPtr, shape.elements[i],
                     {scrut_addr.op, zero_i32, index_operand(i)},
                     {.inbounds = true});
            bind_pattern(elements[i],
                         Val{to_operand(gep, shape.elements[i]),
                             shape.elements[i], true, scrut_addr.place});
            if (failed) {
              return;
            }
          }
          emit_br(body_block);
          switch_to(body_block);
          return;
        }
        // A refutable field is a test, not a binding: project every
        // field first and chain the tests to the fail block.
        std::vector<Val> places;
        places.reserve(elements.size());
        for (u32 i = 0; i < static_cast<u32>(elements.size()) && !failed; ++i) {
          const ir::RegisterIdx gep = emit(
              ir::Opcode::GetElementPtr, shape.elements[i],
              {scrut_addr.op, zero_i32, index_operand(i)}, {.inbounds = true});
          places.push_back(Val{to_operand(gep, shape.elements[i]),
                               shape.elements[i], true, scrut_addr.place});
        }
        if (failed) {
          return;
        }
        lower_arm_sequence(elements, places, body_block, fail_block);
        return;
      }
      if (ast.paths[node.payload.tuple.path].segments.empty()) {
        internal(span, "variant without name");
        return;
      }
      u32 variant = 0;
      if (!variant_index(
              scrut_type,
              ast.paths[node.payload.tuple.path].segments.back().name,
              variant)) {
        internal(span, "variant without declaration");
        return;
      }
      const std::vector<ir::TypeIdx> payloads =
          variant_payload(scrut_type, variant);
      const std::span<const ast::PatternIdx> elements =
          node.payload.tuple.elements;
      if (payloads.size() != elements.size()) {
        internal(span, "variant arity");
        return;
      }
      bool bindable = true;
      bool has_or = false;
      for (ast::PatternIdx element : elements) {
        bindable = bindable && pattern_bindable(element);
        has_or = has_or || ast.patterns[element].kind == ast::PatternKind::Or;
      }
      // A refutable payload tests in a block of its own; an irrefutable
      // one binds straight into the body, as before.
      const bool test_payloads = !is_unit_payload(payloads) &&
                                 (arm_borrowed_ || (!bindable && !has_or));
      const ir::BlockIdx bind_block =
          test_payloads ? reserve_block() : body_block;
      Val tag = load_disc(scrut_addr);
      const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
      const ir::RegisterIdx test =
          emit(ir::Opcode::Eq, boolean, {tag.op, disc_operand(variant)});
      emit_cond_br(to_operand(test, boolean), bind_block, fail_block);
      switch_to(bind_block);
      if (is_unit_payload(payloads)) {
        for (usize i = 0; i < payloads.size() && !failed; ++i) {
          bind_pattern(elements[i], void_value());
        }
        return;
      }
      const ir::TypeIdxRange fields = variant_fields(scrut_type, variant);
      if (!test_payloads) {
        for (usize i = 0; i < payloads.size() && !failed; ++i) {
          if (arm_borrowed_ &&
              ast.patterns[elements[i]].kind == ast::PatternKind::Wildcard) {
            // A borrowed scrutinee keeps its payloads; a wildcard binds
            // nothing to load and nothing to end.
            continue;
          }
          if (ast.patterns[elements[i]].kind == ast::PatternKind::Wildcard &&
              runs_destructor(fields[i])) {
            // The arm discards this payload: end it in place rather than
            // loading a copy nothing owns.
            const ir::RegisterIdx addr = payload_field_addr(
                scrut_addr, scrut_type, fields, static_cast<u32>(i));
            record_drop_in_place(to_operand(addr, fields[i]), fields[i], span);
            continue;
          }
          Val field = load_payload_field(scrut_addr, scrut_type, fields,
                                         static_cast<u32>(i));
          bind_pattern(elements[i], field);
        }
        return;
      }
      // Refutable payloads: project every field first, test in turn, and
      // end a discarded destructible payload once the arm commits, so a
      // later failure never ends a payload the scrutinee still owns.
      std::vector<ir::RegisterIdx> addrs;
      std::vector<Val> places;
      std::vector<bool> discard;
      addrs.reserve(payloads.size());
      places.reserve(payloads.size());
      discard.reserve(payloads.size());
      for (u32 i = 0; i < static_cast<u32>(payloads.size()) && !failed; ++i) {
        const ir::RegisterIdx addr =
            payload_field_slot_addr(scrut_addr, scrut_type, fields, i);
        addrs.push_back(addr);
        discard.push_back(!arm_borrowed_ &&
                          ast.patterns[elements[i]].kind ==
                              ast::PatternKind::Wildcard &&
                          runs_destructor(payloads[i]));
        places.push_back(Val{to_operand(addr, payloads[i]), payloads[i], true,
                             scrut_addr.place});
      }
      if (failed) {
        return;
      }
      bool any_discard = false;
      for (bool d : discard) {
        any_discard = any_discard || d;
      }
      const ir::BlockIdx commit = any_discard ? reserve_block() : body_block;
      lower_arm_sequence(elements, places, commit, fail_block);
      if (any_discard && !failed) {
        switch_to(commit);
        for (u32 i = 0; i < static_cast<u32>(payloads.size()); ++i) {
          if (discard[i]) {
            record_drop_in_place(to_operand(addrs[i], payloads[i]), payloads[i],
                                 span);
          }
        }
        emit_br(body_block);
        switch_to(body_block);
      }
      return;
    }
    case ast::PatternKind::Struct: {
      const std::span<const ast::FieldPattern> fields =
          node.payload.strukt.fields;
      bool bindable = true;
      bool has_or = false;
      for (const ast::FieldPattern& field : fields) {
        bindable = bindable && pattern_bindable(field.pattern);
        has_or =
            has_or || ast.patterns[field.pattern].kind == ast::PatternKind::Or;
      }
      if ((bindable || has_or) && !arm_borrowed_) {
        for (const ast::FieldPattern& field : fields) {
          if (arm_borrowed_ &&
              ast.patterns[field.pattern].kind == ast::PatternKind::Wildcard) {
            // A borrowed field is the owner to end and nothing to bind.
            continue;
          }
          u32 index = 0;
          if (!struct_field_index(scrut_type, field.name.name, index)) {
            internal(field.name.span, "pattern field without declaration");
            return;
          }
          const ir::TypeIdx field_type = field_type_of(scrut_type, index, span);
          const ir::RegisterIdx gep =
              emit(ir::Opcode::GetElementPtr, field_type,
                   {scrut_addr.op, zero_i32, index_operand(index)});
          bind_pattern(field.pattern, Val{to_operand(gep, field_type),
                                          field_type, true, scrut_addr.place});
          if (failed) {
            return;
          }
        }
        emit_br(body_block);
        switch_to(body_block);
        return;
      }
      std::vector<ast::PatternIdx> patterns;
      std::vector<Val> places;
      patterns.reserve(fields.size());
      places.reserve(fields.size());
      for (const ast::FieldPattern& field : fields) {
        u32 index = 0;
        if (!struct_field_index(scrut_type, field.name.name, index)) {
          internal(field.name.span, "pattern field without declaration");
          return;
        }
        const ir::TypeIdx field_type = field_type_of(scrut_type, index, span);
        const ir::RegisterIdx gep =
            emit(ir::Opcode::GetElementPtr, field_type,
                 {scrut_addr.op, zero_i32, index_operand(index)});
        patterns.push_back(field.pattern);
        places.push_back(Val{to_operand(gep, field_type), field_type, true,
                             scrut_addr.place});
      }
      if (failed) {
        return;
      }
      lower_arm_sequence(patterns, places, body_block, fail_block);
      return;
    }
    case ast::PatternKind::Ref: {
      Val loaded = materialize(scrut_addr);
      bind_pattern(pattern, loaded);
      if (failed) {
        return;
      }
      emit_br(body_block);
      switch_to(body_block);
      return;
    }
    case ast::PatternKind::Or:
      internal(span, "or-pattern without expansion");
      return;
  }
}

// Expands or-pattern alternatives into sibling (pattern, body) arms
// sharing one body; checking required identical bindings.
void Lowerer::expand_or_arms(
    const ast::ExprMatchArm& arm,
    std::vector<std::pair<ast::PatternIdx, ast::ExprIdx>>& out) {
  if (ast.patterns[arm.pattern].kind != ast::PatternKind::Or) {
    out.emplace_back(arm.pattern, arm.body);
    return;
  }
  for (ast::PatternIdx alt :
       ast.patterns[arm.pattern].payload.or_pat.alternatives) {
    out.emplace_back(alt, arm.body);
  }
}

// A condition pattern may itself be an or-pattern, which lower_arm_test
// rejects because it expands alternatives at the match level instead.
// Only the top level of a condition is handled; an or-pattern nested
// inside a tuple or struct pattern needs the distributive expansion
// desugaring does not yet do, and bind_pattern reports that case.
std::vector<ast::PatternIdx> Lowerer::condition_alternatives(
    ast::PatternIdx pattern) {
  if (ast.patterns[pattern].kind != ast::PatternKind::Or) {
    return {pattern};
  }
  const std::span<const ast::PatternIdx> alts =
      ast.patterns[pattern].payload.or_pat.alternatives;
  return {alts.begin(), alts.end()};
}

// Tests a condition pattern against a scrutinee place: any matching
// alternative branches to `body_block`, and only a failure of all of
// them reaches `fail_block`. lower_arm_test leaves the current block on
// the match path, so a staging block bridges it back to the next
// alternative.
void Lowerer::lower_condition_test(ast::PatternIdx pattern,
                                   Val scrut_addr,
                                   ir::TypeIdx scrut_type,
                                   ir::BlockIdx body_block,
                                   ir::BlockIdx fail_block) {
  const bool saved_borrowed = arm_borrowed_;
  arm_borrowed_ = false;
  const std::vector<ast::PatternIdx> alternatives =
      condition_alternatives(pattern);
  for (usize i = 0; i + 1 < alternatives.size() && !failed; ++i) {
    const ir::BlockIdx matched = reserve_block();
    const ir::BlockIdx next = reserve_block();
    lower_arm_test(alternatives[i], scrut_addr, scrut_type, matched, next);
    if (failed) {
      return;
    }
    emit_br(body_block);
    switch_to(next);
  }
  if (!failed) {
    lower_arm_test(alternatives.back(), scrut_addr, scrut_type, body_block,
                   fail_block);
  }
  arm_borrowed_ = saved_borrowed;
}

void Lowerer::mark_field_move(ast::ExprIdx expr) {
  if (ast.exprs[expr].kind != ast::ExprKind::Field) {
    return;
  }
  Val field = place_addr(expr);
  if (field.place && field.address && !is_copy(field.type)) {
    mark_move(field);
  }
}

Val Lowerer::lower_match(ast::ExprIdx expr, const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprMatch& match = node.payload.get<ast::ExprMatch>();
  // A scrutinee read through a reference is a place to inspect, not a
  // value to load: the discriminant test must not move the borrow.
  const bool borrowed = scrutinee_borrowed(match.scrutinee);
  // A reference-typed scrutinee is read through as well: the pointer
  // is loaded and the arms test the value it names.
  const ir::TypeTag scrutinee_tag = tag_of(expr_type(match.scrutinee));
  const bool through_ref =
      scrutinee_tag == ir::TypeTag::Ref || scrutinee_tag == ir::TypeTag::MutRef;
  Val addr = Val{size_one, error_type(), true, false};
  if (borrowed) {
    addr = place_addr(match.scrutinee);
  } else {
    Val scrut = lower_expr(match.scrutinee, nullptr);
    if (failed || terminated_cur()) {
      return stopped_value();
    }
    addr = address_of(scrut);
    // A match consumes a non-Copy field, so the owner's drop skips it.
    mark_field_move(match.scrutinee);
  }
  if (failed || terminated_cur()) {
    return stopped_value();
  }
  if (through_ref) {
    Val pointer = materialize(addr);
    if (failed) {
      return stopped_value();
    }
    addr = Val{pointer.op, builder.primitive(ir::TypeTag::Ptr), true, true};
  }
  ir::TypeIdx scrut_type = expr_type(match.scrutinee);
  if (through_ref) {
    scrut_type = builder.state()
                     .ref_types[builder.state().types[scrut_type].as_ref()]
                     .pointee;
  }
  // The drop walk's move event lands before the arms: an arm that
  // returns from inside never reaches the join, and without the event
  // the scrutinee's drop would run on the payload the arm took. The
  // borrow marker still lands at the join, after the arms have read
  // the payloads.
  if (!borrowed) {
    record_move_event(addr);
  }
  const ir::TypeIdx result_type = expr_type(expr);
  Val slot{size_one, result_type, false, false};
  const bool has_slot = tag_of(result_type) != ir::TypeTag::Void &&
                        tag_of(result_type) != ir::TypeTag::Error;
  if (has_slot) {
    slot = result_slot(result_type, node.span);
  }
  // Or-pattern arms expand to one test each, and a guard is copied
  // onto every alternative that shares its body.
  std::vector<ast::PatternIdx> patterns;
  std::vector<ast::ExprIdx> guards;
  std::vector<ast::ExprIdx> arm_bodies;
  for (const ast::ExprMatchArm& arm : match.arms) {
    std::vector<std::pair<ast::PatternIdx, ast::ExprIdx>> expanded;
    expand_or_arms(arm, expanded);
    for (const auto& [pattern, body] : expanded) {
      patterns.push_back(pattern);
      guards.push_back(arm.guard);
      arm_bodies.push_back(body);
    }
  }
  std::vector<ir::BlockIdx> tests;
  std::vector<ir::BlockIdx> bodies;
  std::vector<ir::BlockIdx> guard_blocks;
  for (usize i = 0; i < patterns.size(); ++i) {
    tests.push_back(reserve_block());
    bodies.push_back(reserve_block());
    guard_blocks.push_back(guards[i].is_valid()
                               ? reserve_block()
                               : ir::BlockIdx(base::INVALID_IDX));
  }
  const ir::BlockIdx fail = reserve_block();
  ir::BlockIdx join = ir::BlockIdx(base::INVALID_IDX);
  if (patterns.empty()) {
    internal(node.span, "match without arms");
    return Val{size_one, error_type(), false, false};
  }
  emit_br(tests.front());
  // One epilogue per live arm; it closes the arm's path into the join.
  // The ends of the enclosing scope's values are the drop pass's to
  // place (ADR-0062).
  std::vector<ir::BlockIdx> epilogues;
  std::vector<bool> arm_live(patterns.size(), false);
  epilogues.reserve(patterns.size());
  for (usize i = 0; i < patterns.size(); ++i) {
    epilogues.push_back(reserve_block());
  }
  const bool saved_borrowed = arm_borrowed_;
  arm_borrowed_ = borrowed || through_ref;
  for (usize i = 0; i < patterns.size() && !failed; ++i) {
    // The arm's pattern bindings end at the arm's own exit, or the sweep
    // would place their drops where their slots do not dominate.
    const u32 arm_mark = static_cast<u32>(locals.size());
    switch_to(tests[i]);
    const ir::BlockIdx next = i + 1 < patterns.size() ? tests[i + 1] : fail;
    const bool has_guard = guard_blocks[i].is_valid();
    const ir::BlockIdx matched = has_guard ? guard_blocks[i] : bodies[i];
    const bool saved_guard_arm = guard_arm_;
    guard_arm_ = has_guard;
    lower_arm_test(patterns[i], addr, scrut_type, matched, next);
    guard_arm_ = saved_guard_arm;
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    // A guard tests after the pattern binds and falls through to the
    // next arm when it is false.
    if (has_guard) {
      switch_to(guard_blocks[i]);
      const Val guard = lower_expr(guards[i], nullptr);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      // A false guard falls through to the next arm, and the pattern's
      // bindings end on that edge: the next arm's test must not see
      // them. Ending them in the guard block itself would drop them on
      // the taken edge too, so the false edge gets its own block.
      const ir::BlockIdx guard_fail = reserve_block();
      emit_cond_br(use_value(guard), bodies[i], guard_fail);
      switch_to(guard_fail);
      record_end(arm_mark, node.span);
      emit_br(next);
    }
    // Arm testing always leaves lowering at the body block.
    switch_to(bodies[i]);
    Val produced = lower_expr(arm_bodies[i], expected);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    if (has_slot && !terminated_cur()) {
      store_result(slot, produced);
    } else if (!has_slot) {
      mark_move(produced);
    }
    if (!terminated_cur()) {
      record_end(arm_mark, node.span);
    }
    arm_live[i] = !terminated_cur();
    if (arm_live[i]) {
      emit_br(epilogues[i]);
    }
  }
  arm_borrowed_ = saved_borrowed;
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  for (usize i = 0; i < patterns.size(); ++i) {
    switch_to(epilogues[i]);
    if (!arm_live[i]) {
      // Nothing reaches this block; it still needs a terminator.
      emit_void(ir::Opcode::Unreachable, {});
      continue;
    }
    if (!join.is_valid()) {
      join = reserve_block();
    }
    emit_br(join);
  }
  switch_to(fail);
  emit_void(ir::Opcode::Unreachable, {});
  if (!join.is_valid()) {
    // Every arm ended the path, so the slot holds nothing and the match is
    // `!`. Reading it would emit past the `Unreachable` above.
    return Val{size_one, builder.never_type(), false, false};
  }
  switch_to(join);
  // A by-value match consumes a non-Copy scrutinee; the drop walk's
  // event was recorded before the arms, and the borrow marker lands
  // here, after the reads. A scrutinee read through a reference stays
  // its owner.
  if (!borrowed) {
    emit_move_marker(addr);
  }
  if (has_slot) {
    return materialize(slot);
  }
  return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
}

Val Lowerer::lower_if(ast::ExprIdx expr, const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprIf& if_expr = node.payload.get<ast::ExprIf>();
  const ir::TypeIdx result_type = expr_type(expr);
  const bool has_slot = tag_of(result_type) != ir::TypeTag::Void &&
                        tag_of(result_type) != ir::TypeTag::Error;
  Val slot{size_one, result_type, false, false};
  if (has_slot) {
    slot = result_slot(result_type, node.span);
  }
  ir::BlockIdx else_block = reserve_block();
  ir::BlockIdx join = ir::BlockIdx(base::INVALID_IDX);
  const ir::BlockIdx then_epilogue = reserve_block();
  const ir::BlockIdx else_epilogue = reserve_block();
  bool then_live = false;
  bool else_live = false;

  // Ends one arm: the result lands where it belongs and the arm's own
  // bindings end. The ends of the enclosing scope's values are the drop
  // pass's to place (ADR-0062).
  auto arm_tail = [&](Val produced, u32 arm_mark, ir::BlockIdx epilogue,
                      bool& live_out) {
    if (has_slot && !terminated_cur()) {
      store_result(slot, produced);
    } else if (!has_slot) {
      mark_move(produced);
    }
    if (!terminated_cur()) {
      record_end(arm_mark, node.span);
    }
    live_out = !terminated_cur();
    if (live_out) {
      emit_br(epilogue);
    }
  };

  const ast::Cond& cond_node = ast.conds[if_expr.cond];
  if (!cond_node.is_pattern) {
    ir::BlockIdx then_block = reserve_block();
    Val cond = lower_expr(cond_node.value, nullptr);
    if (failed) {
      return stopped_value();
    }
    if (terminated_cur()) {
      seal_blocks({then_block, else_block, then_epilogue, else_epilogue, join});
      return stopped_value();
    }
    Val material = materialize(cond);
    emit_cond_br(material.op, then_block, else_block);
    switch_to(then_block);
    {
      const u32 arm_mark = static_cast<u32>(locals.size());
      arm_tail(lower_block(if_expr.then_block, expected), arm_mark,
               then_epilogue, then_live);
    }
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    switch_to(else_block);
    if (if_expr.else_block.is_valid()) {
      const u32 arm_mark = static_cast<u32>(locals.size());
      arm_tail(lower_block(if_expr.else_block, expected), arm_mark,
               else_epilogue, else_live);
    } else if (has_slot) {
      internal(node.span, "value if without else");
      return Val{size_one, error_type(), false, false};
    } else {
      // Statement position without else: the empty arm owns everything.
      else_live = true;
      emit_br(else_epilogue);
    }
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
  } else {
    Val init = lower_expr(cond_node.init, nullptr);
    if (failed) {
      return stopped_value();
    }
    if (terminated_cur()) {
      seal_blocks({else_block, then_epilogue, else_epilogue, join});
      return stopped_value();
    }
    Val addr = address_of(init);
    const ir::TypeIdx scrut_type = expr_type(cond_node.init);
    // The binding takes a non-Copy payload out of a field, so the
    // field's place is moved and the owner's drop skips it.
    mark_field_move(cond_node.init);
    // The condition's bindings are locals of the then-arm, so its exit
    // ends them rather than the enclosing scope.
    const u32 arm_mark = static_cast<u32>(locals.size());
    ir::BlockIdx body_block = reserve_block();
    lower_condition_test(cond_node.pattern, addr, scrut_type, body_block,
                         else_block);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    // The then-arm's binding takes a non-`Copy` payload out of the
    // scrutinee, so the drop walk's move event lands at the top of the
    // body: a body that returns from inside still records it. The
    // borrow marker lands at the arm's tail, after the binding is read,
    // since a terminated block takes no more instructions.
    switch_to(body_block);
    record_move_event(addr);
    auto finish_then_arm = [&](Val produced, ir::BlockIdx epilogue,
                               bool& live_out) {
      if (!terminated_cur()) {
        emit_move_marker(addr);
      }
      arm_tail(produced, arm_mark, epilogue, live_out);
    };
    finish_then_arm(lower_block(if_expr.then_block, expected), then_epilogue,
                    then_live);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    switch_to(else_block);
    if (if_expr.else_block.is_valid()) {
      // The condition consumes its scrutinee on either arm, so the
      // else-arm records the move too and a later read does not see it.
      auto finish_else_arm = [&](Val produced, ir::BlockIdx epilogue,
                                 bool& live_out) {
        if (!terminated_cur()) {
          // The condition consumes its scrutinee on either arm, so the
          // arm the pattern did not take ends the value in place rather
          // than moving it and leaving its destructor unrun. Ending it
          // is itself the move the borrow walk sees.
          if (runs_destructor(addr.type)) {
            record_drop_in_place(addr.op, addr.type, node.span);
          } else {
            mark_move(addr);
          }
        }
        arm_tail(produced, arm_mark, epilogue, live_out);
      };
      finish_else_arm(lower_block(if_expr.else_block, expected), else_epilogue,
                      else_live);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
    } else if (!has_slot) {
      else_live = true;
      emit_br(else_epilogue);
    } else {
      internal(node.span, "value if without else");
      return Val{size_one, error_type(), false, false};
    }
  }
  // A dead arm's epilogue only closes the block.
  const auto fill_epilogue = [&](ir::BlockIdx epilogue, bool here_live) {
    switch_to(epilogue);
    if (!here_live) {
      emit_void(ir::Opcode::Unreachable, {});
      return;
    }
    if (!join.is_valid()) {
      join = reserve_block();
    }
    emit_br(join);
  };
  fill_epilogue(then_epilogue, then_live);
  fill_epilogue(else_epilogue, else_live);
  if (!join.is_valid()) {
    // Both arms ended the path, so the slot holds nothing and the `if`
    // is `!`.
    return Val{size_one, builder.never_type(), false, false};
  }
  switch_to(join);
  if (has_slot) {
    return materialize(slot);
  }
  return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
}

Val Lowerer::lower_loop(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprLoop& loop_expr = node.payload.get<ast::ExprLoop>();
  ir::BlockIdx header = reserve_block();
  ir::BlockIdx exit = reserve_block();
  emit_br(header);
  switch_to(header);
  const u32 drops = static_cast<u32>(locals.size());
  break_targets_.push_back({exit, drops});
  continue_targets_.push_back({header, drops});
  lower_block(loop_expr.body, nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  if (!terminated_cur()) {
    emit_br(header);
  }
  break_targets_.pop_back();
  continue_targets_.pop_back();
  switch_to(exit);
  // A loop with no `break` reaches no exit: the block is dead and the
  // loop's value is `!`, so the caller does not fall through it.
  if (tag_of(expr_type(expr)) == ir::TypeTag::Never) {
    emit_void(ir::Opcode::Unreachable, {});
    return Val{size_one, builder.never_type(), false, false};
  }
  return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
}

Val Lowerer::lower_while(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprWhile& while_expr = node.payload.get<ast::ExprWhile>();
  ir::BlockIdx header = reserve_block();
  ir::BlockIdx body = reserve_block();
  ir::BlockIdx exit = reserve_block();
  emit_br(header);
  switch_to(header);
  const ast::Cond& cond_node = ast.conds[while_expr.cond];
  // The condition's bindings live for one iteration, so the mark is
  // taken before the test pushes them: the body's exit and a `break` or
  // `continue` all end them.
  const u32 drops = static_cast<u32>(locals.size());
  if (!cond_node.is_pattern) {
    Val cond = lower_expr(cond_node.value, nullptr);
    if (failed) {
      return stopped_value();
    }
    if (terminated_cur()) {
      seal_blocks({body, exit});
      return stopped_value();
    }
    emit_cond_br(materialize(cond).op, body, exit);
    switch_to(body);
  } else {
    // The scrutinee re-evaluates on every iteration; the header both
    // tests and binds, so continue re-enters the test.
    Val init = lower_expr(cond_node.init, nullptr);
    if (failed) {
      return stopped_value();
    }
    if (terminated_cur()) {
      seal_blocks({body, exit});
      return stopped_value();
    }
    Val addr = address_of(init);
    const ir::TypeIdx scrut_type = expr_type(cond_node.init);
    mark_field_move(cond_node.init);
    lower_condition_test(cond_node.pattern, addr, scrut_type, body, exit);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    // lower_condition_test leaves the current block on the match path,
    // which is the loop body. The move lands after the test bound the
    // payload, and before the body, so neither is use-after-move.
    mark_move(addr);
  }
  break_targets_.push_back({exit, drops});
  continue_targets_.push_back({header, drops});
  lower_block(while_expr.body, nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  if (!terminated_cur()) {
    record_end(drops, node.span);
  }
  if (!terminated_cur()) {
    emit_br(header);
  }
  break_targets_.pop_back();
  continue_targets_.pop_back();
  switch_to(exit);
  return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
}

// `?` on an enum: the first variant yields its first payload, any
// other variant returns the scrutinee unchanged. Checking guarantees
// the scrutinee and the enclosing return type are the same enum.
Val Lowerer::lower_question(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprQuestion& question = node.payload.get<ast::ExprQuestion>();
  Val scrut = lower_expr(question.inner, nullptr);
  if (failed || terminated_cur()) {
    return stopped_value();
  }
  Val addr = address_of(scrut);
  const ir::TypeIdx scrut_type = expr_type(question.inner);
  mark_field_move(question.inner);
  const std::vector<ir::TypeIdx> first = variant_payload(scrut_type, 0);
  if (first.empty()) {
    internal(node.span, "question without a success variant");
    return Val{size_one, error_type(), false, false};
  }
  const Val tag = load_disc(addr);
  const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
  const ir::BlockIdx ok_block = reserve_block();
  const ir::BlockIdx err_block = reserve_block();
  const ir::BlockIdx join_block = reserve_block();
  const ir::RegisterIdx test =
      emit(ir::Opcode::Eq, boolean, {tag.op, disc_operand(0)});
  emit_cond_br(to_operand(test, boolean), ok_block, err_block);
  // Propagating keeps the original value, so the payload slot still
  // holds a valid scrutinee to return by value. The copy is what the
  // caller owns now, so the move is marked before the exit ends the
  // other live values.
  switch_to(err_block);
  const ir::OperandIdx propagate = use_value(addr);
  // Leaving the function ends every live value on this path; the
  // success path continues at ok_block and the pass places its ends
  // separately.
  record_end(0, node.span);
  emit_void(ir::Opcode::Ret, {propagate});
  switch_to(ok_block);
  const Val payload =
      load_payload_field(addr, scrut_type, variant_fields(scrut_type, 0), 0);
  emit_br(join_block);
  // Both arms read the discriminant and payload out of this place, so
  // the move lands at the join; placing it earlier would report those
  // very reads as use-after-move.
  switch_to(join_block);
  mark_move(addr);
  return payload;
}

bool Lowerer::report_nesting(diag::Span span) {
  if (reported_too_deep_) {
    return true;
  }
  reported_too_deep_ = true;
  const u32 index = bag.emit<i18n::Key::LowerNestingTooDeep>(
      DiagCode::TooDeep, span, nesting_.limit());
  (void)index;
  return true;
}

// An array ends its elements whole, because the drop walk's paths name
// fields and tuple slots and an element is neither; a value with a
// destructor that a caller takes out from under one would be ended
// twice.
bool Lowerer::reads_through_array_element(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  switch (node.kind) {
    case ast::ExprKind::Index: {
      const ast::ExprIndex& index = node.payload.get<ast::ExprIndex>();
      const ir::TypeIdx receiver = expr_type(index.receiver);
      if (receiver.is_valid() && tag_of(receiver) == ir::TypeTag::Array) {
        return true;
      }
      return reads_through_array_element(index.receiver);
    }
    case ast::ExprKind::Field:
      return reads_through_array_element(
          node.payload.get<ast::ExprField>().receiver);
    default: return false;
  }
}

Val Lowerer::lower_moved(ast::ExprIdx expr, const ir::TypeIdx* expected) {
  const ir::TypeIdx moved = expr_type(expr);
  if (moved.is_valid() && runs_destructor(moved) &&
      reads_through_array_element(expr)) {
    const u32 index = bag.emit<i18n::Key::LowerMoveOutOfElement>(
        DiagCode::MoveOutOfElement, ast.exprs[expr].span);
    (void)index;
    return Val{size_one, error_type(), false, false};
  }
  // Only a rooted projection names storage: the field of a call's
  // result is a temporary, and reading it is what it always was.
  if (ast.exprs[expr].kind != ast::ExprKind::Field || !is_rooted_place(expr)) {
    return lower_expr(expr, expected);
  }
  // A reference field is a handle the region rules govern: it is still
  // loaded, because moving one out of its owner is the conflict those
  // rules read. An owned field is handed over as a place, so the drop
  // walk records the partial move.
  if (moved.is_valid() && ir::is_pointer_type(tag_of(moved))) {
    return lower_expr(expr, expected);
  }
  return place_addr(expr);
}

Val Lowerer::lower_expr(ast::ExprIdx expr, const ir::TypeIdx* expected) {
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  // Every expression routes through here, so this one check bounds the
  // whole walk.
  if (nesting_.exhausted()) {
    report_nesting(ast.exprs[expr].span);
    failed = true;
    return Val{size_one, error_type(), false, false};
  }
  const base::NestingScope scope(nesting_);
  const ast::ExprNode& node = ast.exprs[expr];
  SpanGuard guard{this, cur_span_};
  cur_span_ = node.span;
  switch (node.kind) {
    case ast::ExprKind::Literal: {
      const ast::ExprLiteral& literal = node.payload.get<ast::ExprLiteral>();
      if (ast.literals[literal.value].kind == ast::LiteralKind::Char) {
        internal(node.span, "character literal without type");
        return Val{size_one, error_type(), false, false};
      }
      // The checker already settled this literal's type; re-deriving it
      // would emit an `i32` in a `usize` function, which is invalid IR.
      const ir::TypeIdx checked = expr_type(expr);
      const ir::TypeIdx* hint = expected;
      if (tag_of(checked) != ir::TypeTag::Error) {
        hint = &checked;
      }
      return lower_literal(literal.value, hint);
    }
    case ast::ExprKind::Path: {
      return lower_path(expr, expected);
    }
    case ast::ExprKind::Struct: {
      return lower_struct(expr);
    }
    case ast::ExprKind::Tuple: {
      return lower_tuple(expr);
    }
    case ast::ExprKind::Array: {
      return lower_array(expr);
    }
    case ast::ExprKind::Unary: {
      const ast::ExprUnary& unary = node.payload.get<ast::ExprUnary>();
      if (const analyzer::CheckedModule::CallTarget* target = call_target(expr);
          target != nullptr) {
        return lower_spec_unary(expr, target);
      }
      // A `-` before a literal is the literal's own sign: `-128i8` is a
      // value, not a subtraction, so a checked build never sees an
      // overflow where the source wrote one.
      if (unary.op == ast::UnaryOp::Neg &&
          ast.exprs[unary.inner].kind == ast::ExprKind::Literal) {
        const ast::ExprLiteral& literal =
            ast.exprs[unary.inner].payload.get<ast::ExprLiteral>();
        const ir::TypeIdx checked = expr_type(expr);
        const ir::TypeIdx* hint = expected;
        if (tag_of(checked) != ir::TypeTag::Error) {
          hint = &checked;
        }
        return lower_literal(literal.value, hint, /*negate=*/true);
      }
      Val inner = lower_moved(unary.inner, nullptr);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      const ir::TypeTag tag = tag_of(inner.type);
      if (unary.op == ast::UnaryOp::Not) {
        const ir::RegisterIdx dst =
            emit(ir::Opcode::Not, inner.type, {use_value(inner)});
        return Val{to_operand(dst, inner.type), inner.type, false, false};
      }
      if (unary.op == ast::UnaryOp::BitNot) {
        const ir::RegisterIdx dst =
            emit(ir::Opcode::Not, inner.type, {use_value(inner)});
        return Val{to_operand(dst, inner.type), inner.type, false, false};
      }
      Val zero = lower_literal_zero(inner.type, node.span);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      ir::Opcode op = ir::Opcode::IntSub;
      if (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64) {
        op = ir::Opcode::FSub;
      }
      const ir::RegisterIdx dst =
          emit(op, inner.type, {zero.op, use_value(inner)});
      return Val{to_operand(dst, inner.type), inner.type, false, false};
    }
    case ast::ExprKind::Borrow: {
      const ast::ExprBorrow& borrow = node.payload.get<ast::ExprBorrow>();
      if (ast.exprs[borrow.inner].kind == ast::ExprKind::Index) {
        const ast::ExprIndex& index =
            ast.exprs[borrow.inner].payload.get<ast::ExprIndex>();
        if (is_range_type(expr_type(index.index))) {
          Val base = lower_expr(index.receiver, nullptr);
          if (failed || terminated_cur()) {
            return stopped_value();
          }
          Val position = lower_expr(index.index, nullptr);
          if (failed || terminated_cur()) {
            return stopped_value();
          }
          return lower_subslice(base, position, borrow.is_mut, node.span);
        }
      }
      Val place = place_addr(borrow.inner);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      const ir::TypeIdx ref = builder.reference_type(place.type, borrow.is_mut);
      const ir::RegisterIdx loan = emit(ir::Opcode::Borrow, ref, {place.op});
      return Val{to_operand(loan, ref), ref, false, false};
    }
    case ast::ExprKind::Binary: {
      return lower_binary(expr);
    }
    case ast::ExprKind::Cast: {
      const ast::ExprCast& cast = node.payload.get<ast::ExprCast>();
      Val inner = lower_expr(cast.inner, nullptr);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      const ir::TypeIdx target = expr_type(expr);
      if (tag_of(target) == ir::TypeTag::Error) {
        internal(node.span, "cast without type");
        return Val{size_one, error_type(), false, false};
      }
      const ir::RegisterIdx dst =
          emit(ir::Opcode::TypeCast, target, {use_value(inner)});
      return Val{to_operand(dst, target), target, false, false};
    }
    case ast::ExprKind::Call: {
      return lower_call(expr);
    }
    case ast::ExprKind::MethodCall: {
      return lower_method_call(expr);
    }
    case ast::ExprKind::Field: {
      const ast::ExprField& field = node.payload.get<ast::ExprField>();
      // A rooted receiver is projected where it lives: reading a field
      // out of a copy would copy the whole value, and a field of a
      // borrowed place would look like a move out of the borrow.
      Val base = is_rooted_place(field.receiver)
                     ? place_addr(field.receiver)
                     : lower_expr(field.receiver, nullptr);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      return materialize(field_addr(base, field.name.name, node.span));
    }
    case ast::ExprKind::Deref: {
      return materialize(place_addr(expr));
    }
    case ast::ExprKind::Question: {
      return lower_question(expr);
    }
    case ast::ExprKind::If: {
      return lower_if(expr, expected);
    }
    case ast::ExprKind::Match: {
      return lower_match(expr, expected);
    }
    case ast::ExprKind::Loop: {
      return lower_loop(expr);
    }
    case ast::ExprKind::While: {
      return lower_while(expr);
    }
    case ast::ExprKind::Break: {
      if (break_targets_.empty()) {
        internal(node.span, "break without loop");
        return Val{size_one, error_type(), false, false};
      }
      // The body's own drop point is skipped by the branch out of it,
      // so everything the body declared ends here. The branch usually
      // sits in a nested block, so the innermost block's mark would
      // miss the body's own values.
      record_end(break_targets_.back().drops, node.span);
      emit_br(break_targets_.back().block);
      return Val{size_one, builder.never_type(), false, false};
    }
    case ast::ExprKind::Continue: {
      if (continue_targets_.empty()) {
        internal(node.span, "continue without loop");
        return Val{size_one, error_type(), false, false};
      }
      // Leaving the body early skips its normal drop point, so the
      // values the body declared are ended here instead; the mark is
      // the body's, not the nested block's the branch sits in.
      record_end(continue_targets_.back().drops, node.span);
      emit_br(continue_targets_.back().block);
      return Val{size_one, builder.never_type(), false, false};
    }
    case ast::ExprKind::Index: {
      const ast::ExprIndex& index = node.payload.get<ast::ExprIndex>();
      if (const analyzer::CheckedModule::CallTarget* target = call_target(expr);
          target != nullptr) {
        return lower_spec_index(expr, target, false);
      }
      if (is_range_type(expr_type(index.index))) {
        Val base = lower_expr(index.receiver, nullptr);
        if (failed || terminated_cur()) {
          return stopped_value();
        }
        Val position = lower_expr(index.index, nullptr);
        if (failed || terminated_cur()) {
          return stopped_value();
        }
        return lower_subslice(base, position, false, node.span);
      }
      Val base = lower_expr(index.receiver, nullptr);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      if (!base.address) {
        base = address_of(base);
      }
      Val position = lower_expr(index.index, nullptr);
      if (failed || terminated_cur()) {
        return stopped_value();
      }
      Val addr = checked_index_addr(base, position, node.span);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      return materialize(addr);
    }
    case ast::ExprKind::Range: return lower_range(expr);
    case ast::ExprKind::Closure: return lower_closure(expr);
    case ast::ExprKind::Block: {
      const ast::ExprBlock& block = node.payload.get<ast::ExprBlock>();
      if (!block.is_comp) {
        return lower_block(block.block, expected);
      }
      CompVal evaluated;
      if (!comp_lowering_.evaluate(module, expr, evaluated)) {
        return Val{size_one, error_type(), false, false};
      }
      return comp_lowering_.materialize(evaluated, node.span);
    }
    case ast::ExprKind::Return: {
      const ast::ExprReturn& ret = node.payload.get<ast::ExprReturn>();
      // Leaving the function ends every value still alive, so nothing
      // the caller receives outlives a value it was borrowed from. The
      // result is taken first, which moves it out of its origin.
      ir::OperandIdx result(base::INVALID_IDX);
      bool carries_result = false;
      if (ret.value.is_valid()) {
        Val value = lower_moved(ret.value, nullptr);
        if (failed || terminated_cur()) {
          return stopped_value();
        }
        if (tag_of(value.type) != ir::TypeTag::Void) {
          result = use_value(value);
          carries_result = true;
        }
      }
      record_end(0, node.span);
      if (carries_result) {
        emit_void(ir::Opcode::Ret, {result});
      } else {
        emit_void(ir::Opcode::Ret, {});
      }
      return Val{size_one, builder.never_type(), false, false};
    }
  }
}

Val Lowerer::lower_literal_zero(ir::TypeIdx type, diag::Span span) {
  const ir::TypeTag tag = tag_of(type);
  ir::Immutable imm{.type = type, .data = {}};
  switch (tag) {
    case ir::TypeTag::F32: imm.data.f32_value = 0; break;
    case ir::TypeTag::F64: imm.data.f64_value = 0; break;
    case ir::TypeTag::I1: imm.data.i1_value = false; break;
    case ir::TypeTag::I8: imm.data.i8_value = 0; break;
    case ir::TypeTag::I16: imm.data.i16_value = 0; break;
    case ir::TypeTag::I32: imm.data.i32_value = 0; break;
    case ir::TypeTag::I64: imm.data.i64_value = 0; break;
    case ir::TypeTag::U8: imm.data.u8_value = 0; break;
    case ir::TypeTag::U16: imm.data.u16_value = 0; break;
    case ir::TypeTag::U32: imm.data.u32_value = 0; break;
    case ir::TypeTag::U64: imm.data.u64_value = 0; break;
    default:
      internal(span, "zero without numeric type");
      return Val{size_one, error_type(), false, false};
  }
  return Val{to_operand(builder.immutable(imm), type), type, false, false};
}

void Lowerer::lower_stmt(ast::StmtIdx stmt) {
  if (failed || terminated_cur()) {
    return;
  }
  const ast::StmtNode& node = ast.stmts[stmt];
  SpanGuard guard{this, cur_span_};
  cur_span_ = node.span;
  switch (node.kind) {
    case ast::StmtKind::Decl: {
      const ast::StmtDecl& decl = node.payload.get<ast::StmtDecl>();
      if (decl.is_const) {
        CompVal evaluated;
        if (!comp_lowering_.evaluate(module, decl.init, evaluated)) {
          return;
        }
        // Comp bindings never take runtime addresses: runtime reads
        // splice constants through comp_scope_, so only publish the
        // value for later comp evaluation here.
        CompScope scope;
        scope.frames.emplace_back();
        if (!comp_lowering_.bind_pattern(module, decl.pattern, evaluated, scope,
                                         node.span)) {
          return;
        }
        for (auto& binding : scope.frames.back()) {
          comp_scope_.push_back(std::move(binding));
        }
        return;
      }
      // Moving into the binding consumes a field rather than loading a
      // copy: the owner's drop walk has to see the partial move.
      Val init = lower_moved(decl.init, nullptr);
      if (failed || terminated_cur()) {
        return;
      }
      if (ast.patterns[decl.pattern].kind == ast::PatternKind::Wildcard &&
          runs_destructor(init.type)) {
        // Discarding ends the value where it stands: the place is
        // consumed first, so a later use is refused and the owner does
        // not end it again, and the pass runs the glue over the slot
        // the value lands in.
        mark_move(init);
        Val slot = init.address ? init : address_of(init);
        if (failed) {
          return;
        }
        record_drop_in_place(slot.op, init.type, node.span);
        return;
      }
      // Moving into the binding consumes a non-Copy place.
      const ir::OperandIdx moved = use_value(init);
      bind_pattern(decl.pattern, Val{moved, init.type, false, false});
      return;
    }
    case ast::StmtKind::Reassign: {
      const ast::StmtReassign& reassign = node.payload.get<ast::StmtReassign>();
      // The value is read before the place is taken: `v[i] = v[j] + 1`
      // reads through a shared borrow before the place asks for the
      // exclusive one, so the two do not overlap.
      Val value = lower_moved(reassign.value, nullptr);
      if (failed || terminated_cur()) {
        return;
      }
      Val place = place_addr(reassign.place);
      if (failed || terminated_cur()) {
        return;
      }
      if (reassign.compound) {
        // `x op= y` reads the place and stores the operation's result,
        // so the operand order matches `x op y`.
        Val loaded = materialize(place);
        if (failed) {
          return;
        }
        const ir::TypeTag tag = tag_of(loaded.type);
        ir::Opcode op = ir::Opcode::Noop;
        if (tag == ir::TypeTag::F32 || tag == ir::TypeTag::F64) {
          switch (reassign.op) {
            case ast::BinaryOp::Add: op = ir::Opcode::FAdd; break;
            case ast::BinaryOp::Sub: op = ir::Opcode::FSub; break;
            case ast::BinaryOp::Mul: op = ir::Opcode::FMul; break;
            default: op = ir::Opcode::FDiv; break;
          }
        } else {
          op = int_binop(reassign.op, tag);
        }
        if (op == ir::Opcode::Noop) {
          // A nominal place has no backend operator: the checker
          // recorded the operator method on the place.
          const analyzer::CheckedModule::CallTarget* target =
              call_target(reassign.place);
          if (target == nullptr) {
            unsupported(node.span, "compound assignment");
            return;
          }
          value = emit_spec_call(target, node.span, loaded, value,
                                 /*has_argument=*/true);
          if (failed) {
            return;
          }
        } else {
          const ir::RegisterIdx result =
              emit(op, loaded.type, {use_value(loaded), use_value(value)});
          value =
              Val{to_operand(result, loaded.type), loaded.type, false, false};
        }
      }
      ir::OperandIdx stored = use_value(value);
      emit_void(ir::Opcode::Store, {stored, place.op});
      // A store re-owns what it writes. The pass ends the value it
      // replaces and clears the flags the exits consult.
      const ir::Operand& target = builder.state().operands[place.op.idx];
      u32 local_index = 0;
      std::vector<u32> path;
      if (target.is<ir::RegisterIdx>() &&
          place_of_register(target.as_register().idx, local_index, path)) {
        record_event(OwnershipEvent::Kind::Store, local_index, std::move(path),
                     node.span);
      }
      return;
    }
    case ast::StmtKind::Expr: {
      const ast::StmtExpr& expr = node.payload.get<ast::StmtExpr>();
      Val value = lower_moved(expr.value, nullptr);
      if (failed || terminated_cur()) {
        return;
      }
      if (runs_destructor(value.type)) {
        // The checker warns about the unused value; ending it is what
        // discarding means. The place is consumed first so a later use
        // is refused and the owner does not end it again.
        mark_move(value);
        Val slot = value.address ? value : address_of(value);
        if (failed) {
          return;
        }
        record_drop_in_place(slot.op, value.type, node.span);
        return;
      }
      mark_move(value);
      return;
    }
  }
}

Val Lowerer::lower_block(ast::BlockIdx block, const ir::TypeIdx* expected) {
  if (nesting_.exhausted()) {
    report_nesting(ast.blocks[block].span);
    failed = true;
    return Val{size_one, error_type(), false, false};
  }
  const base::NestingScope scope(nesting_);
  const ast::Block& node = ast.blocks[block];
  const u32 mark = static_cast<u32>(locals.size());
  // Comp bindings declared in this block leave with it, so a name from
  // an inner block is not visible after the block ends.
  const usize comp_mark = comp_scope_.size();
  auto leave = [&] { comp_scope_.resize(comp_mark); };
  bool reachable = true;
  for (ast::StmtIdx stmt : node.statements) {
    if (failed) {
      break;
    }
    if (!reachable) {
      const u32 index = bag.emit<i18n::Key::LowerUnreachableStatement>(
          DiagCode::Unreachable, ast.stmts[stmt].span);
      (void)index;
      continue;
    }
    lower_stmt(stmt);
    if (!failed && terminated_cur()) {
      reachable = false;
    }
  }
  // A reference to a local this block declares may not be the block's
  // value: the slot ends with the block, and the binding the value
  // reaches outlives it. The borrow pass refuses the same escape
  // through `ret`; a block value reaches a binding, which it does not
  // check.
  if (!failed && reachable && !terminated_cur() && node.value.is_valid() &&
      ast.exprs[node.value].kind == ast::ExprKind::Borrow) {
    ast::ExprIdx root =
        ast.exprs[node.value].payload.get<ast::ExprBorrow>().inner;
    while (root.is_valid()) {
      const ast::ExprNode& place = ast.exprs[root];
      if (place.kind == ast::ExprKind::Field) {
        root = place.payload.get<ast::ExprField>().receiver;
      } else if (place.kind == ast::ExprKind::Index) {
        root = place.payload.get<ast::ExprIndex>().receiver;
      } else {
        break;
      }
    }
    if (root.is_valid() && ast.exprs[root].kind == ast::ExprKind::Path) {
      const ast::PathIdx path =
          ast.exprs[root].payload.get<ast::ExprPath>().idx;
      const std::span<const ast::Ident> segments = ast.paths[path].segments;
      if (segments.size() == 1) {
        const Local* local = lookup_local(segments[0].name);
        if (local != nullptr &&
            static_cast<usize>(local - locals.data()) >= mark) {
          const u32 index = bag.emit<i18n::Key::LowerBlockReferenceEscapes>(
              DiagCode::BlockReferenceEscapes, node.span, segments[0].name);
          (void)index;
          failed = true;
        }
      }
    }
  }
  Val value{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  if (!failed && reachable && !terminated_cur()) {
    // A block's trailing value is lowered first: evaluating it can move
    // a value the block declared out, and ending a value that has
    // already left would run its destructor on nothing.
    if (node.value.is_valid()) {
      value = lower_expr(node.value, expected);
    }
    if (!failed && !terminated_cur()) {
      record_end(mark, node.span);
    }
  } else if (!reachable && node.value.is_valid()) {
    const u32 index = bag.emit<i18n::Key::LowerUnreachableExpression>(
        DiagCode::Unreachable, ast.exprs[node.value].span);
    (void)index;
  }
  const bool bad = failed || terminated_cur();
  leave();
  if (bad) {
    return Val{size_one, error_type(), false, false};
  }
  return value;
}

}  // namespace lowering
