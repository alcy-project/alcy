// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/semantic_tokens.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "lsp/analysis.h"
#include "lsp/document.h"
#include "text/json.h"

namespace lsp {

namespace {

// A token type index, or 0 when the decoder found nothing at the spot.
struct Decoded {
  u32 line = 0;
  u32 character = 0;
  u32 length = 0;
  u32 type = 0;
};

std::vector<Decoded> decode(const text::JsonValue& result) {
  std::vector<Decoded> tokens;
  const text::JsonValue* data = result.find("data");
  if (data == nullptr) {
    return tokens;
  }
  const std::span<const text::JsonValue> flat = data->as_array();
  u32 line = 0;
  u32 character = 0;
  for (usize i = 0; i + 4 < flat.size(); i += 5) {
    const u32 delta_line = static_cast<u32>(flat[i].as_number());
    const u32 delta_character = static_cast<u32>(flat[i + 1].as_number());
    if (delta_line == 0) {
      character += delta_character;
    } else {
      line += delta_line;
      character = delta_character;
    }
    tokens.push_back(Decoded{line, character,
                             static_cast<u32>(flat[i + 2].as_number()),
                             static_cast<u32>(flat[i + 3].as_number())});
  }
  return tokens;
}

u32 type_at(const std::vector<Decoded>& tokens, u32 line, u32 character) {
  for (const Decoded& token : tokens) {
    if (token.line == line && token.character == character) {
      return token.type;
    }
  }
  return 0;
}

std::vector<u32> flat_data(const text::JsonValue& result) {
  std::vector<u32> out;
  const text::JsonValue* data = result.find("data");
  if (data == nullptr) {
    return out;
  }
  for (const text::JsonValue& value : data->as_array()) {
    out.push_back(static_cast<u32>(value.as_number()));
  }
  return out;
}

std::vector<u32> apply_edits(const std::vector<u32>& before,
                             const text::JsonValue& edits) {
  std::vector<u32> out = before;
  for (const text::JsonValue& edit : edits.as_array()) {
    const usize start = static_cast<usize>(edit.find("start")->as_number());
    const usize remove =
        static_cast<usize>(edit.find("deleteCount")->as_number());
    out.erase(out.begin() + start, out.begin() + start + remove);
    const text::JsonValue* inserted = edit.find("data");
    if (inserted != nullptr) {
      std::vector<u32> values;
      for (const text::JsonValue& value : inserted->as_array()) {
        values.push_back(static_cast<u32>(value.as_number()));
      }
      out.insert(out.begin() + start, values.begin(), values.end());
    }
  }
  return out;
}

}  // namespace

TEST_CASE("Keywords, types, and literals are classified") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  ret 0\n}\n");
  const DocumentAnalysis analysis(document);
  const std::vector<Decoded> tokens =
      decode(semantic_tokens(document, analysis));
  CHECK(type_at(tokens, 0, 0) == 11);  // fn
  CHECK(type_at(tokens, 0, 3) == 9);   // main
  CHECK(type_at(tokens, 0, 13) == 1);  // i32
  CHECK(type_at(tokens, 1, 2) == 11);  // ret
  CHECK(type_at(tokens, 1, 6) == 15);  // 0
}

TEST_CASE("A binding and its use are variables") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  x := 1\n  ret x\n}\n");
  const DocumentAnalysis analysis(document);
  const std::vector<Decoded> tokens =
      decode(semantic_tokens(document, analysis));
  CHECK(type_at(tokens, 1, 2) == 6);  // the binding
  CHECK(type_at(tokens, 2, 6) == 6);  // the use
}

TEST_CASE("A struct name and its field carry their kinds") {
  const Document document("file:///main.al", "alcy", 1,
                          "struct Point {\n  x: i32,\n}\n");
  const DocumentAnalysis analysis(document);
  const std::vector<Decoded> tokens =
      decode(semantic_tokens(document, analysis));
  CHECK(type_at(tokens, 0, 7) == 2);  // Point
  CHECK(type_at(tokens, 1, 2) == 7);  // x
}

TEST_CASE("A comment is classified even when analysis fails") {
  const Document document("file:///broken.al", "alcy", 1,
                          "// note\nfn main( -> i32 {\n}\n");
  const DocumentAnalysis analysis(document);
  CHECK(!analysis.ok());
  const std::vector<Decoded> tokens =
      decode(semantic_tokens(document, analysis));
  CHECK(type_at(tokens, 0, 0) == 13);  // the comment
  CHECK(type_at(tokens, 1, 0) == 11);  // fn
}

TEST_CASE("A field read and a method call carry their kinds") {
  const Document document("file:///main.al", "alcy", 1,
                          "struct Point {\n  x: i32,\n}\n\nimpl Point {\n  fn "
                          "get(self: Self) -> i32 {\n"
                          "    ret self.x\n  }\n}\n\nfn take(p: Point) -> i32 "
                          "{\n  ret p.get()\n}\n");
  const DocumentAnalysis analysis(document);
  const std::vector<Decoded> tokens =
      decode(semantic_tokens(document, analysis));
  CHECK(type_at(tokens, 6, 13) == 7);   // self.x
  CHECK(type_at(tokens, 11, 8) == 10);  // p.get()
}

TEST_CASE("A range request answers only the tokens it covers") {
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  x := 1\n  ret x\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue result =
      semantic_tokens_range(document, analysis, Position{1, 0}, Position{2, 0});
  const std::vector<Decoded> tokens = decode(result);
  CHECK(!tokens.empty());
  bool one_line = true;
  for (const Decoded& token : tokens) {
    if (token.line != 1) {
      one_line = false;
    }
  }
  CHECK(one_line);
}

TEST_CASE("A multi-line comment becomes one token per line") {
  const Document document(
      "file:///main.al", "alcy", 1,
      "/* one\n   two */\nfn main() -> i32 {\n  ret 0\n}\n");
  const DocumentAnalysis analysis(document);
  const std::vector<Decoded> tokens =
      decode(semantic_tokens(document, analysis));
  CHECK(type_at(tokens, 0, 0) == 13);
  CHECK(type_at(tokens, 1, 0) == 13);
}

TEST_CASE("A delta after an edit reproduces the new array") {
  SemanticTokensCache cache;
  const Document before("file:///main.al", "alcy", 1,
                        "fn main() -> i32 {\n  x := 1\n  ret x\n}\n");
  const DocumentAnalysis before_analysis(before);
  const text::JsonValue full = cache.full(before, before_analysis);
  const std::string_view result_id = full.find("resultId")->as_string();
  const std::vector<u32> old_data = flat_data(full);

  const Document after("file:///main.al", "alcy", 2,
                       "fn main() -> i32 {\n  x := 1\n  y := 2\n  ret x\n}\n");
  const DocumentAnalysis after_analysis(after);
  const text::JsonValue delta = cache.delta(after, after_analysis, result_id);
  const text::JsonValue* edits = delta.find("edits");
  CHECK(edits != nullptr);
  if (edits == nullptr) {
    return;
  }
  CHECK(!edits->as_array().empty());
  const std::vector<u32> applied = apply_edits(old_data, *edits);
  const std::vector<u32> expected =
      flat_data(semantic_tokens(after, after_analysis));
  CHECK(applied == expected);
}

TEST_CASE("A stale previous id answers the whole array") {
  SemanticTokensCache cache;
  const Document document("file:///main.al", "alcy", 1,
                          "fn main() -> i32 {\n  ret 0\n}\n");
  const DocumentAnalysis analysis(document);
  const text::JsonValue delta = cache.delta(document, analysis, "stale");
  CHECK(delta.find("data") != nullptr);
  CHECK(delta.find("edits") == nullptr);
}

}  // namespace lsp
