// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "symbol/symbol_table.h"

#include <string>
#include <string_view>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/str/string_pool_id.h"

namespace {

using symbol::SymbolTable;

// An id a test can compare against when interning was expected to answer and
// did not; no name is ever given this handle.
constexpr str::StringPoolId NO_ID = str::INVALID_STRING_POOL_ID;

TEST_CASE("A name interns to one handle whichever way it is interned") {
  SymbolTable table;
  const std::string_view name = "advance";
  const auto view_id = table.try_intern(name);
  const auto copy_id = table.intern_copied(name);
  CHECK(view_id.has_value());
  CHECK(copy_id.has_value());
  CHECK(view_id.value_or(NO_ID) == copy_id.value_or(NO_ID));
  CHECK(table.get(view_id.value_or(NO_ID)) == name);
}

TEST_CASE("The empty name has the handle the IR defaults to") {
  SymbolTable table;
  const std::string_view empty;
  const auto id = table.try_intern(empty);
  CHECK(id.value_or(NO_ID) == str::EMPTY_STRING_ID);
  CHECK(table.get(str::EMPTY_STRING_ID).empty());
}

TEST_CASE("A name interned as a view reads the bytes it was given") {
  SymbolTable table;
  // A stable buffer, which is what `try_intern` requires of its caller.
  static constexpr std::string_view NAME = "length_squared";
  const auto id = table.try_intern(NAME);
  CHECK(id.has_value());
  // The same bytes and not a copy: the stored view points into the buffer.
  CHECK(table.get(id.value_or(NO_ID)).data() == NAME.data());
}

TEST_CASE("A copied name survives the bytes it was made from") {
  SymbolTable table;
  std::string decoded = "line";
  const auto id = table.intern_copied(decoded);
  CHECK(id.has_value());
  decoded = "line\ntwo";
  CHECK(table.get(id.value_or(NO_ID)) == std::string_view("line"));
}

TEST_CASE("Names that differ are different handles") {
  SymbolTable table;
  std::vector<str::StringPoolId> ids;
  const std::vector<std::string_view> names = {"a",  "b",  "aa", "ab",
                                               "ba", "bb", "abc"};
  for (std::string_view name : names) {
    const auto id = table.try_intern(name);
    CHECK(id.has_value());
    ids.push_back(id.value_or(NO_ID));
  }
  for (usize i = 0; i < ids.size(); ++i) {
    CHECK(table.get(ids[i]) == names[i]);
    for (usize j = i + 1; j < ids.size(); ++j) {
      CHECK(ids[i] != ids[j]);
    }
  }
  CHECK(table.count() == names.size() + 1);
}

TEST_CASE("An id this table did not mint reads as no name") {
  SymbolTable table;
  CHECK(table.get(str::INVALID_STRING_POOL_ID).empty());
  CHECK(table.get(str::StringPoolId{1000}).empty());
}

}  // namespace
