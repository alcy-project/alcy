// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/storage.h"

#include <algorithm>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "debug/fatal.h"
#include "fpag/base/numeric.h"
#include "fpag/base/vec.h"
#include "ir/common.h"
#include "ir/type.h"

namespace ir {

TypeIdx enum_place_type(const StorageState& state, TypeIdx idx) {
  if (!idx.is_valid() || idx.idx >= state.types.size()) {
    return idx;
  }
  const TypeNode& node = state.types[idx];
  if (node.tag == TypeTag::Ref || node.tag == TypeTag::MutRef) {
    return state.ref_types[node.as_ref()].pointee;
  }
  return idx;
}

EnumNiche enum_niche(const StorageState& state, TypeIdx idx) {
  EnumNiche niche;
  if (!idx.is_valid() || idx.idx >= state.types.size()) {
    return niche;
  }
  const TypeNode& node = state.types[idx];
  if (node.tag != TypeTag::Enum) {
    return niche;
  }
  const EnumType& shape = state.enum_types[node.as_enum()];
  if (shape.variants.size() != 2) {
    return niche;
  }
  bool found_carrying = false;
  for (u32 v = 0; v < 2; ++v) {
    const TypeIdxRange fields = state
                                    .enum_variant_types[EnumVariantTypeIdx(
                                        shape.variants.head().idx + v)]
                                    .fields;
    if (fields.size() == 0) {
      niche.fieldless = v;
    } else if (fields.size() == 1) {
      niche.carrying = v;
      niche.payload = fields[0];
      found_carrying = true;
    } else {
      return niche;
    }
  }
  if (!found_carrying) {
    return niche;
  }
  const TypeTag payload_tag = state.types[niche.payload].tag;
  if (payload_tag == TypeTag::Ref || payload_tag == TypeTag::MutRef) {
    // Only a thin reference has a spare null pattern. A reference to a
    // slice is fat - a data pointer and a length - and a fieldless
    // variant would have to spell a null pair, which no thin reference does not
    // promise and the lowering does not build. Such an enum carries a
    // discriminant like any other.
    const TypeNode& payload_node = state.types[niche.payload];
    const TypeNode& pointee =
        state.types[state.ref_types[payload_node.as_ref()].pointee];
    if (pointee.tag == TypeTag::Slice) {
      return niche;
    }
    niche.slot = niche.payload;
    niche.sentinel = 0;
    niche.has = true;
    return niche;
  }
  if (payload_tag == TypeTag::I1) {
    // A bool needs a third state and a bit has none: the slot is a
    // byte, zero or one is the payload, and two is the fieldless
    // variant.
    niche.slot = primitive_idx(TypeTag::U8);
    niche.sentinel = 2;
    niche.has = true;
    return niche;
  }
  return niche;
}

std::pair<const TypeLayoutHints*, u32> field_hints_of(const StorageState& state,
                                                      const StructType& shape) {
  const TypeLayoutHintsIdxRange& range = shape.field_hints;
  if (!range.empty() && static_cast<u64>(range.head().idx) + range.size() <=
                            state.layout_hints.size()) {
    return {&state.layout_hints[range.head().idx], range.size()};
  }
  return {nullptr, 0};
}

TypeLayout fields_layout(const StorageState& state,
                         TypeIdxRange fields,
                         PointerWidth width,
                         const TypeLayoutHints& hints,
                         const TypeLayoutHints* field_hints,
                         u32 field_hint_count) {
  TypeLayout out{};
  for (u32 i = 0; i < fields.size(); ++i) {
    const TypeIdx field = fields[i];
    TypeLayout field_layout = type_layout(state, field, width);
    if (field_hints != nullptr && i < field_hint_count) {
      const TypeLayoutHints& hint = field_hints[i];
      if (hint.max_align != 0 && field_layout.align > hint.max_align) {
        field_layout.align = hint.max_align;
      }
      if (hint.min_align != 0 && field_layout.align < hint.min_align) {
        field_layout.align = hint.min_align;
      }
    }
    out.size = align_up(out.size, field_layout.align) + field_layout.size;
    out.align = field_layout.align > out.align ? field_layout.align : out.align;
  }
  if (hints.min_align != 0 && out.align < hints.min_align) {
    out.align = hints.min_align;
  }
  if (hints.max_align != 0 && out.align > hints.max_align) {
    out.align = hints.max_align;
  }
  out.size = align_up(out.size, out.align);
  return out;
}

u64 field_offset(const StorageState& state,
                 TypeIdxRange fields,
                 u32 index,
                 PointerWidth width,
                 const TypeLayoutHints* hints,
                 u32 hint_count) {
  const auto effective = [&](u32 i) {
    TypeLayout layout = type_layout(state, fields[i], width);
    if (hints != nullptr && i < hint_count) {
      if (hints[i].max_align != 0 && layout.align > hints[i].max_align) {
        layout.align = hints[i].max_align;
      }
      if (hints[i].min_align != 0 && layout.align < hints[i].min_align) {
        layout.align = hints[i].min_align;
      }
    }
    return layout;
  };
  u64 cursor = 0;
  for (u32 i = 0; i < index; ++i) {
    const TypeLayout layout = effective(i);
    cursor = align_up(cursor, layout.align) + layout.size;
  }
  return align_up(cursor, effective(index).align);
}

u8 enum_tag_width(const StorageState& state, TypeIdx idx) {
  const EnumType& shape = state.enum_types[state.types[idx].as_enum()];
  if (shape.hints.tag_size != 0) {
    return shape.hints.tag_size;
  }
  if (shape.variants.size() <= 256) {
    return 1;
  }
  if (shape.variants.size() <= 65536) {
    return 2;
  }
  return 4;
}

bool enum_is_zst(const StorageState& state, TypeIdx idx) {
  const EnumType& shape = state.enum_types[state.types[idx].as_enum()];
  if (shape.variants.size() == 0) {
    return true;
  }
  if (shape.variants.size() != 1) {
    return false;
  }
  const EnumVariantType& only = state.enum_variant_types[shape.variants.head()];
  for (TypeIdx field : only.fields) {
    const TypeTag tag = state.types[field].tag;
    if (tag == TypeTag::Param || tag == TypeTag::Const ||
        type_layout(state, field, PointerWidth::W64).size != 0) {
      return false;
    }
  }
  return true;
}

TypeLayout enum_payload_area(const StorageState& state,
                             TypeIdx idx,
                             PointerWidth width) {
  u64 align = 1;
  u64 size = 0;
  const EnumType& enum_type = state.enum_types[state.types[idx].as_enum()];
  for (EnumVariantTypeIdx variant : enum_type.variants) {
    const TypeIdxRange fields = state.enum_variant_types[variant].fields;
    if (fields.empty()) {
      continue;
    }
    // One walk yields both numbers: sizing the fields and taking their
    // strictest alignment is the same loop, and running it twice made a
    // nesting of enums double the layout work per level.
    const TypeLayout variant_layout = fields_layout(state, fields, width);
    if (variant_layout.align > align) {
      align = variant_layout.align;
    }
    if (variant_layout.size > size) {
      size = variant_layout.size;
    }
  }
  return {align_up(size, align), align};
}

TypeLayout type_layout(const StorageState& state,
                       TypeIdx idx,
                       PointerWidth width) {
  const u64 word = width == PointerWidth::W64 ? 8 : 4;
  const TypeNode& node = state.types[idx];
  switch (node.tag) {
    // Uninhabited: a `()` field stores nothing, so it takes no room.
    case TypeTag::Void:
    case TypeTag::Never:
    case TypeTag::Error: return {};
    case TypeTag::I1: return {1, 1};
    case TypeTag::I8:
    case TypeTag::U8:
    case TypeTag::Char: return {1, 1};
    case TypeTag::I16:
    case TypeTag::U16: return {2, 2};
    case TypeTag::I32:
    case TypeTag::U32:
    case TypeTag::F32:
    case TypeTag::Rune: return {4, 4};
    case TypeTag::I64:
    case TypeTag::U64:
    case TypeTag::F64: return {8, 8};
    case TypeTag::Isize:
    case TypeTag::Usize: return {word, word};
    // Fat pointer: {bytes, len}, with the length riding the target.
    case TypeTag::Str: return {2 * word, word};
    case TypeTag::Slice: return {2 * word, word};
    case TypeTag::Ptr:
    case TypeTag::RawPtr:
    case TypeTag::RawMutPtr:
    case TypeTag::Function: return {word, word};
    case TypeTag::Func: {
      // Code plus environment, whatever the signature says: every
      // function value fits two words.
      return {2 * word, word};
    }
    case TypeTag::Ref:
    case TypeTag::MutRef: {
      // A reference to a slice is the fat pointer itself, passed by
      // value like `str`: borrowck owns the distinction, codegen sees
      // two words either way.
      const TypeNode& pointee =
          state.types[state.ref_types[node.as_ref()].pointee];
      if (pointee.tag == TypeTag::Slice) {
        return {2 * word, word};
      }
      return {word, word};
    }
    case TypeTag::Struct: {
      const StructType& shape = state.struct_types[node.as_struct()];
      const TypeLayoutHintsIdxRange& hints = shape.field_hints;
      const bool have_hints =
          !hints.empty() && static_cast<u64>(hints.head().idx) + hints.size() <=
                                state.layout_hints.size();
      return fields_layout(
          state, shape.fields, width, shape.hints,
          have_hints ? &state.layout_hints[hints.head().idx] : nullptr,
          have_hints ? hints.size() : 0);
    }
    case TypeTag::Tuple:
      return fields_layout(state, state.tuple_types[node.as_tuple()].elements,
                           width);
    case TypeTag::Array: {
      const ArrayType& array = state.array_types[node.as_array()];
      const TypeTag element_tag = state.types[array.element].tag;
      if (array.count == 0) {
        // No element is stored, but the type keeps its element's
        // alignment so a zero-length array still serves as an aligned
        // placeholder in a containing aggregate.
        const u64 align =
            element_tag == TypeTag::Param || element_tag == TypeTag::Const
                ? 1
                : type_layout(state, array.element, width).align;
        return {0, align};
      }
      const TypeLayout element = type_layout(state, array.element, width);
      return {element.size * array.count, element.align};
    }
    case TypeTag::Enum: {
      // No variant can hold a value, so there is no discriminant to
      // store. An empty enum is a zero-sized type like the unit and the
      // empty struct.
      const EnumType& shape = state.enum_types[state.types[idx].as_enum()];
      if (enum_is_zst(state, idx)) {
        TypeLayout empty{};
        if (shape.hints.min_align != 0 && empty.align < shape.hints.min_align) {
          empty.align = shape.hints.min_align;
        }
        return empty;
      }
      // A niche-carrying enum is its payload: the discriminant lives in
      // the payload's spare pattern and takes no room of its own
      const EnumNiche niche = enum_niche(state, idx);
      if (niche.has) {
        return type_layout(state, niche.slot, width);
      }
      // Discriminant, padded up to the payload area's alignment, then
      // the area itself, and the whole rounded up to the alignment as
      // every other aggregate is. The last step is not cosmetic: an
      // array of this enum strides by this size, and the LLVM struct the
      // emitter builds is allocated at its own alignment, so a size that
      // is not a multiple of it puts every element after the first at
      // the wrong offset. A byte payload is where the two part company -
      // `{i8, [1 x i8]}` is two bytes and allocates two.
      const u64 disc = enum_tag_width(state, idx);
      const TypeLayout area = enum_payload_area(state, idx, width);
      u64 align = area.align > disc ? area.align : disc;
      if (shape.hints.min_align != 0 && align < shape.hints.min_align) {
        align = shape.hints.min_align;
      }
      if (shape.hints.max_align != 0 && align > shape.hints.max_align) {
        align = shape.hints.max_align;
      }
      const u64 payload_start = align_up(disc, area.align);
      return {align_up(payload_start + area.size, align), align};
    }
    // A const argument is never a value, so no layout exists for it; a
    // reachable call would mean one leaked into a value position.
    case TypeTag::Const: UNREACHABLE();
    // A type parameter has no layout until the argument is known: the
    // size of the type it stands for is the size of that type. A
    // reachable call would mean an unspecialized body reached the
    // backend.
    case TypeTag::Param: UNREACHABLE();
  }
  UNREACHABLE();
}

bool is_copy_type(const StorageState& state,
                  TypeIdx idx,
                  std::span<const u8> has_drop,
                  std::span<const u8> not_movable) {
  std::vector<u32> inside;
  return is_copy_type(state, idx, has_drop, not_movable, inside);
}

bool is_copy_type(const StorageState& state,
                  TypeIdx idx,
                  std::span<const u8> has_drop,
                  std::span<const u8> not_movable,
                  std::vector<u32>& inside) {
  if (idx.idx < has_drop.size() && has_drop[idx.idx] != 0) {
    return false;
  }
  // A type that holds an anchor is not Copy: copying a value would
  // place it somewhere else, which is what the anchor forbids
  if (idx.idx < not_movable.size() && not_movable[idx.idx] != 0) {
    return false;
  }
  if (std::find(inside.begin(), inside.end(), idx.idx) != inside.end()) {
    return true;
  }
  const InsideScope record(inside, idx.idx);
  const TypeNode& node = state.types[idx];
  switch (node.tag) {
    case TypeTag::MutRef: return false;
    case TypeTag::Error: return true;
    // A const argument is never a value, so the question does not apply
    // to it; a reachable call is a bug, as in type_layout.
    case TypeTag::Const: UNREACHABLE();
    // A type parameter is not Copy: whether it is depends on the type
    // it stands for, and a body that has not been specialized cannot
    // know. The bound that says `T: Copy` is what settles it.
    case TypeTag::Param: return false;
    case TypeTag::Ref:
    case TypeTag::Void:
    case TypeTag::Never:
    case TypeTag::I1:
    case TypeTag::I8:
    case TypeTag::I16:
    case TypeTag::I32:
    case TypeTag::I64:
    case TypeTag::U8:
    case TypeTag::U16:
    case TypeTag::U32:
    case TypeTag::U64:
    case TypeTag::Isize:
    case TypeTag::Usize:
    case TypeTag::F32:
    case TypeTag::F64:
    case TypeTag::Str:
    case TypeTag::Char:
    case TypeTag::Rune:
    case TypeTag::Ptr:
    case TypeTag::RawPtr:
    case TypeTag::RawMutPtr:
    case TypeTag::Function: return true;
    case TypeTag::Struct: {
      const StructType& struct_type = state.struct_types[node.as_struct()];
      for (TypeIdx field : struct_type.fields) {
        if (!is_copy_type(state, field, has_drop, not_movable, inside)) {
          return false;
        }
      }
      return true;
    }
    case TypeTag::Array:
      return is_copy_type(state, state.array_types[node.as_array()].element,
                          has_drop, not_movable, inside);
    case TypeTag::Slice:
      return is_copy_type(state, state.slice_types[node.as_slice()].element,
                          has_drop, not_movable, inside);
    case TypeTag::Enum: {
      const EnumType& enum_type = state.enum_types[node.as_enum()];
      for (EnumVariantTypeIdx vidx = enum_type.variants.head();
           vidx.idx < enum_type.variants.head().idx + enum_type.variants.size();
           vidx = EnumVariantTypeIdx(vidx.idx + 1)) {
        const EnumVariantType& variant = state.enum_variant_types[vidx];
        for (TypeIdx field : variant.fields) {
          if (!is_copy_type(state, field, has_drop, not_movable, inside)) {
            return false;
          }
        }
      }
      return true;
    }
    case TypeTag::Tuple: {
      const TupleType& tuple = state.tuple_types[node.as_tuple()];
      for (TypeIdx element : tuple.elements) {
        if (!is_copy_type(state, element, has_drop, not_movable, inside)) {
          return false;
        }
      }
      return true;
    }
    case TypeTag::Func: {
      // Copies share the environment handle, and the environment
      // holds shared references, one exclusive borrow however many
      // handles reach it, or copied values. A non-Copy capture
      // would make its closure move-only, and that mode waits for
      // the owning environment.
      return true;
    }
    default: UNREACHABLE();
  }
}

bool is_copy_type(const StorageState& state, TypeIdx idx) {
  return is_copy_type(state, idx, std::span<const u8>{});
}

}  // namespace ir
