// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "ir/binary_format.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "tests/ir/ir_fixtures.h"

namespace ir {

TEST_CASE("The binary form frames its sections and hashes its bytes") {
  ir::SymbolTable strings;
  const Storage storage = test::composite_storage(strings);
  const std::vector<u8> bytes = test::composite_bytes(strings, storage);
  const std::span<const u8> file(bytes);

  CHECK(bytes.size() > 64);
  CHECK(std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) ==
        "ALIR");
  const usize version_len = test::read_u32(file, 16);
  CHECK(test::read_u16(file, 4) == binary::MAJOR);
  CHECK(test::read_u16(file, 6) == binary::MINOR);
  CHECK(test::read_u32(file, 8) == binary::FLAG_LITTLE_ENDIAN);
  CHECK(test::read_u32(file, 12) == 64);
  CHECK(std::string_view(reinterpret_cast<const char*>(bytes.data() + 20),
                         version_len) == "0.0.0-test");

  const usize table_at = 20 + version_len + 8;
  const usize count = test::read_u64(file, 20 + version_len);
  CHECK(count == 21);
  // The kinds are the format's, in order, and nothing overlaps.
  usize previous_end = table_at + count * 24;
  for (usize i = 0; i < count; ++i) {
    const usize at = table_at + i * 24;
    CHECK(test::read_u32(file, at) == i + 1);
    CHECK(test::read_u32(file, at + 4) == 0);
    const u64 offset = test::read_u64(file, at + 8);
    const u64 size = test::read_u64(file, at + 16);
    CHECK(offset % binary::SECTION_ALIGN == 0);
    CHECK(offset >= previous_end);
    CHECK(offset + size <= bytes.size() - 8);
    previous_end = offset + size;
  }

  // The footer is the writer's hash of everything before it.
  CHECK(test::read_u64(file, bytes.size() - 8) ==
        test::fnv1a64(file.first(bytes.size() - 8)));
}

TEST_CASE("The binary form holds the strings the tables reference") {
  ir::SymbolTable strings;
  const Storage storage = test::composite_storage(strings);
  const std::vector<u8> bytes = test::composite_bytes(strings, storage);
  const std::span<const u8> file(bytes);

  const usize strings_at = test::section_at(file, binary::Section::Strings);
  CHECK(test::read_u32(file, strings_at) >= 6);
  const usize strings_count = test::read_u32(file, strings_at);
  std::vector<std::string> found;
  usize at = strings_at + 4;
  for (usize i = 0; i < strings_count; ++i) {
    const usize length = test::read_u32(file, at);
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
  ir::SymbolTable strings;
  const Storage storage = test::composite_storage(strings);
  const std::vector<u8> first = test::composite_bytes(strings, storage);
  const std::vector<u8> second = test::composite_bytes(strings, storage);
  CHECK(first == second);
}

}  // namespace ir
