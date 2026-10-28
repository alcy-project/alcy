// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/rename.h"

#include <optional>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

struct Edit {
  u32 line = 0;
  u32 character = 0;
  std::string text;
};

std::vector<Edit> edits_of(const text::JsonValue& result) {
  std::vector<Edit> out;
  const text::JsonValue* changes = result.find("changes");
  if (changes == nullptr) {
    return out;
  }
  const text::JsonValue* edits = changes->find("file:///main.al");
  if (edits == nullptr) {
    return out;
  }
  for (const text::JsonValue& edit : edits->as_array()) {
    const text::JsonValue* range = edit.find("range");
    const text::JsonValue* start = range->find("start");
    const text::JsonValue* new_text = edit.find("newText");
    out.push_back(Edit{static_cast<u32>(start->find("line")->as_number()),
                       static_cast<u32>(start->find("character")->as_number()),
                       std::string(new_text->as_string())});
  }
  return out;
}

bool has_edit(const std::vector<Edit>& edits, u32 line, u32 character) {
  for (const Edit& edit : edits) {
    if (edit.line == line && edit.character == character &&
        edit.text == "ident") {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("An identifier is a valid rename target and its text is not") {
  CHECK(is_rename_identifier("ident"));
  CHECK(is_rename_identifier("_x1"));
  CHECK(!is_rename_identifier(""));
  CHECK(!is_rename_identifier("fn"));
  CHECK(!is_rename_identifier("1x"));
  CHECK(!is_rename_identifier("a b"));
  CHECK(!is_rename_identifier("_"));
}

TEST_CASE("A function renames its declaration and its call") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn id(x: i32) -> i32 {\n  ret x\n}\nfn main() -> "
                          "i32 {\n  ret id(1)\n}\n");
  const DocumentAnalysis analysis(document);

  const std::optional<RenameTarget> target =
      rename_target(document, analysis, Position{4, 7});
  CHECK(target.has_value());
  if (target.has_value()) {
    CHECK(target->name == "id");
    CHECK(target->span.offset ==
          document.index().byte_offset(document.text(), Position{4, 6}));
  }

  const std::optional<text::JsonValue> edit =
      workspace_rename(document, analysis, Position{4, 7}, "ident");
  CHECK(edit.has_value());
  if (!edit.has_value()) {
    return;
  }
  const std::vector<Edit> edits = edits_of(*edit);
  CHECK(edits.size() == 2);
  CHECK(has_edit(edits, 0, 3));
  CHECK(has_edit(edits, 4, 6));
}

TEST_CASE("A local renames its binding and its use") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  x := 1\n  ret x\n}\n");
  const DocumentAnalysis analysis(document);
  const std::optional<text::JsonValue> edit =
      workspace_rename(document, analysis, Position{2, 6}, "ident");
  CHECK(edit.has_value());
  if (!edit.has_value()) {
    return;
  }
  const std::vector<Edit> edits = edits_of(*edit);
  CHECK(edits.size() == 2);
  CHECK(has_edit(edits, 1, 2));
  CHECK(has_edit(edits, 2, 6));
}

TEST_CASE("A field renames its declaration and its read") {
  const Document document(
      "file:///main.al", "alcy", 1,
      "struct Point {\n  x: i32,\n}\nfn take(p: Point) -> i32 {\n"
      "  ret p.x\n}\n");
  const DocumentAnalysis analysis(document);
  const std::optional<text::JsonValue> edit =
      workspace_rename(document, analysis, Position{4, 8}, "y");
  CHECK(edit.has_value());
  if (!edit.has_value()) {
    return;
  }
  const std::vector<Edit> edits = edits_of(*edit);
  CHECK(edits.size() == 2);
  if (edits.size() == 2) {
    CHECK(edits[0].text == "y");
    CHECK(edits[1].text == "y");
    CHECK(edits[0].line == 1);
    CHECK(edits[0].character == 2);
    CHECK(edits[1].line == 4);
    CHECK(edits[1].character == 8);
  }
}

TEST_CASE("A comment has no rename target") {
  const Document document("file:///main.al", "alcy", 1,
                          "// nothing\nfn main() -> i32 {\n  ret 0\n}\n");
  const DocumentAnalysis analysis(document);
  CHECK(!rename_target(document, analysis, Position{0, 3}).has_value());
  CHECK(!workspace_rename(document, analysis, Position{0, 3}, "x").has_value());
}

}  // namespace lsp
