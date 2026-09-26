// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdint>
#include <utility>

#include "doctest/doctest.h"
#include "ir/common.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"

namespace ir {

namespace {

TypeLayout layout_of(const StorageState& state,
                     TypeIdx idx,
                     PointerWidth width) {
  return type_layout(state, idx, width);
}

}  // namespace

TEST_CASE("Layout gives primitives their natural size and alignment") {
  StorageBuilder builder;
  Storage storage = std::move(builder).build().unwrap().unwrap();
  const StorageState& state = storage.state();

  CHECK(layout_of(state, primitive_idx(TypeTag::I1), PointerWidth::W64).size ==
        1);
  CHECK(layout_of(state, primitive_idx(TypeTag::U8), PointerWidth::W64).align ==
        1);
  CHECK(layout_of(state, primitive_idx(TypeTag::I16), PointerWidth::W64).size ==
        2);
  CHECK(
      layout_of(state, primitive_idx(TypeTag::U32), PointerWidth::W64).align ==
      4);
  CHECK(layout_of(state, primitive_idx(TypeTag::I64), PointerWidth::W64).size ==
        8);
  CHECK(
      layout_of(state, primitive_idx(TypeTag::F64), PointerWidth::W64).align ==
      8);
  // Uninhabited: a `()` field takes no room.
  CHECK(
      layout_of(state, primitive_idx(TypeTag::Void), PointerWidth::W64).size ==
      0);
}

TEST_CASE("Layout follows the target pointer width") {
  StorageBuilder builder;
  Storage storage = std::move(builder).build().unwrap().unwrap();
  const StorageState& state = storage.state();
  const TypeIdx ptr = primitive_idx(TypeTag::Ptr);
  const TypeIdx str = primitive_idx(TypeTag::Str);

  CHECK(layout_of(state, ptr, PointerWidth::W64).size == 8);
  CHECK(layout_of(state, ptr, PointerWidth::W32).size == 4);
  // Fat pointer: {bytes, len}.
  CHECK(layout_of(state, str, PointerWidth::W64).size == 16);
  CHECK(layout_of(state, str, PointerWidth::W32).size == 8);
}

TEST_CASE("Layout pads a struct to its strictest field") {
  StorageBuilder builder;
  const TypeIdx i8_ty = builder.primitive(TypeTag::I8);
  const TypeIdx i32_ty = builder.primitive(TypeTag::I32);
  const TypeIdx i64_ty = builder.primitive(TypeTag::I64);

  // Three bytes then an i32: the i32 sits at offset 4, not 3.
  TypeSeq fields;
  fields.push(builder.ref_type(i8_ty));
  fields.push(builder.ref_type(i8_ty));
  fields.push(builder.ref_type(i8_ty));
  fields.push(builder.ref_type(i32_ty));
  const TypeIdx padded = builder.struct_type(str::kInvalidStringPoolId,
                                             fields.finish(), TypeIdxRange{});

  // An i64 first forces everything up to a multiple of eight.
  TypeSeq wide;
  wide.push(builder.ref_type(i64_ty));
  wide.push(builder.ref_type(i8_ty));
  const TypeIdx wide_struct = builder.struct_type(
      str::kInvalidStringPoolId, wide.finish(), TypeIdxRange{});

  Storage storage = std::move(builder).build().unwrap().unwrap();
  const StorageState& state = storage.state();

  const TypeLayout padded_layout = layout_of(state, padded, PointerWidth::W64);
  CHECK(padded_layout.align == 4);
  CHECK(padded_layout.size == 8);

  const TypeLayout wide_layout =
      layout_of(state, wide_struct, PointerWidth::W64);
  CHECK(wide_layout.align == 8);
  CHECK(wide_layout.size == 16);
}

TEST_CASE("Layout sizes an array from its element") {
  StorageBuilder builder;
  const TypeIdx i32_ty = builder.primitive(TypeTag::I32);
  const TypeIdx arr = builder.array_type(i32_ty, 3);
  const TypeIdx empty = builder.array_type(i32_ty, 0);
  Storage storage = std::move(builder).build().unwrap().unwrap();
  const StorageState& state = storage.state();

  CHECK(layout_of(state, arr, PointerWidth::W64).size == 12);
  CHECK(layout_of(state, arr, PointerWidth::W64).align == 4);
  CHECK(layout_of(state, empty, PointerWidth::W64).size == 0);
}

TEST_CASE("Layout gives an enum a discriminant and a payload area") {
  StorageBuilder builder;
  const TypeIdx i32_ty = builder.primitive(TypeTag::I32);
  const TypeIdx ptr_ty = builder.primitive(TypeTag::Ptr);

  // `enum E { A(i32), B }`: discriminant plus a 4-byte area.
  TypeSeq narrow;
  narrow.push(builder.ref_type(i32_ty));
  const EnumVariantTypeIdx narrow_variant =
      builder.enum_variant(str::kInvalidStringPoolId, narrow.finish());
  const TypeIdx narrow_enum = builder.enum_type(
      str::kInvalidStringPoolId, EnumVariantTypeIdxRange{narrow_variant, 1},
      TypeIdxRange{});

  // `enum F { A(&mut i32) }`: the area has to be 8-aligned for a pointer.
  TypeSeq wide;
  wide.push(builder.ref_type(ptr_ty));
  const EnumVariantTypeIdx wide_variant =
      builder.enum_variant(str::kInvalidStringPoolId, wide.finish());
  const TypeIdx wide_enum = builder.enum_type(
      str::kInvalidStringPoolId, EnumVariantTypeIdxRange{wide_variant, 1},
      TypeIdxRange{});

  Storage storage = std::move(builder).build().unwrap().unwrap();
  const StorageState& state = storage.state();

  const TypeLayout narrow_layout =
      layout_of(state, narrow_enum, PointerWidth::W64);
  CHECK(narrow_layout.align == 4);
  CHECK(narrow_layout.size == 8);

  // The area starts at the next multiple of eight, so the pointer inside
  // it is aligned even though the discriminant is not.
  const TypeLayout wide_layout = layout_of(state, wide_enum, PointerWidth::W64);
  CHECK(wide_layout.align == 8);
  CHECK(wide_layout.size == 16);
}

TEST_CASE("Layout sizes an enum area for its widest variant") {
  StorageBuilder builder;
  const TypeIdx i32_ty = builder.primitive(TypeTag::I32);
  const TypeIdx ptr_ty = builder.primitive(TypeTag::Ptr);

  // `enum R { Ok(i32), Err(&mut i32) }`: the area must hold a pointer
  // even though the first variant only carries an i32.
  TypeSeq ok_fields;
  ok_fields.push(builder.ref_type(i32_ty));
  TypeSeq err_fields;
  err_fields.push(builder.ref_type(ptr_ty));
  const EnumVariantTypeIdx ok =
      builder.enum_variant(str::kInvalidStringPoolId, ok_fields.finish());
  const EnumVariantTypeIdx err =
      builder.enum_variant(str::kInvalidStringPoolId, err_fields.finish());
  // Variants land in declaration order, so `ok` then `err`.
  const TypeIdx both =
      builder.enum_type(str::kInvalidStringPoolId,
                        EnumVariantTypeIdxRange{ok, 2}, TypeIdxRange{});
  CHECK(err.idx == ok.idx + 1);

  Storage storage = std::move(builder).build().unwrap().unwrap();
  const TypeLayout both_layout =
      layout_of(storage.state(), both, PointerWidth::W64);
  CHECK(both_layout.align == 8);
  CHECK(both_layout.size == 16);
}

TEST_CASE("Layout gives a payload-less enum only its discriminant") {
  StorageBuilder builder;
  TypeSeq none;
  const EnumVariantTypeIdx unit =
      builder.enum_variant(str::kInvalidStringPoolId, none.finish());
  const TypeIdx tag_only =
      builder.enum_type(str::kInvalidStringPoolId,
                        EnumVariantTypeIdxRange{unit, 1}, TypeIdxRange{});
  Storage storage = std::move(builder).build().unwrap().unwrap();

  const TypeLayout layout =
      layout_of(storage.state(), tag_only, PointerWidth::W64);
  CHECK(layout.align == 4);
  CHECK(layout.size == 4);
}

TEST_CASE("Layout of a nested enum flows into its containing struct") {
  StorageBuilder builder;
  const TypeIdx i8_ty = builder.primitive(TypeTag::I8);
  const TypeIdx ptr_ty = builder.primitive(TypeTag::Ptr);

  TypeSeq wide_fields;
  wide_fields.push(builder.ref_type(ptr_ty));
  const EnumVariantTypeIdx wide_variant =
      builder.enum_variant(str::kInvalidStringPoolId, wide_fields.finish());
  const TypeIdx wide_enum = builder.enum_type(
      str::kInvalidStringPoolId, EnumVariantTypeIdxRange{wide_variant, 1},
      TypeIdxRange{});

  // A byte then that enum: the enum is 8-aligned, so the byte is padded
  // out to offset 8 and the struct ends at 24.
  TypeSeq fields;
  fields.push(builder.ref_type(i8_ty));
  fields.push(builder.ref_type(wide_enum));
  const TypeIdx outer = builder.struct_type(str::kInvalidStringPoolId,
                                            fields.finish(), TypeIdxRange{});

  Storage storage = std::move(builder).build().unwrap().unwrap();
  const TypeLayout outer_layout =
      layout_of(storage.state(), outer, PointerWidth::W64);
  CHECK(outer_layout.align == 8);
  CHECK(outer_layout.size == 24);
}

}  // namespace ir
