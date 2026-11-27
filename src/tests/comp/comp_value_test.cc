// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "comp/comp_value.h"

#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "ir/symbol_table.h"
#include "ir/type.h"
#include "text/unescape.h"

namespace comp {

namespace {

CompVal int_val(u64 bits) {
  CompVal out;
  out.value.tag = CompValue::Tag::Int;
  out.value.int_value = bits;
  return out;
}

}  // namespace

TEST_CASE("Comp integer predicates pin every declared width") {
  CHECK(is_signed(ir::TypeTag::I8));
  CHECK(is_signed(ir::TypeTag::I16));
  CHECK(is_signed(ir::TypeTag::I32));
  CHECK(is_signed(ir::TypeTag::I64));
  CHECK(is_signed(ir::TypeTag::Isize));
  CHECK(!is_signed(ir::TypeTag::U8));
  CHECK(!is_signed(ir::TypeTag::Rune));
  CHECK(!is_signed(ir::TypeTag::Usize));

  CHECK(int_bytes(ir::TypeTag::I8) == 1);
  CHECK(int_bytes(ir::TypeTag::U8) == 1);
  CHECK(int_bytes(ir::TypeTag::I16) == 2);
  CHECK(int_bytes(ir::TypeTag::U16) == 2);
  CHECK(int_bytes(ir::TypeTag::I32) == 4);
  CHECK(int_bytes(ir::TypeTag::U32) == 4);
  CHECK(int_bytes(ir::TypeTag::I64) == 8);
  CHECK(int_bytes(ir::TypeTag::U64) == 8);
  CHECK(int_bytes(ir::TypeTag::I1) == 8);

  CHECK(mask(ir::TypeTag::I8) == 0xFF);
  CHECK(mask(ir::TypeTag::U16) == 0xFFFF);
  CHECK(mask(ir::TypeTag::U32) == 0xFFFFFFFFull);
  CHECK(mask(ir::TypeTag::I64) == std::numeric_limits<u64>::max());

  CHECK(sign_extend(0xFF, ir::TypeTag::I8) == -1);
  CHECK(sign_extend(0x80, ir::TypeTag::I8) == -128);
  CHECK(sign_extend(0x7F, ir::TypeTag::I8) == 127);
  CHECK(sign_extend(0xFFFF, ir::TypeTag::U16) == -1);
  CHECK(sign_extend(0x8000000000000000ull, ir::TypeTag::I64) ==
        std::numeric_limits<i64>::min());
}

TEST_CASE("Comp overflow predicates agree with the type's range") {
  CHECK(add_overflows(127, 1, ir::TypeTag::I8));
  CHECK(add_overflows(255, 1, ir::TypeTag::U8));
  CHECK(add_overflows(0x7FFFFFFFFFFFFFFFull, 1, ir::TypeTag::Isize));
  CHECK(!add_overflows(0x8000000000000000ull, 1, ir::TypeTag::Isize));
  CHECK(!add_overflows(0xFFFFFFFFFFFFFFFFull, 1, ir::TypeTag::Isize));
  CHECK(!add_overflows(126, 1, ir::TypeTag::I8));
  CHECK(!add_overflows(254, 1, ir::TypeTag::U8));

  CHECK(sub_overflows(0, 1, ir::TypeTag::U8));
  CHECK(sub_overflows(0x80, 1, ir::TypeTag::I8));
  CHECK(!sub_overflows(255, 0, ir::TypeTag::U8));
  CHECK(!sub_overflows(1, 1, ir::TypeTag::I8));

  CHECK(mul_overflows(16, 16, ir::TypeTag::U8));
  CHECK(mul_overflows(100, 2, ir::TypeTag::I8));
  CHECK(!mul_overflows(127, 1, ir::TypeTag::I8));
  CHECK(!mul_overflows(0, 0xFF, ir::TypeTag::U64));
}

TEST_CASE("Comp literal parsing strips suffixes and bases") {
  bool overflow = false;
  CHECK(parse_numeric_value("123i32", &overflow) == 123);
  CHECK(!overflow);
  CHECK(parse_numeric_value("0xFF") == 255);
  CHECK(parse_numeric_value("0X1f") == 31);
  CHECK(parse_numeric_value("0b1010") == 10);
  CHECK(parse_numeric_value("0o17") == 15);
  CHECK(parse_numeric_value("1_000u32") == 1000);
  CHECK(parse_numeric_value("18446744073709551615", &overflow) ==
        std::numeric_limits<u64>::max());
  CHECK(!overflow);
  CHECK(parse_numeric_value("18446744073709551616", &overflow) == 0);
  CHECK(overflow);

  CHECK(has_type_suffix("123i32"));
  CHECK(has_type_suffix("1.5f64"));
  CHECK(has_type_suffix("9usize"));
  CHECK(!has_type_suffix("123"));
  CHECK(!has_type_suffix("i32"));
  CHECK(!has_type_suffix("0x1F"));

  CHECK(parse_f32_value("1_000.5e3f32") == 1000500.0f);
  CHECK(parse_f64_value("1.5e2f64") == 150.0);
  CHECK(parse_f32_value("f32") == 0.0f);
  CHECK(parse_f64_value("f64") == 0.0);
}

TEST_CASE("Comp scope shadows innermost and string_bytes clamps views") {
  CompVal x_outer = int_val(1);
  CompVal y_outer = int_val(2);
  std::vector<std::pair<std::string_view, CompVal>> outer = {{"x", x_outer},
                                                             {"y", y_outer}};
  CompVal x_frame0 = int_val(3);
  CompVal x_frame1 = int_val(4);
  CompVal z_frame1 = int_val(5);
  CompScope scope;
  scope.outer = &outer;
  scope.frames.push_back({{"x", x_frame0}});
  scope.frames.push_back({{"x", x_frame1}, {"z", z_frame1}});

  const CompVal* found = lookup(scope, "x");
  CHECK(found != nullptr);
  CHECK(found->value.int_value == 4);
  found = lookup(scope, "y");
  CHECK(found != nullptr);
  CHECK(found->value.int_value == 2);
  found = lookup(scope, "z");
  CHECK(found != nullptr);
  CHECK(found->value.int_value == 5);
  CHECK(lookup(scope, "missing") == nullptr);

  CompVal boolean;
  boolean.value.tag = CompValue::Tag::Bool;
  boolean.value.bool_value = false;
  CHECK(!truth(boolean));
  boolean.value.bool_value = true;
  CHECK(truth(boolean));
  CHECK(!truth(int_val(0)));
  CHECK(truth(int_val(2)));

  ir::SymbolTable strings;
  const std::optional<str::StringPoolId> maybe_text =
      strings.intern_copied("abc");
  CHECK(maybe_text.has_value());
  if (!maybe_text.has_value()) {
    return;
  }
  const str::StringPoolId text = *maybe_text;
  CompValue view;
  view.tag = CompValue::Tag::Str;
  view.str_id = text;
  CHECK(string_bytes(view, strings) == "abc");
  view.str_offset = 3;
  CHECK(string_bytes(view, strings).empty());
  view.str_offset = 2;
  view.str_length = 1;
  CHECK(string_bytes(view, strings) == "c");

  CompValue owned;
  owned.tag = CompValue::Tag::Str;
  owned.owned_str = "xyz";
  CHECK(string_bytes(owned, strings) == "xyz");

  CHECK(unescape("\"a\\nb\"") == text::unescape_string("\"a\\nb\""));
}

}  // namespace comp
