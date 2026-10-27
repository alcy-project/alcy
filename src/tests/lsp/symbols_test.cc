// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/symbols.h"

#include <string>
#include <string_view>

#include "doctest/doctest.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

const text::JsonValue* find_symbol(const text::JsonValue& symbols,
                                   std::string_view name) {
  for (const text::JsonValue& symbol : symbols.as_array()) {
    const text::JsonValue* candidate = symbol.find("name");
    if (candidate != nullptr && candidate->as_string() == name) {
      return &symbol;
    }
  }
  return nullptr;
}

f64 kind_of(const text::JsonValue& symbol) {
  const text::JsonValue* kind = symbol.find("kind");
  return kind == nullptr ? 0.0 : kind->as_number();
}

}  // namespace

TEST_CASE("An outline lists top-level items with their kinds") {
  const Document document("file:///main.al", "alcy", 1,
                          "struct Point {\n  x: i32,\n  y: i32,\n}\n\n"
                          "enum Color {\n  Red,\n  Green,\n}\n\n"
                          "fn main() -> i32 {\n  ret 0\n}\n");
  const text::JsonValue symbols = document_symbols(document);
  CHECK(symbols.is_array());
  CHECK(symbols.as_array().size() == 3);

  const text::JsonValue* point = find_symbol(symbols, "Point");
  CHECK(point != nullptr);
  if (point != nullptr) {
    CHECK(kind_of(*point) == 23.0);
    CHECK(point->find("range") != nullptr);
    CHECK(point->find("selectionRange") != nullptr);
    const text::JsonValue* children = point->find("children");
    CHECK(children != nullptr);
    if (children != nullptr) {
      CHECK(children->as_array().size() == 2);
      CHECK(find_symbol(*children, "x") != nullptr);
      CHECK(find_symbol(*children, "y") != nullptr);
    }
  }

  const text::JsonValue* color = find_symbol(symbols, "Color");
  CHECK(color != nullptr);
  if (color != nullptr) {
    CHECK(kind_of(*color) == 10.0);
    const text::JsonValue* children = color->find("children");
    CHECK(children != nullptr);
    if (children != nullptr) {
      CHECK(find_symbol(*children, "Red") != nullptr);
      CHECK(find_symbol(*children, "Green") != nullptr);
    }
  }

  const text::JsonValue* main_fn = find_symbol(symbols, "main");
  CHECK(main_fn != nullptr);
  if (main_fn != nullptr) {
    CHECK(kind_of(*main_fn) == 12.0);
  }
}

TEST_CASE("An outline lists impl methods as children") {
  const Document document("file:///main.al", "alcy", 1,
                          "struct Point {\n  x: i32,\n}\n\n"
                          "impl Point {\n  fn id(x: i32) -> i32 {\n"
                          "    ret x\n  }\n}\n");
  const text::JsonValue symbols = document_symbols(document);
  CHECK(symbols.is_array());
  CHECK(symbols.as_array().size() == 2);

  const text::JsonValue* impl = nullptr;
  for (const text::JsonValue& symbol : symbols.as_array()) {
    if (kind_of(symbol) == 19.0) {
      impl = &symbol;
    }
  }
  CHECK(impl != nullptr);
  if (impl == nullptr) {
    return;
  }
  const text::JsonValue* children = impl->find("children");
  CHECK(children != nullptr);
  if (children != nullptr) {
    CHECK(find_symbol(*children, "id") != nullptr);
  }
}

TEST_CASE("An outline survives a buffer the parser cannot finish") {
  const Document document("file:///broken.al", "alcy", 1,
                          "struct Broken {\n  x: i32,\n");
  const text::JsonValue symbols = document_symbols(document);
  CHECK(symbols.is_array());
}

}  // namespace lsp
