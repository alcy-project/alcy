// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/layout.h"

#include <optional>
#include <string>

#include "doctest/doctest.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

TEST_CASE("A struct layout packs fields at their alignment") {
  const Document document("file:///mix.al", "alcy", 1,
                          "struct Mix {\n  a: u8,\n  b: i32,\n}\n");
  const std::optional<StructLayout> layout =
      struct_layout_at(document, Position{0, 8});
  CHECK(layout.has_value());
  if (!layout.has_value()) {
    return;
  }
  CHECK(layout->name == "Mix");
  CHECK(layout->size == 8);
  CHECK(layout->align == 4);
  CHECK(layout->fields.size() == 2);
  if (layout->fields.size() != 2) {
    return;
  }
  CHECK(layout->fields[0].name == "a");
  CHECK(layout->fields[0].type == "u8");
  CHECK(layout->fields[0].offset == 0);
  CHECK(layout->fields[0].size == 1);
  CHECK(layout->fields[1].name == "b");
  CHECK(layout->fields[1].type == "i32");
  CHECK(layout->fields[1].offset == 4);
  CHECK(layout->fields[1].size == 4);
}

TEST_CASE("A position off the declaration reports no layout") {
  const Document document("file:///mix.al", "alcy", 1,
                          "struct Mix {\n  a: u8,\n}\n");
  CHECK(!struct_layout_at(document, Position{1, 3}).has_value());
}

TEST_CASE("A generic declaration reports no concrete offsets") {
  const Document document("file:///box.al", "alcy", 1,
                          "struct Box<T> {\n  v: T,\n}\n");
  const std::optional<StructLayout> layout =
      struct_layout_at(document, Position{0, 8});
  CHECK(layout.has_value());
  if (!layout.has_value()) {
    return;
  }
  CHECK(layout->generic);
  CHECK(layout->fields.size() == 1);
}

TEST_CASE("The hover markdown names the size and the fields") {
  const Document document("file:///mix.al", "alcy", 1,
                          "struct Mix {\n  a: u8,\n  b: i32,\n}\n");
  const std::optional<StructLayout> layout =
      struct_layout_at(document, Position{0, 8});
  CHECK(layout.has_value());
  if (!layout.has_value()) {
    return;
  }
  const std::string markdown = struct_layout_markdown(*layout);
  CHECK(markdown.find("size **8**") != std::string::npos);
  CHECK(markdown.find("| b | i32 |") != std::string::npos);
}

TEST_CASE("The layout JSON carries every field") {
  const Document document("file:///mix.al", "alcy", 1,
                          "struct Mix {\n  a: u8,\n  b: i32,\n}\n");
  const std::optional<StructLayout> layout =
      struct_layout_at(document, Position{0, 8});
  CHECK(layout.has_value());
  if (!layout.has_value()) {
    return;
  }
  const text::JsonValue json = struct_layout_json(*layout);
  CHECK(json.is_object());
  const text::JsonValue* name = json.find("name");
  CHECK(name != nullptr);
  if (name != nullptr) {
    CHECK(name->as_string() == "Mix");
  }
  const text::JsonValue* size = json.find("size");
  CHECK(size != nullptr);
  if (size != nullptr) {
    CHECK(size->as_number() == 8.0);
  }
  const text::JsonValue* fields = json.find("fields");
  CHECK(fields != nullptr);
  if (fields != nullptr) {
    CHECK(fields->as_array().size() == 2);
  }
}

TEST_CASE("An enum layout gives the discriminant and the payload area") {
  const Document document(
      "file:///shape.al", "alcy", 1,
      "enum Shape {\n  Dot,\n  Line(i32),\n  Box(i32, i64),\n}\n");
  const std::optional<EnumLayout> layout =
      enum_layout_at(document, Position{0, 6});
  CHECK(layout.has_value());
  if (!layout.has_value()) {
    return;
  }
  CHECK(layout->name == "Shape");
  CHECK(layout->size == 24);
  CHECK(layout->align == 8);
  CHECK(layout->discriminant_size == 4);
  CHECK(layout->payload_offset == 8);
  CHECK(layout->payload_size == 16);
  CHECK(layout->payload_align == 8);
  CHECK(layout->variants.size() == 3);
  if (layout->variants.size() != 3) {
    return;
  }
  CHECK(layout->variants[0].name == "Dot");
  CHECK(layout->variants[0].fields.empty());
  CHECK(layout->variants[1].name == "Line");
  CHECK(layout->variants[1].fields.size() == 1);
  CHECK(layout->variants[2].name == "Box");
  CHECK(layout->variants[2].fields.size() == 2);
  if (layout->variants[2].fields.size() == 2) {
    CHECK(layout->variants[2].fields[0].type == "i32");
    CHECK(layout->variants[2].fields[1].type == "i64");
    CHECK(layout->variants[2].fields[1].offset == 8);
  }
}

}  // namespace lsp
