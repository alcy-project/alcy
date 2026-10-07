// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/serialize.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "diag/span.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/str/string_interner.h"
#include "fpag/str/string_pool_id.h"
#include "ir/binary_format.h"
#include "ir/common.h"
#include "ir/external_function.h"
#include "ir/function.h"
#include "ir/opcode.h"
#include "ir/operand.h"
#include "ir/seq_builder.h"
#include "ir/storage.h"
#include "ir/storage_builder.h"
#include "ir/type.h"
#include "ir/write_input.h"

namespace ir {

namespace {

u16 read_u16(std::span<const u8> bytes, usize at) {
  return static_cast<u16>(bytes[at]) | static_cast<u16>(bytes[at + 1]) << 8;
}

u32 read_u32(std::span<const u8> bytes, usize at) {
  u32 value = 0;
  for (u32 i = 0; i < 4; ++i) {
    value |= static_cast<u32>(bytes[at + i]) << (8 * i);
  }
  return value;
}

u64 read_u64(std::span<const u8> bytes, usize at) {
  u64 value = 0;
  for (u32 i = 0; i < 8; ++i) {
    value |= static_cast<u64>(bytes[at + i]) << (8 * i);
  }
  return value;
}

u64 fnv1a64(std::span<const u8> bytes) {
  u64 hash = 0xCBF29CE484222325ULL;
  for (const u8 byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001B3ULL;
  }
  return hash;
}

// A package with one function, one external, spans, an address name, and
// every composite table populated, so the writer's sections all carry a
// row.
Storage composite_storage(str::StringInterner& strings) {
  StorageBuilder builder;
  const TypeIdx i32 = builder.primitive(TypeTag::I32);
  const TypeIdx u32 = builder.primitive(TypeTag::U32);
  const TypeIdx ptr = builder.primitive(TypeTag::Ptr);
  const TypeIdx str = builder.primitive(TypeTag::Str);
  const TypeIdx void_ty = builder.primitive(TypeTag::Void);

  TypeSeq point_fields;
  point_fields.push(builder.ref_type(i32));
  point_fields.push(builder.ref_type(i32));
  const TypeIdx point = builder.struct_type(
      strings.intern("Point"), point_fields.finish(), TypeIdxRange{});
  (void)point;

  EnumVariantTypeSeq variants;
  variants.push(builder.enum_variant(strings.intern("Circle"), {i32, 1}));
  variants.push(builder.enum_variant(strings.intern("Rect"), {}));
  const TypeIdx shape = builder.enum_type(strings.intern("Shape"),
                                          variants.finish(), TypeIdxRange{});
  (void)shape;

  (void)builder.array_type(i32, 4);
  (void)builder.slice_type(i32);
  (void)builder.reference_type(i32, false);
  (void)builder.raw_pointer_type(i32, true);
  TypeSeq tuple_elements;
  tuple_elements.push(builder.ref_type(i32));
  tuple_elements.push(builder.ref_type(str));
  (void)builder.tuple_type(tuple_elements.finish());
  TypeSeq fn_params;
  fn_params.push(builder.ref_type(i32));
  (void)builder.func_type(fn_params.finish(), i32);

  TypeSeq print_params;
  print_params.push(builder.ref_type(ptr));
  print_params.push(builder.ref_type(u32));
  builder.external_function({
      .meta = {.return_type = void_ty,
               .param_types = print_params.finish(),
               .name = strings.intern("alcy_print"),
               .path = str::EMPTY_STRING_ID,
               .kind = SymbolKind::Foreign,
               .generics = TypeIdxRange{}},
      .calling_conv = CallingConvention::C,
  });

  const ImmutableIdx one =
      builder.immutable({.type = i32, .data = {.i32_value = 1}});
  const ImmutableIdx five =
      builder.immutable({.type = i32, .data = {.i32_value = 5}});
  const OperandIdx one_op = builder.operand(Operand::from_immutable(one, i32));
  const OperandIdx five_op =
      builder.operand(Operand::from_immutable(five, i32));

  const RegisterIdx v0(0);
  const RegisterIdx v1(1);
  const InstructionIdx alloc = builder.instr({
      .op = Opcode::Alloca,
      .flags = {},
      .dst = v0,
      .measure = TypeIdx::invalid(),
      .operands = {one_op, 1},
  });
  const OperandIdx alloc_ptr = builder.operand(Operand::from_register(v0, i32));
  builder.operand(Operand::from_register(v0, i32));
  builder.instr({
      .op = Opcode::Store,
      .flags = {},
      .dst = RegisterIdx::invalid(),
      .measure = TypeIdx::invalid(),
      .operands = {five_op, 2},
  });
  const InstructionIdx load = builder.instr({
      .op = Opcode::Load,
      .flags = {},
      .dst = v1,
      .measure = TypeIdx::invalid(),
      .operands = {alloc_ptr, 1},
  });
  const OperandIdx ret_v1 = builder.operand(Operand::from_register(v1, i32));
  builder.instr({
      .op = Opcode::Ret,
      .flags = {},
      .dst = RegisterIdx::invalid(),
      .measure = TypeIdx::invalid(),
      .operands = {ret_v1, 1},
  });

  builder.reg({.type = i32, .def_idx = alloc});
  builder.reg({.type = i32, .def_idx = load});
  const BlockIdx b0 = builder.block({.instrs = {alloc, 4}, .block_params = {}});
  const FunctionMeta meta{
      .return_type = i32,
      .param_types = {},
      .name = strings.intern("main"),
      .path = str::EMPTY_STRING_ID,
      .kind = SymbolKind::Free,
      .generics = TypeIdxRange{},
  };
  builder.function({.meta = meta, .blocks = {b0, 1}});
  return std::move(builder).build().unwrap().unwrap();
}

std::vector<u8> serialize_fixture(str::StringInterner& strings,
                                  const Storage& storage) {
  const std::array<std::string_view, 1> names{"main.al"};
  const std::array<AddrName, 1> addr_names{
      AddrName{RegisterIdx(0), "x", /*is_param=*/false, /*is_capture=*/false}};
  const std::array<diag::Span, 4> spans{
      diag::Span{.file = 0, .offset = 1, .length = 2},
      diag::Span{.file = 0, .offset = 3, .length = 4},
      diag::Span{.file = 0, .offset = 5, .length = 6},
      diag::Span{.file = 0, .offset = 7, .length = 8},
  };
  const WriteInput input{
      .storage = &storage,
      .strings = &strings,
      .instr_spans = spans,
      .files = FileTable{.names = names, .hashes = {}},
      .addr_names = addr_names,
      .prelude_functions = 0,
      .width = PointerWidth::W64,
      .compiler_version = "0.0.0-test",
  };
  return serialize(input);
}

}  // namespace

TEST_CASE("The binary form frames its sections and hashes its bytes") {
  str::StringInterner strings;
  const Storage storage = composite_storage(strings);
  const std::vector<u8> bytes = serialize_fixture(strings, storage);
  const std::span<const u8> file(bytes);

  CHECK(bytes.size() > 64);
  CHECK(std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) ==
        "ALIR");
  const usize version_len = read_u32(file, 16);
  CHECK(read_u16(file, 4) == binary::MAJOR);
  CHECK(read_u16(file, 6) == binary::MINOR);
  CHECK(read_u32(file, 8) == binary::FLAG_LITTLE_ENDIAN);
  CHECK(read_u32(file, 12) == 64);
  CHECK(std::string_view(reinterpret_cast<const char*>(bytes.data() + 20),
                         version_len) == "0.0.0-test");

  const usize table_at = 20 + version_len + 8;
  const usize count = read_u64(file, 20 + version_len);
  CHECK(count == 21);
  // The kinds are the format's, in order, and nothing overlaps.
  usize previous_end = table_at + count * 24;
  for (usize i = 0; i < count; ++i) {
    const usize at = table_at + i * 24;
    CHECK(read_u32(file, at) == i + 1);
    CHECK(read_u32(file, at + 4) == 0);
    const u64 offset = read_u64(file, at + 8);
    const u64 size = read_u64(file, at + 16);
    CHECK(offset % binary::SECTION_ALIGN == 0);
    CHECK(offset >= previous_end);
    CHECK(offset + size <= bytes.size() - 8);
    previous_end = offset + size;
  }

  // The footer is the writer's hash of everything before it.
  CHECK(read_u64(file, bytes.size() - 8) ==
        fnv1a64(file.first(bytes.size() - 8)));
}

TEST_CASE("The binary form holds the strings the tables reference") {
  str::StringInterner strings;
  const Storage storage = composite_storage(strings);
  const std::vector<u8> bytes = serialize_fixture(strings, storage);
  const std::span<const u8> file(bytes);

  const usize version_len = read_u32(file, 16);
  const usize table_at = 20 + version_len + 8;
  const usize strings_at = static_cast<usize>(read_u64(file, table_at + 8));
  CHECK(read_u32(file, strings_at) >= 6);
  const usize strings_count = read_u32(file, strings_at);
  std::vector<std::string> found;
  usize at = strings_at + 4;
  for (usize i = 0; i < strings_count; ++i) {
    const usize length = read_u32(file, at);
    at += 4;
    found.emplace_back(reinterpret_cast<const char*>(bytes.data() + at),
                       length);
    at += length;
  }
  const auto has = [&found](std::string_view want) {
    return std::find(found.begin(), found.end(), want) != found.end();
  };
  CHECK(has("main"));
  CHECK(has("alcy_print"));
  CHECK(has("Point"));
  CHECK(has("Shape"));
  CHECK(has("Circle"));
  CHECK(has("x"));
  CHECK(has("main.al"));
}

TEST_CASE("The same package serializes to the same bytes") {
  str::StringInterner strings;
  const Storage storage = composite_storage(strings);
  const std::vector<u8> first = serialize_fixture(strings, storage);
  const std::vector<u8> second = serialize_fixture(strings, storage);
  CHECK(first == second);
}

}  // namespace ir
