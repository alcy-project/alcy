// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "text/json.h"

#include <string>
#include <string_view>
#include <utility>

#include "doctest/doctest.h"
#include "fpag/base/result.h"

namespace text {

namespace {

JsonValue parse_ok(std::string_view input) {
  base::Result<JsonValue, JsonError> result = parse_json(input);
  CHECK(result.is_ok());
  if (result.is_err()) {
    return JsonValue{};
  }
  return std::move(result).unwrap();
}

}  // namespace

TEST_CASE("A JSON object reads its members by key") {
  const JsonValue value = parse_ok(R"({"name":"alcy","count":3,"ok":true})");
  CHECK(value.is_object());
  const JsonValue* name = value.find("name");
  CHECK(name != nullptr);
  if (name != nullptr) {
    CHECK(name->as_string() == "alcy");
  }
  const JsonValue* count = value.find("count");
  CHECK(count != nullptr);
  if (count != nullptr) {
    CHECK(count->as_number() == 3.0);
  }
  const JsonValue* ok = value.find("ok");
  CHECK(ok != nullptr);
  if (ok != nullptr) {
    CHECK(ok->as_bool());
  }
  CHECK(value.find("missing") == nullptr);
}

TEST_CASE("A JSON value round-trips through the writer") {
  const JsonValue value =
      parse_ok(R"({"a":1,"b":[true,null,"x"],"c":{"d":-2.5}})");
  CHECK(to_json(value) == R"({"a":1,"b":[true,null,"x"],"c":{"d":-2.5}})");
}

TEST_CASE("A JSON string decodes its escapes") {
  const JsonValue value = parse_ok(R"("a\nb\t\"c\"\\d")");
  CHECK(value.as_string() == "a\nb\t\"c\"\\d");
}

TEST_CASE("A JSON unicode escape becomes UTF-8") {
  CHECK(parse_ok(R"("\u0041")").as_string() == "A");
  // A surrogate pair is one code point, not two replacement characters.
  CHECK(parse_ok(R"("\uD83D\uDE00")").as_string() ==
        std::string("\xF0\x9F\x98\x80"));
}

TEST_CASE("A lone surrogate escape becomes the replacement character") {
  CHECK(parse_ok(R"("\uD800")").as_string() == std::string("\xEF\xBF\xBD"));
}

TEST_CASE("A JSON number keeps its integer form") {
  const JsonValue value = parse_ok("[0,-17,1e3,2.5]");
  CHECK(value.is_array());
  if (!value.is_array()) {
    return;
  }
  CHECK(value.as_array()[0].as_number() == 0.0);
  CHECK(value.as_array()[1].as_number() == -17.0);
  CHECK(value.as_array()[2].as_number() == 1000.0);
  CHECK(value.as_array()[3].as_number() == 2.5);
}

TEST_CASE("A JSON parse reports where it stopped") {
  base::Result<JsonValue, JsonError> trailing = parse_json("1 2");
  CHECK(trailing.is_err());
  if (trailing.is_err()) {
    const JsonError error = std::move(trailing).unwrap_err();
    CHECK(error.offset == 2);
  }

  base::Result<JsonValue, JsonError> unterminated = parse_json(R"("abc)");
  CHECK(unterminated.is_err());
  if (unterminated.is_err()) {
    const JsonError error = std::move(unterminated).unwrap_err();
    CHECK(error.offset == 4);
  }

  CHECK(parse_json("{").is_err());
  CHECK(parse_json("[1,]").is_err());
  CHECK(parse_json(R"("\q")").is_err());
}

TEST_CASE("Nesting deeper than the shared budget is rejected, not recursed") {
  std::string deep;
  for (u32 i = 0; i < 300; ++i) {
    deep.push_back('[');
  }
  deep.push_back('1');
  for (u32 i = 0; i < 300; ++i) {
    deep.push_back(']');
  }
  CHECK(parse_json(deep).is_err());
}

}  // namespace text
