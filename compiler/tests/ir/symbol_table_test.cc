// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ir/symbol_table.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/str/string_pool_id.h"

namespace {

using ir::SymbolTable;

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
  const std::vector<std::string_view> names = {"a",    "b",     "ab",    "abc",
                                               "abcd", "abcde", "abcdef"};
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

TEST_CASE("A grown table keeps every handle and the views behind them") {
  SymbolTable table;
  // A view interned before the growth; growing moves slots, not names,
  // so its bytes must still be the buffer's own.
  static constexpr std::string_view VIEW_NAME = "before_the_growth";
  const auto view_id = table.try_intern(VIEW_NAME);
  CHECK(view_id.has_value());

  // Four thousand names cannot fit sixty-four shards of sixteen slots
  // without some shard passing its half-full mark, whatever the hash
  // spreads them over, so the rehash path runs here.
  constexpr u32 NAME_COUNT = 4096;
  std::vector<std::string> names;
  names.reserve(NAME_COUNT);
  for (u32 i = 0; i < NAME_COUNT; ++i) {
    names.push_back("grown_" + std::to_string(i));
  }
  std::vector<str::StringPoolId> ids;
  ids.reserve(NAME_COUNT);
  for (const std::string& name : names) {
    const auto id = table.intern_copied(name);
    CHECK(id.has_value());
    ids.push_back(id.value_or(NO_ID));
  }

  CHECK(table.count() == NAME_COUNT + 2);
  for (u32 i = 0; i < NAME_COUNT; ++i) {
    CHECK(table.get(ids[i]) == names[i]);
    // A lookup that probed through the grown slots answers with the
    // handle the insert returned.
    CHECK(table.try_intern(names[i]).value_or(NO_ID) == ids[i]);
  }
  CHECK(table.get(view_id.value_or(NO_ID)) == VIEW_NAME);
  CHECK(table.get(view_id.value_or(NO_ID)).data() == VIEW_NAME.data());
}

TEST_CASE("A table at its limit refuses the next name") {
  // The empty name holds one of the four slots, so three are left.
  SymbolTable table(4);
  const auto a = table.try_intern("a");
  const auto b = table.intern_copied("b");
  const auto c = table.try_intern("c");
  CHECK(a.has_value());
  CHECK(b.has_value());
  CHECK(c.has_value());
  // Neither path takes a name past the limit, and a refused name leaves
  // the table as it was.
  CHECK(!table.try_intern("d").has_value());
  CHECK(!table.intern_copied("d").has_value());
  CHECK(table.count() == 4);
  CHECK(table.get(a.value_or(NO_ID)) == "a");
  CHECK(table.get(b.value_or(NO_ID)) == "b");
  CHECK(table.get(c.value_or(NO_ID)) == "c");
  CHECK(table.get(str::EMPTY_STRING_ID).empty());
}

TEST_CASE("A table sized for the empty name alone holds nothing else") {
  SymbolTable table(1);
  CHECK(table.count() == 1);
  CHECK(!table.try_intern("a").has_value());
  CHECK(table.get(str::EMPTY_STRING_ID).empty());
}

// The handle each recorded key got, by key, so a case can compare two gathers
// whose shards were filled in different orders.
std::vector<str::StringPoolId> handles_by_key(
    const std::vector<std::vector<SymbolTable::Pending>>& shards,
    usize keys) {
  std::vector<str::StringPoolId> ids(keys, str::INVALID_STRING_POOL_ID);
  for (const std::vector<SymbolTable::Pending>& shard : shards) {
    for (const SymbolTable::Pending& pending : shard) {
      ids[pending.key] = pending.handle;
    }
  }
  return ids;
}

TEST_CASE("A gather interns every name a producer recorded") {
  static constexpr std::array<std::string_view, 4> NAMES = {
      "advance", "length_squared", "make", "area"};
  SymbolTable table;
  std::vector<SymbolTable::Bins> bins(2);
  for (SymbolTable::Bins& producer : bins) {
    for (usize i = 0; i < NAMES.size(); ++i) {
      SymbolTable::record(producer, NAMES[i], /*stable=*/true,
                          static_cast<u32>(i));
    }
  }
  table.gather(bins);
  const std::vector<str::StringPoolId> first =
      handles_by_key(bins[0].shards, NAMES.size());
  const std::vector<str::StringPoolId> second =
      handles_by_key(bins[1].shards, NAMES.size());
  CHECK(first == second);
  CHECK(table.count() == NAMES.size() + 1);
  for (usize i = 0; i < NAMES.size(); ++i) {
    CHECK(table.get(first[i]) == NAMES[i]);
  }
}

TEST_CASE("The order the producers were filled does not reach the handles") {
  static constexpr std::array<std::string_view, 3> NAMES = {"alpha", "beta",
                                                            "gamma"};
  SymbolTable table;
  std::vector<SymbolTable::Bins> bins(2);
  for (SymbolTable::Bins& producer : bins) {
    for (usize i = 0; i < NAMES.size(); ++i) {
      SymbolTable::record(producer, NAMES[i], /*stable=*/true,
                          static_cast<u32>(i));
    }
  }
  // The same two producers, filled in the opposite order.
  std::vector<SymbolTable::Bins> reversed(2);
  for (usize producer = reversed.size(); producer-- > 0;) {
    for (usize i = 0; i < NAMES.size(); ++i) {
      SymbolTable::record(reversed[producer], NAMES[i], /*stable=*/true,
                          static_cast<u32>(i));
    }
  }
  table.gather(bins);
  table.gather(reversed);
  for (usize producer = 0; producer < bins.size(); ++producer) {
    CHECK(handles_by_key(bins[producer].shards, NAMES.size()) ==
          handles_by_key(reversed[producer].shards, NAMES.size()));
  }
}

TEST_CASE("A name recorded as unstable is copied by the gather") {
  SymbolTable table;
  std::vector<SymbolTable::Bins> bins(1);
  std::string synthesized = "closure$1";
  SymbolTable::record(bins[0], synthesized, /*stable=*/false, 0);
  synthesized = "closure$1\0two";
  table.gather(bins);
  const std::vector<str::StringPoolId> handles =
      handles_by_key(bins[0].shards, 1);
  CHECK(table.get(handles[0]) == std::string_view("closure$1"));
}

}  // namespace
