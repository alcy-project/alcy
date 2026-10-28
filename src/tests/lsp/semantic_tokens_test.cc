// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "lsp/semantic_tokens.h"

#include <span>
#include <string>
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

}  // namespace lsp
