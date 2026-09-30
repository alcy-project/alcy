// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "text/unescape.h"

#include <string>
#include <string_view>

#include "doctest/doctest.h"

namespace text {

namespace {

std::string bytes(std::string_view quoted) {
  return unescape_string(quoted);
}

}  // namespace

TEST_CASE("Unescape decodes the simple escapes") {
  CHECK(bytes(R"("a\nb")") == "a\nb");
  CHECK(bytes(R"("\t\r\\\"")") == "\t\r\\\"");
  CHECK(bytes(R"("\0")") == std::string(1, '\0'));
  // An unknown escape keeps the escaped character, dropping the slash.
  CHECK(bytes(R"("\q")") == "q");
}

TEST_CASE("Unescape decodes a unicode escape as UTF-8") {
  CHECK(bytes(R"("\u{41}")") == "A");
  // Two bytes: U+00E9.
  CHECK(bytes(R"("\u{e9}")") == "\xC3\xA9");
  // Three bytes: U+263A.
  CHECK(bytes(R"("\u{263A}")") == "\xE2\x98\xBA");
  // Four bytes: U+1F600.
  CHECK(bytes(R"("\u{1F600}")") == "\xF0\x9F\x98\x80");
  CHECK(bytes(R"("a\u{41}b")") == "aAb");
}

TEST_CASE("Unescape drops a code point it cannot encode") {
  // Above U+10FFFF, and the surrogate range, are not characters.
  CHECK(bytes(R"("\u{110000}")").empty());
  CHECK(bytes(R"("\u{D800}")").empty());
  // A lone surrogate inside a longer string leaves the rest alone.
  CHECK(bytes(R"("x\u{D800}y")") == "xy");
}

TEST_CASE("Unescape tolerates a malformed unicode escape") {
  // No braces: the character stands for itself.
  CHECK(bytes(R"("\u41")") == "u41");
  // An empty run has no code point to decode.
  CHECK(bytes(R"("\u{}")") == "\\u{}");
  // A non-hex character ends the run, so the digits decode and the
  // offending character is dropped along with the run.
  CHECK(bytes(R"("\u{41z}")") == "A}");
}

}  // namespace text
