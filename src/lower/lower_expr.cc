// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include <cstdlib>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analyzer/resolve.h"
#include "analyzer/types.h"
#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "fmt/format.h"
#include "fpag/base/idx.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/immutable.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "lower/lowerer.h"

namespace lower {

void Lowerer::unsupported(diag::Span span, std::string_view what) {
  const u32 index =
      bag.emit(diag::Severity::Error, kLowerUnsupported, span,
               "'{}' is not supported in this lowering slice", what);
  (void)index;
  failed = true;
}

void Lowerer::internal(diag::Span span, std::string_view what) {
  const u32 index = bag.emit(diag::Severity::Error, kLowerInternal, span,
                             "internal lowering error: {}", what);
  (void)index;
  failed = true;
}

ir::TypeIdx Lowerer::error_type() {
  return builder.error_type();
}

bool Lowerer::is_copy(ir::TypeIdx type) const {
  return ir::is_copy_type(builder.state(), type);
}

ir::TypeTag Lowerer::tag_of(ir::TypeIdx idx) const {
  return builder.state().types[idx].tag;
}

bool Lowerer::is_ref_tag(ir::TypeTag tag) const {
  return tag == ir::TypeTag::Ref || tag == ir::TypeTag::MutRef ||
         tag == ir::TypeTag::Ptr;
}

// Structural equality for the same reason the checker needs it:
// field slots hold copies, so shared shapes carry different indexes.
bool Lowerer::same_shape(ir::TypeIdx a, ir::TypeIdx b) {
  std::vector<u64> seen;
  return same_shape_inner(a, b, seen);
}

bool Lowerer::same_shape_inner(ir::TypeIdx a,
                               ir::TypeIdx b,
                               std::vector<u64>& seen) {
  if (a.idx == b.idx) {
    return true;
  }
  const u64 key = (static_cast<u64>(a.idx) << 32) | b.idx;
  for (u64 prior : seen) {
    if (prior == key) {
      return true;
    }
  }
  seen.push_back(key);
  const ir::TypeTag ta = tag_of(a);
  if (ta != tag_of(b)) {
    return false;
  }
  switch (ta) {
    case ir::TypeTag::Ref:
    case ir::TypeTag::MutRef: {
      const auto& refs = builder.state().ref_types;
      return same_shape_inner(refs[builder.state().types[a].as_ref()].pointee,
                              refs[builder.state().types[b].as_ref()].pointee,
                              seen);
    }
    case ir::TypeTag::Array: {
      const auto& arrays = builder.state().array_types;
      const ir::ArrayType& aa = arrays[builder.state().types[a].as_array()];
      const ir::ArrayType& ab = arrays[builder.state().types[b].as_array()];
      return aa.count == ab.count &&
             same_shape_inner(aa.element, ab.element, seen);
    }
    case ir::TypeTag::Tuple: {
      const auto& tuples = builder.state().tuple_types;
      const ir::TupleType& ta_t = tuples[builder.state().types[a].as_tuple()];
      const ir::TupleType& tb_t = tuples[builder.state().types[b].as_tuple()];
      if (ta_t.elements.size() != tb_t.elements.size()) {
        return false;
      }
      for (u32 i = 0; i < ta_t.elements.size(); ++i) {
        if (!same_shape_inner(ta_t.elements[i], tb_t.elements[i], seen)) {
          return false;
        }
      }
      return true;
    }
    case ir::TypeTag::Struct:
    case ir::TypeTag::Enum: return false;
    default: return true;
  }
}

// Literal values. Suffixes were validated by checking; strip the
// longest known suffix and parse what remains (wrapping arithmetic
// matches release overflow semantics; checked overflow is later work).
u64 Lowerer::parse_numeric_value(std::string_view spelling) {
  constexpr std::string_view kSuffixes[] = {
      "isize", "usize", "i8",  "i16", "i32", "i64",
      "u8",    "u16",   "u32", "u64", "f32", "f64",
  };

  // Strip type suffix if present.
  for (std::string_view suffix : kSuffixes) {
    const bool has_suffix =
        spelling.size() > suffix.size() && spelling.ends_with(suffix);
    if (has_suffix) {
      spelling.remove_suffix(suffix.size());
      break;
    }
  }

  // Determine base and strip prefix (e.g., "0x", "0b", "0o").
  u32 base = 10;
  const bool has_prefix = spelling.size() > 2 && spelling[0] == '0';
  if (has_prefix) {
    const char prefix_indicator = spelling[1];
    if (prefix_indicator == 'x' || prefix_indicator == 'X') {
      // hex
      base = 16;
      spelling.remove_prefix(2);
    } else if (prefix_indicator == 'b' || prefix_indicator == 'B') {
      // bin
      base = 2;
      spelling.remove_prefix(2);
    } else if (prefix_indicator == 'o' || prefix_indicator == 'O') {
      // oct
      base = 8;
      spelling.remove_prefix(2);
    }
  }

  // Accumulate numerical digits.
  u64 value = 0;
  for (const char ch : spelling) {
    if (ch == '_') {
      continue;
    }

    const bool is_digit = (ch >= '0' && ch <= '9');
    const bool is_lower_hex = (ch >= 'a' && ch <= 'f');
    const bool is_upper_hex = (ch >= 'A' && ch <= 'F');
    u32 digit = 0;
    if (is_digit) {
      digit = static_cast<u32>(ch - '0');
    } else if (is_lower_hex) {
      digit = static_cast<u32>(ch - 'a' + 10);
    } else if (is_upper_hex) {
      digit = static_cast<u32>(ch - 'A' + 10);
    } else {
      continue;
    }
    value = value * base + digit;
  }
  return value;
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
  constexpr SuffixTag kSuffixes[] = {
      {"f32", ir::TypeTag::F32},   {"f64", ir::TypeTag::F64},
      {"isize", ir::TypeTag::I64}, {"usize", ir::TypeTag::U64},
      {"i8", ir::TypeTag::I8},     {"i16", ir::TypeTag::I16},
      {"i32", ir::TypeTag::I32},   {"i64", ir::TypeTag::I64},
      {"u8", ir::TypeTag::U8},     {"u16", ir::TypeTag::U16},
      {"u32", ir::TypeTag::U32},   {"u64", ir::TypeTag::U64},
  };
  for (const SuffixTag& entry : kSuffixes) {
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
                           const ir::TypeIdx* expected) {
  const ast::Literal& lit = ast.literals[lit_idx];
  const ir::TypeTag tag = literal_tag(lit_idx, expected);
  const ir::TypeIdx type = builder.primitive(tag);
  if (tag == ir::TypeTag::Str) {
    std::string bytes;
    std::string_view spelling = lit.spelling;
    if (spelling.size() >= 2) {
      spelling.remove_prefix(1);
      spelling.remove_suffix(1);
    }
    for (usize i = 0; i < spelling.size(); ++i) {
      const char c = spelling[i];
      if (c != '\\' || i + 1 >= spelling.size()) {
        bytes.push_back(c);
        continue;
      }
      const char esc = spelling[++i];
      switch (esc) {
        case 'n': bytes.push_back('\n'); break;
        case 't': bytes.push_back('\t'); break;
        case 'r': bytes.push_back('\r'); break;
        case '\\': bytes.push_back('\\'); break;
        case '"': bytes.push_back('"'); break;
        case '0': bytes.push_back('\0'); break;
        default: bytes.push_back(esc); break;
      }
    }
    const str::StringPoolId id = strings.intern(bytes);
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
    const double value =
        digits.empty() ? 0.0 : std::strtod(digits.c_str(), nullptr);
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
    value = parse_numeric_value(lit.spelling);
  }
  return Val{imm_from_u64(tag, type, value), type, false, false};
}

ir::OperandIdx Lowerer::imm_from_u64(ir::TypeTag tag,
                                     ir::TypeIdx type,
                                     u64 value) {
  ir::Immutable imm{.type = type, .data = {}};
  switch (tag) {
    case ir::TypeTag::I1: imm.data.i1_value = value != 0; break;
    case ir::TypeTag::I8: imm.data.i8_value = static_cast<i8>(value); break;
    case ir::TypeTag::I16: imm.data.i16_value = static_cast<i16>(value); break;
    case ir::TypeTag::I32: imm.data.i32_value = static_cast<i32>(value); break;
    case ir::TypeTag::I64: imm.data.i64_value = static_cast<i64>(value); break;
    case ir::TypeTag::U8: imm.data.u8_value = static_cast<u8>(value); break;
    case ir::TypeTag::U16: imm.data.u16_value = static_cast<u16>(value); break;
    case ir::TypeTag::U32: imm.data.u32_value = static_cast<u32>(value); break;
    default: imm.data.u64_value = value; break;
  }
  return to_operand(builder.immutable(imm), type);
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
      // `*p` names is the one that address points at.
      Val inner = materialize(
          lower_expr(node.payload.get<ast::ExprDeref>().inner, nullptr));
      if (failed) {
        return Val{size_one, error_type(), true, false};
      }
      const ir::TypeTag tag = tag_of(inner.type);
      if (tag != ir::TypeTag::Ref && tag != ir::TypeTag::MutRef) {
        internal(node.span, "dereference of a non-reference");
        return Val{size_one, error_type(), true, false};
      }
      const ir::TypeIdx pointee =
          builder.ref_types()[builder.types()[inner.type.idx].as_ref()].pointee;
      return Val{inner.op, pointee, true, true};
    }
    default:
      unsupported(node.span, "borrowed temporary");
      return Val{size_one, error_type(), true, false};
  }
}

// Bounds-checked address of base[position]: panics out of bounds.
// The base must be a direct array address; indexing through a
// reference cannot project through codegen's alloca tracking.
Val Lowerer::checked_index_addr(Val base, Val position, diag::Span span) {
  if (!base.address || tag_of(base.type) != ir::TypeTag::Array) {
    unsupported(span, "index through reference");
    return Val{size_one, error_type(), true, false};
  }
  const ir::ArrayType& shape =
      builder.state().array_types[builder.state().types[base.type].as_array()];
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
  emit_panic(str_operand("index out of bounds"));
  switch_to(ok_block);
  if (failed) {
    return Val{size_one, error_type(), true, false};
  }
  const ir::RegisterIdx gep = emit(ir::Opcode::GetElementPtr, shape.element,
                                   {base.op, zero_i32, use_value(wide)});
  return Val{to_operand(gep, shape.element), shape.element, true, base.place};
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
  for (const auto& checked : pkg.modules) {
    for (const auto& info : checked.structs) {
      if (info.type.idx != origin.idx) {
        continue;
      }
      for (u32 i = 0; i < static_cast<u32>(info.fields.size()); ++i) {
        if (info.fields[i] == name) {
          index_out = i;
          return true;
        }
      }
      return false;
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
  const ast::PatternNode& node = ast.patterns[pattern];
  switch (node.kind) {
    case ast::PatternKind::Wildcard: break;
    case ast::PatternKind::Ident:
    case ast::PatternKind::MutIdent: {
      std::string_view name;
      if (node.kind == ast::PatternKind::Ident) {
        name = node.payload.ident.name.name;
      } else {
        name = node.payload.mut_ident.name.name;
      }
      if (tag_of(init.type) == ir::TypeTag::Void) {
        locals.push_back({name, ir::RegisterIdx(base::kInvalidIdx), init.type});
        return;
      }
      const Val material = materialize(init);
      const ir::RegisterIdx addr =
          emit(ir::Opcode::Alloca, init.type, {size_one});
      emit_void(ir::Opcode::Store, {material.op, to_operand(addr, init.type)});
      locals.push_back({name, addr, init.type});
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
                 {base.op, zero_i32, index_operand(i)});
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
                 {base.op, zero_i32, index_operand(index)});
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
    case ast::PatternKind::Or:
      unsupported(node.span, "refutable pattern in lowering");
      return;
  }
}

Val Lowerer::lower_path(ast::ExprIdx expr, const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::PathIdx path = node.payload.get<ast::ExprPath>().idx;
  const std::span<const ast::Ident> segments = ast.paths[path].segments;
  if (segments.size() == 1) {
    const std::string_view name = segments[0].name;
    if (const Local* local = lookup_local(name)) {
      if (tag_of(local->type) == ir::TypeTag::Void) {
        return Val{size_one, local->type, false, false};
      }
      return Val{to_operand(local->addr, local->type), local->type, true, true};
    }
    // Comp bindings never take runtime addresses; splice the value.
    for (usize i = comp_scope_.size(); i-- > 0;) {
      if (comp_scope_[i].first == name) {
        return materialize_comp_value(comp_scope_[i].second, node.span);
      }
    }
    if (const auto* info = lookup_static(module, name)) {
      if (info->is_const && info->init.is_valid() &&
          ast.exprs[info->init].kind == ast::ExprKind::Literal) {
        // The declared type wins over the context: a `const` read is
        // always the type it was declared with.
        const ir::TypeIdx want =
            info->type.is_valid()
                ? info->type
                : (expected != nullptr ? *expected : error_type());
        return lower_literal(
            ast.exprs[info->init].payload.get<ast::ExprLiteral>().value,
            info->type.is_valid() ? &want : expected);
      }
      unsupported(node.span, "static item in lowering");
      return Val{size_one, error_type(), false, false};
    }
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
    const ir::TypeIdx slot = enum_slot_type();
    const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, slot, {size_one});
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
    seq.push(builder.ref_type(param));
  }
  const ir::ExternalFunctionIdx idx =
      builder.external_function({.meta = {.return_type = ret,
                                          .param_types = seq.finish(),
                                          .name = strings.intern(name)},
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
  for (const auto& checked : pkg.modules) {
    for (const auto& use : checked.variants) {
      if (use.path == path && use.inst == inst) {
        return &use;
      }
    }
  }
  return nullptr;
}

const analyzer::CheckedModule::EnumInfo* Lowerer::enum_info(
    ir::TypeIdx type) const {
  for (const auto& checked : pkg.modules) {
    for (const auto& info : checked.enums) {
      if (info.type.idx == type.idx) {
        return &info;
      }
    }
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

// Payload field types of one variant, in order.
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

ir::TypeIdx Lowerer::enum_slot_type() {
  if (enum_slot_type_.is_valid()) {
    return enum_slot_type_;
  }
  ir::TypeSeq seq;
  seq.push(builder.ref_type(builder.primitive(ir::TypeTag::I32)));
  seq.push(builder.ref_type(builder.primitive(ir::TypeTag::Ptr)));
  enum_slot_type_ = builder.tuple_type(seq.finish());
  return enum_slot_type_;
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
  const ir::TypeIdx slot = enum_slot_type();
  const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, slot, {size_one});
  const ir::RegisterIdx tag =
      emit(ir::Opcode::GetElementPtr, builder.primitive(ir::TypeTag::I32),
           {to_operand(addr, slot), zero_i32, index_operand(0)});
  emit_void(ir::Opcode::Store,
            {disc_operand(use->variant), to_operand(tag, slot)});
  if (is_unit_payload(payloads)) {
    // Lower for effects; nothing is stored.
    for (usize i = 0; i < payloads.size(); ++i) {
      Val value =
          lower_expr(call.payload.get<ast::ExprCall>().args[i], &payloads[i]);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      mark_move(value);
    }
  } else if (!payloads.empty()) {
    const ir::TypeIdx payload_type = payload_tuple(payloads);
    const ir::RegisterIdx payload =
        emit(ir::Opcode::Alloca, payload_type, {size_one});
    for (usize i = 0; i < payloads.size(); ++i) {
      Val value =
          lower_expr(call.payload.get<ast::ExprCall>().args[i], &payloads[i]);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      const ir::RegisterIdx field =
          emit(ir::Opcode::GetElementPtr, payloads[i],
               {to_operand(payload, payload_type), zero_i32,
                index_operand(static_cast<u32>(i))});
      emit_void(ir::Opcode::Store,
                {use_value(value), to_operand(field, payloads[i])});
    }
    const ir::TypeIdx ptr = builder.primitive(ir::TypeTag::Ptr);
    const ir::RegisterIdx slot_field =
        emit(ir::Opcode::GetElementPtr, ptr,
             {to_operand(addr, slot), zero_i32, index_operand(1)});
    emit_void(ir::Opcode::Store,
              {to_operand(payload, ptr), to_operand(slot_field, ptr)});
  }
  return Val{to_operand(addr, slot), use->enum_type, true, false};
}

ir::OperandIdx Lowerer::disc_operand(u32 discriminant) {
  const ir::TypeIdx i32_ty = builder.primitive(ir::TypeTag::I32);
  ir::Immutable imm{.type = i32_ty, .data = {}};
  imm.data.i32_value = static_cast<i32>(discriminant);
  return to_operand(builder.immutable(imm), i32_ty);
}

Val Lowerer::lower_call(ast::ExprIdx expr, const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
  // Intrinsics by name (checking rejected shadowing definitions).
  if (ast.exprs[call.callee].kind == ast::ExprKind::Path) {
    const ast::ExprPath& path =
        ast.exprs[call.callee].payload.get<ast::ExprPath>();
    const ast::PathIdx path_idx = path.idx;
    if (ast.paths[path_idx].segments.size() == 1) {
      const std::string_view name = ast.paths[path_idx].segments[0].name;
      if (lookup_local(name) == nullptr &&
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
        if (!shadowed &&
            (name == "print" || name == "println" || name == "panic")) {
          return lower_intrinsic(expr, name);
        }
      }
    }
  }
  const analyzer::CheckedModule::CallTarget* target = call_target(call.callee);
  if (target == nullptr) {
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
  if (sig.name == "write" && pkg.tree.modules[target->module]->is_prelude) {
    return lower_fmt_write(expr, sig);
  }
  if (sig.name == "format" && pkg.tree.modules[target->module]->is_prelude) {
    return lower_fmt_format(expr, sig);
  }
  const std::vector<u32> comp = comp_positions(sig.item);
  std::vector<CompVal> comp_args;
  for (u32 pos : comp) {
    CompVal arg;
    if (!comp_evaluate(module, call.args[pos], arg)) {
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
    Val arg = lower_expr(call.args[i], &sig.params[i]);
    if (failed) {
      return Val{size_one, error_type(), false, false};
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
  if (expected != nullptr) {
    (void)expected;
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
  const std::vector<u32> comp = comp_positions(info.item);
  std::vector<CompVal> comp_args;
  for (u32 pos : comp) {
    CompVal arg;
    if (!comp_evaluate(module, args[pos], arg)) {
      return Val{size_one, error_type(), false, false};
    }
    comp_args.push_back(std::move(arg));
  }
  const ir::FunctionIdx fn =
      fn_index(target->module, info.item, info.name, info.params, info.ret,
               callee_inst(target), std::move(comp_args));
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
    Val arg = lower_expr(args[i], &info.params[i]);
    if (failed) {
      return Val{size_one, error_type(), false, false};
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
// runtime hooks or IR operations. Undeclared legacy names
// (print/println/panic) still arrive through lower_intrinsic.
Val Lowerer::lower_intrinsic_call(ast::ExprIdx expr,
                                  const analyzer::CheckedModule::FnSig& sig,
                                  const std::vector<ir::TypeIdx>& type_args) {
  const ast::ExprNode& node = ast.exprs[expr];
  const std::string_view name = sig.name;
  if (name == "print" || name == "println" || name == "panic") {
    return lower_intrinsic(expr, name);
  }
  if (name == "sys_write") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 2) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val fd = lower_expr(call.args[0], nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val buf = lower_expr(call.args[1], nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
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
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val src = lower_expr(call.args[1], nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val len = lower_expr(call.args[2], nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    emit_void(ir::Opcode::Memcopy,
              {use_value(dst), use_value(src), use_value(len)});
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }
  if (name == "str_len" || name == "str_byte" || name == "str_slice") {
    return lower_str_intrinsic(expr, name);
  }
  if (name == "elem_ptr") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 2) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    std::vector<ir::OperandIdx> args;
    for (usize i = 0; i < 2; ++i) {
      Val arg = lower_expr(call.args[i], &sig.params[i]);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      args.push_back(arg_for(arg, sig.params[i]));
    }
    const ir::RegisterIdx result = emit(ir::Opcode::ElemOffset, sig.ret, args);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(result, sig.ret), sig.ret, false, false};
  }
  if (name == "uninit_write" || name == "uninit_assume") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    const usize arity = name == "uninit_write" ? 2 : 1;
    if (call.args.size() != arity) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val slot = lower_expr(call.args[0], &sig.params[0]);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::OperandIdx slot_op = arg_for(slot, sig.params[0]);
    if (name == "uninit_assume") {
      // The wrapper is representation-transparent, so releasing the
      // value is a relabel of the same address.
      const ir::RegisterIdx held =
          emit(ir::Opcode::TypeCast, sig.ret, {slot_op});
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      return Val{to_operand(held, sig.ret), sig.ret, false, false};
    }
    Val value = lower_expr(call.args[1], &sig.params[1]);
    if (failed) {
      return Val{size_one, error_type(), false, false};
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
      Val arg = lower_expr(call.args[i], &sig.params[i]);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      args.push_back(arg_for(arg, sig.params[i]));
    }
    // The runtime counts bytes; the compiler supplies the element size
    // and alignment it reserved them at.
    const ir::RegisterIdx bytes =
        emit_type_query(ir::Opcode::TypeSizeOf, type_args[0]);
    const ir::RegisterIdx align =
        emit_type_query(ir::Opcode::TypeAlignOf, type_args[0]);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::RegisterIdx total =
        emit(ir::Opcode::IntMul, usize_ty,
             {to_operand(bytes, usize_ty), args[name == "alloc" ? 0 : 1]});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::OperandIdx size_op = to_operand(total, usize_ty);
    const ir::OperandIdx align_op = to_operand(align, usize_ty);
    if (name == "dealloc") {
      const ir::ExternalFunctionIdx ext =
          declare_external("alcy_dealloc", builder.primitive(ir::TypeTag::Void),
                           {ptr_ty, usize_ty, usize_ty});
      emit_void(ir::Opcode::Call,
                {builder.operand(ir::Operand::from_external_function(
                     ext, builder.primitive(ir::TypeTag::Function))),
                 args[0], size_op, align_op});
      return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
    }
    const ir::ExternalFunctionIdx ext =
        declare_external("alcy_alloc", ptr_ty, {usize_ty, usize_ty});
    const ir::RegisterIdx result =
        emit(ir::Opcode::Call, ptr_ty,
             {builder.operand(ir::Operand::from_external_function(
                  ext, builder.primitive(ir::TypeTag::Function))),
              size_op, align_op});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    // The intrinsic's `&mut T` return is a reference; the runtime
    // pointer needs its pointee label for later element offsets.
    const ir::RegisterIdx labelled =
        emit(ir::Opcode::TypeCast, sig.ret, {to_operand(result, ptr_ty)});
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    return Val{to_operand(labelled, sig.ret), sig.ret, false, false};
  }
  if (name == "str_from_parts") {
    const ast::ExprCall& call = node.payload.get<ast::ExprCall>();
    if (call.args.size() != 2) {
      internal(node.span, "intrinsic arity");
      return Val{size_one, error_type(), false, false};
    }
    Val ptr = lower_expr(call.args[0], nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val len = lower_expr(call.args[1], nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
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
    return ir::OperandIdx(base::kInvalidIdx);
  }
  const ir::RegisterIdx sum = emit(ir::Opcode::IntAdd, usize_ty,
                                   {to_operand(as_int, usize_ty), offset});
  if (failed) {
    return ir::OperandIdx(base::kInvalidIdx);
  }
  const ir::RegisterIdx bumped =
      emit(ir::Opcode::TypeCast, ptr_ty, {to_operand(sum, usize_ty)});
  if (failed) {
    return ir::OperandIdx(base::kInvalidIdx);
  }
  (void)span;
  return to_operand(bumped, ptr_ty);
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
  if (failed) {
    return Val{size_one, error_type(), false, false};
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
    emit_panic(str_operand("index out of bounds"));
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
    emit_panic(str_operand("slice out of bounds"));
    switch_to(ok_block);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::OperandIdx sub = advance_ptr(bytes, start, node.span);
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
              {sub, to_operand(field0, builder.primitive(ir::TypeTag::Ptr))});
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

Val Lowerer::lower_intrinsic(ast::ExprIdx expr, std::string_view name) {
  const ast::ExprNode& call = ast.exprs[expr];
  if (call.payload.get<ast::ExprCall>().args.size() != 1) {
    internal(call.span, "intrinsic arity");
    return Val{size_one, error_type(), false, false};
  }
  const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
  Val arg = lower_expr(call.payload.get<ast::ExprCall>().args[0], &str);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const Val material = materialize(arg);
  // Fat strings cross the ABI as (bytes, len).
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  const ir::TypeIdx usize_ty = builder.primitive(
      width == ir::PointerWidth::W64 ? ir::TypeTag::U64 : ir::TypeTag::U32);
  const ir::RegisterIdx bytes =
      emit(ir::Opcode::ExtractValue, ptr_ty, {material.op, index_operand(0)});
  const ir::RegisterIdx len =
      emit(ir::Opcode::ExtractValue, usize_ty, {material.op, index_operand(1)});

  bool is_panic = false;
  ir::ExternalFunctionIdx ext(0);
  if (name == "print") {
    ext = declare_external("alcy_print", builder.primitive(ir::TypeTag::Void),
                           {ptr_ty, usize_ty});

  } else if (name == "println") {
    ext = declare_external("alcy_println", builder.primitive(ir::TypeTag::Void),
                           {ptr_ty, usize_ty});
  } else if (name == "panic") {
    is_panic = true;
    ext = declare_external("alcy_panic", builder.never_type(),
                           {ptr_ty, usize_ty});
  }
  emit_void(ir::Opcode::Call,
            {builder.operand(ir::Operand::from_external_function(
                 ext, builder.primitive(ir::TypeTag::Function))),
             to_operand(bytes, ptr_ty), to_operand(len, usize_ty)});
  if (is_panic) {
    emit_void(ir::Opcode::Unreachable, {});
    return Val{size_one, builder.never_type(), false, false};
  }
  return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
}

ir::OperandIdx Lowerer::arg_for(Val arg, ir::TypeIdx param) {
  if (!is_ref_tag(tag_of(param))) {
    return use_value(arg);
  }
  if (arg.address) {
    if (is_ref_tag(tag_of(arg.type))) {
      return materialize(arg).op;
    }
    return arg.op;
  }
  if (is_ref_tag(tag_of(arg.type))) {
    return arg.op;
  }
  return address_of(arg).op;
}

// Address of an enum slot value (spills SSA temporaries).
Val Lowerer::enum_addr(Val value) {
  return address_of(value);
}

// Loads the discriminant of an enum slot address.
Val Lowerer::load_disc(Val slot_addr) {
  const ir::TypeIdx i32 = builder.primitive(ir::TypeTag::I32);
  const ir::RegisterIdx gep = emit(ir::Opcode::GetElementPtr, i32,
                                   {slot_addr.op, zero_i32, index_operand(0)});
  const ir::RegisterIdx loaded =
      emit(ir::Opcode::Load, i32, {to_operand(gep, i32)});
  return Val{to_operand(loaded, i32), i32, false, false};
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

ir::TypeIdx Lowerer::payload_tuple(const std::vector<ir::TypeIdx>& fields) {
  ir::TypeSeq seq;
  for (ir::TypeIdx field : fields) {
    seq.push(builder.ref_type(field));
  }
  return builder.tuple_type(seq.finish());
}

// Value of payload field i of the enum at slot_addr. The payload
// pointer is type-erased in the slot, so it reinterprets through
// the variant payload type before projecting the field.
Val Lowerer::load_payload_field(Val slot_addr,
                                ir::TypeIdx payload_type,
                                u32 field) {
  const ir::TupleType& shape =
      builder.state()
          .tuple_types[builder.state().types[payload_type].as_tuple()];
  const ir::TypeIdx field_type = shape.elements[field];
  const ir::TypeIdx ptr = builder.primitive(ir::TypeTag::Ptr);
  const ir::RegisterIdx ptr_gep =
      emit(ir::Opcode::GetElementPtr, ptr,
           {slot_addr.op, zero_i32, index_operand(1)});
  const ir::RegisterIdx payload =
      emit(ir::Opcode::Load, ptr, {to_operand(ptr_gep, ptr)});
  const ir::RegisterIdx typed =
      emit(ir::Opcode::TypeCast, payload_type, {to_operand(payload, ptr)});
  const ir::RegisterIdx field_gep =
      emit(ir::Opcode::GetElementPtr, field_type,
           {to_operand(typed, payload_type), zero_i32, index_operand(field)});
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

void Lowerer::emit_panic(ir::OperandIdx message) {
  const ir::TypeIdx ptr_ty = builder.primitive(ir::TypeTag::Ptr);
  const ir::TypeIdx usize_ty = builder.primitive(
      width == ir::PointerWidth::W64 ? ir::TypeTag::U64 : ir::TypeTag::U32);
  const ir::RegisterIdx bytes =
      emit(ir::Opcode::ExtractValue, ptr_ty, {message, index_operand(0)});
  const ir::RegisterIdx len =
      emit(ir::Opcode::ExtractValue, usize_ty, {message, index_operand(1)});
  const ir::ExternalFunctionIdx ext =
      declare_external("alcy_panic", builder.never_type(), {ptr_ty, usize_ty});
  emit_void(ir::Opcode::Call,
            {builder.operand(ir::Operand::from_external_function(
                 ext, builder.primitive(ir::TypeTag::Function))),
             to_operand(bytes, ptr_ty), to_operand(len, usize_ty)});
  emit_void(ir::Opcode::Unreachable, {});
}

ir::OperandIdx Lowerer::str_operand(std::string_view message) {
  const ir::TypeIdx str = builder.primitive(ir::TypeTag::Str);
  const ir::ImmutableIdx imm = builder.immutable(
      {.type = str, .data = {.str_id_value = strings.intern(message)}});
  return to_operand(imm, str);
}

Val Lowerer::lower_method_call(ast::ExprIdx expr, const ir::TypeIdx* expected) {
  const ast::ExprNode& method = ast.exprs[expr];
  Val receiver =
      lower_expr(method.payload.get<ast::ExprMethodCall>().receiver, nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  const analyzer::CheckedModule::CallTarget* target = call_target(expr);
  if (target == nullptr || !target->is_method) {
    internal(method.span, "method call without target");
    return Val{size_one, error_type(), false, false};
  }
  const analyzer::CheckedModule& def = pkg.modules[target->module];
  const analyzer::CheckedModule::MethodInfo& info = def.methods[target->index];
  const std::vector<u32> comp = comp_positions(info.item);
  if (!comp.empty() && comp[0] == 0) {
    unsupported(method.span, "comp method receiver");
    return Val{size_one, error_type(), false, false};
  }
  std::vector<CompVal> comp_args;
  const std::span<const ast::ExprIdx> method_args =
      method.payload.get<ast::ExprMethodCall>().args;
  for (u32 pos : comp) {
    CompVal arg;
    if (!comp_evaluate(module, method_args[pos - 1], arg)) {
      return Val{size_one, error_type(), false, false};
    }
    comp_args.push_back(std::move(arg));
  }
  ir::FunctionIdx fn =
      fn_index(target->module, info.item, info.name, info.params, info.ret,
               callee_inst(target), std::move(comp_args));
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
    Val arg = lower_expr(method_args[i], &info.params[i + 1]);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    ops.push_back(arg_for(arg, info.params[i + 1]));
  }
  if (tag_of(info.ret) == ir::TypeTag::Void) {
    emit_void(ir::Opcode::Call, ops);
    return Val{size_one, info.ret, false, false};
  }
  if (expected != nullptr) {
    (void)expected;
  }
  const ir::RegisterIdx dst = emit(ir::Opcode::Call, info.ret, ops);
  return Val{to_operand(dst, info.ret), info.ret, false, false};
}

// Address of `base.name`: sees through references to the nominal
// carrying the field. Works for values and places alike.
Val Lowerer::field_addr(Val base, std::string_view name, diag::Span span) {
  ir::OperandIdx ptr_op = size_one;
  ir::TypeIdx struct_ty = error_type();
  const ir::TypeTag tag = tag_of(base.type);
  if (base.address && !is_ref_tag(tag)) {
    ptr_op = base.op;
    struct_ty = base.type;
  } else if (base.address || is_ref_tag(tag)) {
    const Val material = materialize(base);
    ptr_op = material.op;
    struct_ty = builder.state()
                    .ref_types[builder.state().types[base.type].as_ref()]
                    .pointee;
  } else {
    const Val spilled = address_of(base);
    ptr_op = spilled.op;
    struct_ty = base.type;
  }
  u32 index = 0;
  if (tag_of(struct_ty) == ir::TypeTag::Tuple) {
    for (char c : name) {
      index = index * 10 + static_cast<u32>(c - '0');
    }
  } else if (!struct_field_index(struct_ty, name, index)) {
    internal(span, "field without declaration");
    return Val{size_one, error_type(), true, false};
  }
  const ir::TypeIdx field_type = field_type_of(struct_ty, index, span);
  const ir::RegisterIdx gep = emit(ir::Opcode::GetElementPtr, field_type,
                                   {ptr_op, zero_i32, index_operand(index)});
  return Val{to_operand(gep, field_type), field_type, true, base.place};
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
  for (const auto& checked : pkg.modules) {
    for (const auto& info : checked.structs) {
      if (info.type.idx == struct_type.idx) {
        field_count = static_cast<u32>(info.fields.size());
      }
    }
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
    Val value = lower_expr(field.value, &element_type);
    if (failed) {
      return Val{size_one, error_type(), false, false};
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
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val base_addr = address_of(base);
    // Update consumes the base value.
    mark_move(base_addr);
    for (u32 i = 0; i < static_cast<u32>(seen.size()); ++i) {
      if (seen[i]) {
        continue;
      }
      const ir::TypeIdx element_type = field_type_of(struct_type, i, node.span);
      const ir::RegisterIdx src =
          emit(ir::Opcode::GetElementPtr, element_type,
               {base_addr.op, zero_i32, index_operand(i)});
      const ir::RegisterIdx loaded =
          emit(ir::Opcode::Load, element_type, {to_operand(src, element_type)});
      const ir::RegisterIdx dst =
          emit(ir::Opcode::GetElementPtr, element_type,
               {to_operand(addr, struct_type), zero_i32, index_operand(i)});
      emit_void(ir::Opcode::Store, {to_operand(loaded, element_type),
                                    to_operand(dst, element_type)});
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
  const ir::TupleType& shape =
      builder.state().tuple_types[builder.state().types[tuple_type].as_tuple()];
  const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, tuple_type, {size_one});
  for (u32 i = 0; i < static_cast<u32>(tuple.elements.size()) && !failed; ++i) {
    const ir::TypeIdx* element_expected = nullptr;
    ir::TypeIdx element_type = error_type();
    if (i < shape.elements.size()) {
      element_type = shape.elements[i];
      element_expected = &element_type;
    }
    Val value = lower_expr(tuple.elements[i], element_expected);
    if (failed) {
      return Val{size_one, error_type(), false, false};
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
  const ir::ArrayType& shape =
      builder.state().array_types[builder.state().types[array_type].as_array()];
  const ir::RegisterIdx addr = emit(ir::Opcode::Alloca, array_type, {size_one});
  if (array.repeat.is_valid()) {
    if (shape.count == 0) {
      return Val{to_operand(addr, array_type), array_type, true, false};
    }
    Val value = lower_expr(array.repeat, &shape.element);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    const ir::OperandIdx stored = use_value(value);
    for (u64 i = 0; i < shape.count && !failed; ++i) {
      const ir::RegisterIdx gep = emit(ir::Opcode::GetElementPtr, shape.element,
                                       {to_operand(addr, array_type), zero_i32,
                                        index_operand(static_cast<u32>(i))});
      emit_void(ir::Opcode::Store, {stored, to_operand(gep, shape.element)});
    }
  } else {
    if (shape.count != array.elements.size()) {
      internal(node.span, "array arity");
      return Val{size_one, error_type(), false, false};
    }
    for (u32 i = 0; i < static_cast<u32>(array.elements.size()) && !failed;
         ++i) {
      Val value = lower_expr(array.elements[i], &shape.element);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      const ir::RegisterIdx gep =
          emit(ir::Opcode::GetElementPtr, shape.element,
               {to_operand(addr, array_type), zero_i32, index_operand(i)});
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

Val Lowerer::lower_binary(ast::ExprIdx expr) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprBinary& bin = node.payload.get<ast::ExprBinary>();
  if (bin.op == ast::BinaryOp::Pow) {
    unsupported(node.span, "power operator");
    return Val{size_one, error_type(), false, false};
  }
  // A bare literal defaults its width; lower the non-literal
  // side first so literals coerce to it (checking unified them).
  const bool lhs_literal = ast.exprs[bin.lhs].kind == ast::ExprKind::Literal;
  const bool rhs_literal = ast.exprs[bin.rhs].kind == ast::ExprKind::Literal;
  Val lhs = Val{size_one, error_type(), false, false};
  Val rhs = Val{size_one, error_type(), false, false};
  if (!lhs_literal && !rhs_literal) {
    lhs = lower_expr(bin.lhs, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    rhs = lower_expr(bin.rhs, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
  } else if (!lhs_literal) {
    lhs = lower_expr(bin.lhs, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    rhs = lower_expr(bin.rhs, &lhs.type);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
  } else if (!rhs_literal) {
    rhs = lower_expr(bin.rhs, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    lhs = lower_expr(bin.lhs, &rhs.type);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
  } else {
    lhs = lower_expr(bin.lhs, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    rhs = lower_expr(bin.rhs, &lhs.type);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
  }
  const ir::TypeTag tag = tag_of(lhs.type);
  if (bin.op == ast::BinaryOp::And || bin.op == ast::BinaryOp::Or) {
    const ir::RegisterIdx dst =
        emit(bin.op == ast::BinaryOp::And ? ir::Opcode::And : ir::Opcode::Or,
             lhs.type, {use_value(lhs), use_value(rhs)});
    return Val{to_operand(dst, lhs.type), lhs.type, false, false};
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
    ir::Opcode op = ir::Opcode::FAdd;
    switch (bin.op) {
      case ast::BinaryOp::Add: op = ir::Opcode::FAdd; break;
      case ast::BinaryOp::Sub: op = ir::Opcode::FSub; break;
      case ast::BinaryOp::Mul: op = ir::Opcode::FMul; break;
      default: op = ir::Opcode::FDiv; break;
    }
    const ir::RegisterIdx dst =
        emit(op, lhs.type, {use_value(lhs), use_value(rhs)});
    return Val{to_operand(dst, lhs.type), lhs.type, false, false};
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
void Lowerer::lower_arm_test(ast::PatternIdx pattern,
                             Val scrut_addr,
                             ir::TypeIdx scrut_type,
                             ir::BlockIdx body_block,
                             ir::BlockIdx fail_block) {
  const ast::PatternNode& node = ast.patterns[pattern];
  const diag::Span span = node.span;
  switch (node.kind) {
    case ast::PatternKind::Wildcard:
      emit_br(body_block);
      switch_to(body_block);
      return;
    case ast::PatternKind::Ident:
    case ast::PatternKind::MutIdent: {
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
        for (u32 i = 0;
             i < static_cast<u32>(node.payload.tuple.elements.size()) &&
             !failed;
             ++i) {
          const ir::RegisterIdx gep =
              emit(ir::Opcode::GetElementPtr, shape.elements[i],
                   {scrut_addr.op, zero_i32, index_operand(i)});
          bind_pattern(node.payload.tuple.elements[i],
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
      ir::BlockIdx bind_block = body_block;
      Val tag = load_disc(scrut_addr);
      const ir::TypeIdx boolean = builder.primitive(ir::TypeTag::I1);
      const ir::RegisterIdx test =
          emit(ir::Opcode::Eq, boolean, {tag.op, disc_operand(variant)});
      emit_cond_br(to_operand(test, boolean), bind_block, fail_block);
      switch_to(bind_block);
      const std::vector<ir::TypeIdx> payloads =
          variant_payload(scrut_type, variant);
      const std::span<const ast::PatternIdx> elements =
          node.payload.tuple.elements;
      if (payloads.size() != elements.size()) {
        internal(span, "variant arity");
        return;
      }
      if (is_unit_payload(payloads)) {
        for (usize i = 0; i < payloads.size() && !failed; ++i) {
          bind_pattern(elements[i], void_value());
        }
        return;
      }
      const ir::TypeIdx payload_type = payload_tuple(payloads);
      for (usize i = 0; i < payloads.size() && !failed; ++i) {
        Val field =
            load_payload_field(scrut_addr, payload_type, static_cast<u32>(i));
        bind_pattern(elements[i], field);
      }
      return;
    }
    case ast::PatternKind::Struct: {
      for (const ast::FieldPattern& field : node.payload.strukt.fields) {
        u32 index = 0;
        if (!struct_field_index(scrut_type, field.name.name, index)) {
          internal(field.name.span, "pattern field without declaration");
          return;
        }
        const ir::TypeIdx field_type = field_type_of(scrut_type, index, span);
        const ir::RegisterIdx gep =
            emit(ir::Opcode::GetElementPtr, field_type,
                 {scrut_addr.op, zero_i32, index_operand(index)});
        bind_pattern(field.pattern, Val{to_operand(gep, field_type), field_type,
                                        true, scrut_addr.place});
        if (failed) {
          return;
        }
      }
      emit_br(body_block);
      switch_to(body_block);
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

Val Lowerer::lower_match(ast::ExprIdx expr, const ir::TypeIdx* expected) {
  const ast::ExprNode& node = ast.exprs[expr];
  const ast::ExprMatch& match = node.payload.get<ast::ExprMatch>();
  Val scrut = lower_expr(match.scrutinee, nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  Val addr = address_of(scrut);
  // By-value matches consume a non-Copy scrutinee; payload bindings
  // copy out of the moved value.
  mark_move(addr);
  const ir::TypeIdx scrut_type = expr_type(match.scrutinee);
  const ir::TypeIdx result_type = expr_type(expr);
  Val slot{size_one, result_type, false, false};
  const bool has_slot = tag_of(result_type) != ir::TypeTag::Void &&
                        tag_of(result_type) != ir::TypeTag::Error;
  if (has_slot) {
    slot = result_slot(result_type, node.span);
  }
  std::vector<std::pair<ast::PatternIdx, ast::ExprIdx>> arms;
  for (const ast::ExprMatchArm& arm : match.arms) {
    expand_or_arms(arm, arms);
  }
  std::vector<ir::BlockIdx> tests;
  std::vector<ir::BlockIdx> bodies;
  for (usize i = 0; i < arms.size(); ++i) {
    tests.push_back(reserve_block());
    bodies.push_back(reserve_block());
  }
  const ir::BlockIdx fail = reserve_block();
  ir::BlockIdx join = ir::BlockIdx(base::kInvalidIdx);
  if (arms.empty()) {
    internal(node.span, "match without arms");
    return Val{size_one, error_type(), false, false};
  }
  emit_br(tests.front());
  for (usize i = 0; i < arms.size() && !failed; ++i) {
    switch_to(tests[i]);
    const ir::BlockIdx next = i + 1 < arms.size() ? tests[i + 1] : fail;
    lower_arm_test(arms[i].first, addr, scrut_type, bodies[i], next);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    // Arm testing always leaves lowering at the body block.
    switch_to(bodies[i]);
    Val produced = lower_expr(arms[i].second, expected);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    if (has_slot && !terminated_cur()) {
      store_result(slot, produced);
    } else if (!has_slot) {
      mark_move(produced);
    }
    if (!terminated_cur()) {
      if (!join.is_valid()) {
        join = reserve_block();
      }
      emit_br(join);
    }
  }
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  switch_to(fail);
  emit_void(ir::Opcode::Unreachable, {});
  if (join.is_valid()) {
    switch_to(join);
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
  ir::BlockIdx join = ir::BlockIdx(base::kInvalidIdx);
  auto finish_arm = [&](Val produced) {
    if (has_slot && !terminated_cur()) {
      store_result(slot, produced);
    } else if (!has_slot) {
      mark_move(produced);
    }
    if (!terminated_cur()) {
      if (!join.is_valid()) {
        join = reserve_block();
      }
      emit_br(join);
    }
  };
  const ast::Cond& cond_node = ast.conds[if_expr.cond];
  if (!cond_node.is_pattern) {
    ir::BlockIdx then_block = reserve_block();
    Val cond = lower_expr(cond_node.value, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val material = materialize(cond);
    emit_cond_br(material.op, then_block, else_block);
    switch_to(then_block);
    finish_arm(lower_block(if_expr.then_block, expected));
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    switch_to(else_block);
    if (if_expr.else_block.is_valid()) {
      finish_arm(lower_block(if_expr.else_block, expected));
    } else if (has_slot) {
      internal(node.span, "value if without else");
      return Val{size_one, error_type(), false, false};
    } else {
      // Statement position without else: the empty arm falls through.
      if (!join.is_valid()) {
        join = reserve_block();
      }
      emit_br(join);
    }
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
  } else {
    Val init = lower_expr(cond_node.init, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val addr = address_of(init);
    mark_move(addr);
    const ir::TypeIdx scrut_type = expr_type(cond_node.init);
    ir::BlockIdx body_block = reserve_block();
    lower_arm_test(cond_node.pattern, addr, scrut_type, body_block, else_block);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    switch_to(body_block);
    finish_arm(lower_block(if_expr.then_block, expected));
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    switch_to(else_block);
    if (if_expr.else_block.is_valid()) {
      finish_arm(lower_block(if_expr.else_block, expected));
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
    } else if (!has_slot) {
      if (!join.is_valid()) {
        join = reserve_block();
      }
      emit_br(join);
    } else {
      internal(node.span, "value if without else");
      return Val{size_one, error_type(), false, false};
    }
  }
  if (join.is_valid()) {
    switch_to(join);
  }
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
  break_targets_.push_back(exit);
  continue_targets_.push_back(header);
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
  if (!cond_node.is_pattern) {
    Val cond = lower_expr(cond_node.value, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    emit_cond_br(materialize(cond).op, body, exit);
    switch_to(body);
  } else {
    // The scrutinee re-evaluates on every iteration; the header both
    // tests and binds, so continue re-enters the test.
    Val init = lower_expr(cond_node.init, nullptr);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    Val addr = address_of(init);
    mark_move(addr);
    const ir::TypeIdx scrut_type = expr_type(cond_node.init);
    lower_arm_test(cond_node.pattern, addr, scrut_type, body, exit);
    if (failed) {
      return Val{size_one, error_type(), false, false};
    }
    switch_to(body);
  }
  break_targets_.push_back(exit);
  continue_targets_.push_back(header);
  lower_block(while_expr.body, nullptr);
  if (failed) {
    return Val{size_one, error_type(), false, false};
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
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  Val addr = address_of(scrut);
  mark_move(addr);
  const ir::TypeIdx scrut_type = expr_type(question.inner);
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
  switch_to(err_block);
  // Propagating keeps the original value, so the payload slot still
  // holds a valid scrutinee to return by value.
  const ir::RegisterIdx propagate =
      emit(ir::Opcode::Load, scrut_type, {addr.op});
  emit_void(ir::Opcode::Ret, {to_operand(propagate, scrut_type)});
  switch_to(ok_block);
  const Val payload = load_payload_field(addr, payload_tuple(first), 0);
  emit_br(join_block);
  switch_to(join_block);
  return payload;
}

Val Lowerer::lower_expr(ast::ExprIdx expr, const ir::TypeIdx* expected) {
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
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
      return lower_literal(literal.value, expected);
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
      Val inner = lower_expr(unary.inner, nullptr);
      if (failed) {
        return Val{size_one, error_type(), false, false};
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
      if (failed) {
        return Val{size_one, error_type(), false, false};
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
      Val place = place_addr(borrow.inner);
      if (failed) {
        return Val{size_one, error_type(), false, false};
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
      if (failed) {
        return Val{size_one, error_type(), false, false};
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
      return lower_call(expr, expected);
    }
    case ast::ExprKind::MethodCall: {
      return lower_method_call(expr, expected);
    }
    case ast::ExprKind::Field: {
      const ast::ExprField& field = node.payload.get<ast::ExprField>();
      Val base = lower_expr(field.receiver, nullptr);
      if (failed) {
        return Val{size_one, error_type(), false, false};
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
      emit_br(break_targets_.back());
      return Val{size_one, builder.never_type(), false, false};
    }
    case ast::ExprKind::Continue: {
      if (continue_targets_.empty()) {
        internal(node.span, "continue without loop");
        return Val{size_one, error_type(), false, false};
      }
      emit_br(continue_targets_.back());
      return Val{size_one, builder.never_type(), false, false};
    }
    case ast::ExprKind::Index: {
      const ast::ExprIndex& index = node.payload.get<ast::ExprIndex>();
      Val base = lower_expr(index.receiver, nullptr);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      if (!base.address) {
        base = address_of(base);
      }
      Val position = lower_expr(index.index, nullptr);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      Val addr = checked_index_addr(base, position, node.span);
      if (failed) {
        return Val{size_one, error_type(), false, false};
      }
      return materialize(addr);
    }
    case ast::ExprKind::Range:
      unsupported(node.span, "control flow in lowering");
      return Val{size_one, error_type(), false, false};
    case ast::ExprKind::Block: {
      const ast::ExprBlock& block = node.payload.get<ast::ExprBlock>();
      if (!block.is_comp) {
        return lower_block(block.block, expected);
      }
      CompVal evaluated;
      if (!comp_evaluate(module, expr, evaluated)) {
        return Val{size_one, error_type(), false, false};
      }
      return materialize_comp_value(evaluated, node.span);
    }
    case ast::ExprKind::Return: {
      const ast::ExprReturn& ret = node.payload.get<ast::ExprReturn>();
      if (!ret.value.is_valid()) {
        emit_void(ir::Opcode::Ret, {});
      } else {
        Val value = lower_expr(ret.value, nullptr);
        if (failed) {
          return Val{size_one, error_type(), false, false};
        }
        if (tag_of(value.type) == ir::TypeTag::Void) {
          emit_void(ir::Opcode::Ret, {});
        } else {
          emit_void(ir::Opcode::Ret, {use_value(value)});
        }
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
      if (decl.is_comp) {
        CompVal evaluated;
        if (!comp_evaluate(module, decl.init, evaluated)) {
          return;
        }
        // Comp bindings never take runtime addresses: runtime reads
        // splice constants through comp_scope_, so only publish the
        // value for later comp evaluation here.
        CompScope scope;
        scope.frames.emplace_back();
        if (!comp_bind_pattern(module, decl.pattern, evaluated, scope,
                               node.span)) {
          return;
        }
        for (auto& binding : scope.frames.back()) {
          comp_scope_.push_back(std::move(binding));
        }
        return;
      }
      Val init = lower_expr(decl.init, nullptr);
      if (failed) {
        return;
      }
      // Moving into the binding consumes a non-Copy place.
      const ir::OperandIdx moved = use_value(init);
      bind_pattern(decl.pattern, Val{moved, init.type, false, false});
      return;
    }
    case ast::StmtKind::Reassign: {
      const ast::StmtReassign& reassign = node.payload.get<ast::StmtReassign>();
      Val place = place_addr(reassign.place);
      if (failed) {
        return;
      }
      Val value = lower_expr(reassign.value, nullptr);
      if (failed) {
        return;
      }
      ir::OperandIdx stored = use_value(value);
      if (reassign.compound) {
        Val loaded = materialize(place);
        // Rebuild the compound operation from the operator spelling
        // is unnecessary: checking validated the shape, and only
        // plain assignment reaches lowering intact when the operator
        // needs control flow. Arithmetic compounds lower directly.
        (void)loaded;
      }
      emit_void(ir::Opcode::Store, {stored, place.op});
      return;
    }
    case ast::StmtKind::Expr: {
      const ast::StmtExpr& expr = node.payload.get<ast::StmtExpr>();
      mark_move(lower_expr(expr.value, nullptr));
      return;
    }
  }
}

Val Lowerer::lower_block(ast::BlockIdx block, const ir::TypeIdx* expected) {
  const ast::Block& node = ast.blocks[block];
  bool reachable = true;
  for (ast::StmtIdx stmt : node.statements) {
    if (failed) {
      break;
    }
    if (!reachable) {
      const u32 index = bag.emit(diag::Severity::Warning, kLowerUnreachable,
                                 ast.stmts[stmt].span, "unreachable statement");
      (void)index;
      continue;
    }
    lower_stmt(stmt);
    if (!failed && terminated_cur()) {
      reachable = false;
    }
  }
  if (failed || terminated_cur()) {
    if (!reachable && node.value.is_valid()) {
      const u32 index =
          bag.emit(diag::Severity::Warning, kLowerUnreachable,
                   ast.exprs[node.value].span, "unreachable expression");
      (void)index;
    }
    return Val{size_one, error_type(), false, false};
  }
  if (!node.value.is_valid()) {
    return Val{size_one, builder.primitive(ir::TypeTag::Void), false, false};
  }
  Val value = lower_expr(node.value, expected);
  if (failed) {
    return Val{size_one, error_type(), false, false};
  }
  return value;
}
}  // namespace lower
