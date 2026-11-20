// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/document.h"

#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "lsp/protocol.h"
#include "text/json.h"

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

TEST_CASE("Positions can count UTF-8 bytes when the client asks") {
  const std::string text =
      "a\xC3\xA9"
      "z";
  PositionIndex index;
  index.set_encoding(PositionEncoding::Utf8);
  index.reset(text);

  CHECK(index.byte_offset(text, Position{0, 1}) == 1);
  CHECK(index.byte_offset(text, Position{0, 3}) == 3);
  CHECK(index.position_at(text, 3) == Position{0, 3});
  CHECK(index.position_at(text, 4) == Position{0, 4});
}

TEST_CASE("A position cache reuses an index for the same buffer") {
  const std::string text = "fn main() {\n  ret 0\n}\n";
  PositionCache cache;
  const PositionIndex& first = cache.index(text, PositionEncoding::Utf16);
  const PositionIndex& again = cache.index(text, PositionEncoding::Utf16);
  const PositionIndex& utf8 = cache.index(text, PositionEncoding::Utf8);
  CHECK(&first == &again);
  CHECK(&first != &utf8);
}

TEST_CASE("Cross-file ranges use the negotiated encoding") {
  const std::string text =
      "a\xC3\xA9"
      "z";
  const text::JsonValue utf8 = range_json(text, 3, 1, PositionEncoding::Utf8);
  const text::JsonValue utf16 = range_json(text, 3, 1, PositionEncoding::Utf16);
  CHECK(utf8.find("start")->find("character")->as_number() == 3);
  CHECK(utf16.find("start")->find("character")->as_number() == 2);
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
