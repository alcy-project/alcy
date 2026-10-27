// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/document.h"

#include <string>
#include <vector>

#include "doctest/doctest.h"

namespace lsp {

TEST_CASE("Positions count UTF-16 code units, not bytes") {
  // 'a', U+1F600 (four UTF-8 bytes, two UTF-16 units), 'b'.
  const std::string text =
      "a\xF0\x9F\x98\x80"
      "b";
  PositionIndex index;
  index.reset(text);

  CHECK(index.byte_offset(text, Position{0, 0}) == 0);
  CHECK(index.byte_offset(text, Position{0, 1}) == 1);
  CHECK(index.byte_offset(text, Position{0, 2}) == 5);
  CHECK(index.byte_offset(text, Position{0, 3}) == 5);
  CHECK(index.position_at(text, 0) == Position{0, 0});
  CHECK(index.position_at(text, 1) == Position{0, 1});
  CHECK(index.position_at(text, 5) == Position{0, 3});
  // Inside the code point rounds down to its start.
  CHECK(index.position_at(text, 3) == Position{0, 1});
}

TEST_CASE("A carriage-return pair is one line break") {
  const std::string text = "ab\r\ncd";
  PositionIndex index;
  index.reset(text);

  CHECK(index.position_at(text, 2) == Position{0, 2});
  CHECK(index.position_at(text, 4) == Position{1, 0});
  CHECK(index.position_at(text, 5) == Position{1, 1});
  CHECK(index.byte_offset(text, Position{1, 0}) == 4);
  CHECK(index.byte_offset(text, Position{1, 1}) == 5);
  // A line past the end clamps instead of reading out of bounds.
  CHECK(index.byte_offset(text, Position{9, 0}) == text.size());
}

TEST_CASE("A document applies an incremental change") {
  Document document("file:///main.al", "alcy", 1, "fn main() {}");
  std::vector<ContentChange> changes;
  changes.push_back(ContentChange{true, Range{{0, 3}, {0, 7}}, "run"});
  document.apply(changes, 2);

  CHECK(document.text() == "fn run() {}");
  CHECK(document.version() == 2);
}

TEST_CASE("A document applies a full replacement") {
  Document document("file:///main.al", "alcy", 1, "fn main() {}");
  std::vector<ContentChange> changes;
  changes.push_back(ContentChange{false, Range{}, "x"});
  document.apply(changes, 2);

  CHECK(document.text() == "x");
}

TEST_CASE("A document store opens, finds, and closes by URI") {
  DocumentStore store;
  Document& document = store.open("file:///main.al", "alcy", 1, "a");
  CHECK(document.text() == "a");
  CHECK(store.find("file:///main.al") != nullptr);
  CHECK(store.find("file:///other.al") == nullptr);
  CHECK(store.close("file:///main.al"));
  CHECK(store.find("file:///main.al") == nullptr);
  CHECK(!store.close("file:///main.al"));
}

}  // namespace lsp
