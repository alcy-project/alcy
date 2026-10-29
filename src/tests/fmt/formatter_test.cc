// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "fmt/formatter.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "fpag/base/numeric.h"

namespace alcy_fmt {
namespace {

// One pass with the default configuration, which is what the tool passes.
std::string once(std::string_view source) {
  return alcy_fmt::format(source, default_format_config());
}

// One pass with an injected width, which is what a host does.
std::string once_at(std::string_view source, usize line_length) {
  FormatConfig config;
  config.line_length = line_length;
  return alcy_fmt::format(source, config);
}

// The property the formatter is built around: a second pass changes
// nothing.
bool idempotent(std::string_view source, const FormatConfig& config) {
  const std::string first = alcy_fmt::format(source, config);
  const std::string second = alcy_fmt::format(first, config);
  if (first != second) {
    return false;
  }
  return true;
}

bool idempotent_default(std::string_view source) {
  return idempotent(source, default_format_config());
}

// A batch of inputs, each of which must format without crashing, must be
// idempotent, and must never end in anything but a single newline.
void check_corpus(std::string_view label, std::vector<std::string> sources) {
  for (std::string& source : sources) {
    const std::string formatted = once(source);
    INFO(label, " input: ", source);
    CHECK(idempotent_default(source));
    if (!formatted.empty()) {
      CHECK(formatted.back() == '\n');
    }
    if (formatted.size() >= 2) {
      CHECK(formatted[formatted.size() - 2] != '\n');
    }
  }
}

}  // namespace

TEST_CASE("The default configuration is a plain value") {
  const FormatConfig config = default_format_config();
  CHECK(config.line_length == 100);
  CHECK(config.indent_width == 2);
}

TEST_CASE("Empty and blank input formats to empty output") {
  CHECK(once("").empty());
  CHECK(once("   \t\r\n ").empty());
  CHECK(once("// just a comment\n") == "// just a comment\n");
}

TEST_CASE("A function lays out on one header line and an indented body") {
  const std::string out = once("fn   main ( ) { ret 0 }");
  CHECK(out == "fn main() {\n  ret 0\n}\n");
}

TEST_CASE("Statement separators become one statement per line") {
  const std::string out = once("fn f() { a := 1; b := 2 }");
  CHECK(out == "fn f() {\n  a := 1\n  b := 2\n}\n");
}

TEST_CASE("Line breaks inside brackets do not end statements") {
  const std::string out = once("fn f() { g(1,\n\n2) }");
  CHECK(out == "fn f() {\n  g(1, 2)\n}\n");
}

TEST_CASE("Comments survive in their own positions") {
  const std::string out = once(
      "fn f() {\n"
      "  // leading\n"
      "  a := 1 // trailing\n"
      "  /* block */\n"
      "  b := 2\n"
      "}\n");
  CHECK(out ==
        "fn f() {\n"
        "  // leading\n"
        "  a := 1 // trailing\n"
        "  /* block */\n"
        "  b := 2\n"
        "}\n");
}

TEST_CASE("Generic brackets stay tight and keep their space before a brace") {
  CHECK(once("fn alloc<T>(count: usize) -> Vec<T> { ret 0 }") ==
        "fn alloc<T>(count: usize) -> Vec<T> {\n  ret 0\n}\n");
  CHECK(once("fn f() { ret Vec::<String>::new() }") ==
        "fn f() {\n  ret Vec::<String>::new()\n}\n");
  CHECK(once("fn f() { ret Point::<i32> { x: 1 } }") ==
        "fn f() {\n  ret Point::<i32> { x: 1 }\n}\n");
  CHECK(once("fn f() { ret Point::<i32>{x: 1} }") ==
        "fn f() {\n  ret Point::<i32> { x: 1 }\n}\n");
}

TEST_CASE("Spec and extern members keep their terminating semicolons") {
  const std::string spec =
      once("spec Show {\n  fn show(self: &Self) -> str;\n}\n");
  CHECK(spec == "spec Show {\n  fn show(self: &Self) -> str;\n}\n");
  CHECK(idempotent_default("spec Show {\n  fn show(self: &Self) -> str;\n}\n"));

  const std::string external =
      once("extern \"C\" {\n  fn abs(x: i32) -> i32;\n}\n");
  CHECK(external == "extern \"C\" {\n  fn abs(x: i32) -> i32;\n}\n");
}

TEST_CASE("A signature closing in a greater-than keeps its semicolon") {
  const std::string intrinsic = once(
      "pub unsafe intrinsic fn alloc<T>(count: usize) -> "
      "&mut MaybeUninit<T>;\n");
  CHECK(intrinsic ==
        "pub unsafe intrinsic fn alloc<T>(count: usize) -> "
        "&mut MaybeUninit<T>;\n");
  CHECK(
      idempotent_default("pub unsafe intrinsic fn alloc<T>(count: usize) -> "
                         "&mut MaybeUninit<T>;\n"));
}

TEST_CASE("The last statement of a body keeps its spelled-out semicolon") {
  // Without the semicolon the call becomes the block's trailing
  // expression, so the run it belongs to changes meaning.
  CHECK(once("fn f() { add(1, 2); }") == "fn f() {\n  add(1, 2);\n}\n");
  CHECK(once("use a::b;\n") == "use a::b;\n");
  // A semicolon mid-body reads the same as the break that replaces it;
  // the last one stays, because the run it ends is the block's last.
  CHECK(once("fn f() { g(1); g(2); }") == "fn f() {\n  g(1)\n  g(2);\n}\n");
  // A use carries its terminator whether the source spelled it or
  // left it to the newline: the grammar writes the semicolon, and a
  // reader without the insertion rule has to see it.
  CHECK(once("use a::b\n") == "use a::b;\n");
  CHECK(once("use a::b;\n") == "use a::b;\n");
  CHECK(once("pub use a::b;\n") == "pub use a::b;\n");
}

TEST_CASE("Doc comments stay attached to the item that follows") {
  const std::string out = once("/// One.\n/// Two.\nfn f() {}");
  CHECK(out == "/// One.\n/// Two.\nfn f() {}\n");
}

TEST_CASE("An unsafe block reads like the value it wraps") {
  // The gate block is an expression, so one statement that fits stays
  // on the line; a body of statements takes the lines.
  CHECK(once("fn f() { ret unsafe { alloc::<u8>(0) } }") ==
        "fn f() {\n  ret unsafe { alloc::<u8>(0) }\n}\n");
  CHECK(once("fn f() {\n"
             "  ret unsafe {\n"
             "    g(1)\n"
             "    h(2)\n"
             "  }\n"
             "}\n") ==
        "fn f() {\n"
        "  ret unsafe {\n"
        "    g(1)\n"
        "    h(2)\n"
        "  }\n"
        "}\n");
  CHECK(idempotent_default("fn f() { ret unsafe { alloc::<u8>(0) } }"));
  // A spelled-out semicolon inside keeps the run a statement rather
  // than the block's value, flat or broken.
  CHECK(once("fn f() { _ := unsafe { g(1); } }") ==
        "fn f() {\n  _ := unsafe { g(1); }\n}\n");
  // A comment that ends its line keeps the block off the one-line form.
  const std::string commented = once(
      "fn f() {\n"
      "  ret unsafe { g(1) // why\n"
      "  }\n"
      "}\n");
  CHECK(commented ==
        "fn f() {\n"
        "  ret unsafe {\n"
        "    g(1) // why\n"
        "  }\n"
        "}\n");
}

TEST_CASE("Control-flow bodies take a line each however short") {
  CHECK(once("fn f(x: bool) { if x { ret 1 } else { ret 2 } }") ==
        "fn f(x: bool) {\n"
        "  if x {\n"
        "    ret 1\n"
        "  } else {\n"
        "    ret 2\n"
        "  }\n"
        "}\n");
  CHECK(once("struct Point { x: i32, y: i32 }") ==
        "struct Point {\n"
        "  x: i32,\n"
        "  y: i32,\n"
        "}\n");
  CHECK(once("fn f(x: i32) { match x { 1 => 2, _ => 3 } }") ==
        "fn f(x: i32) {\n"
        "  match x {\n"
        "    1 => 2,\n"
        "    _ => 3,\n"
        "  }\n"
        "}\n");
}

TEST_CASE("Blank lines between statements are kept to one") {
  const std::string out = once("fn f() { a := 1\n\n\n\n b := 2 }");
  CHECK(out == "fn f() {\n  a := 1\n\n  b := 2\n}\n");
}

TEST_CASE("A long argument list wraps and gains a trailing comma") {
  const std::string out =
      once_at("fn f() { g(some_argument, another_argument, third) }", 24);
  CHECK(out ==
        "fn f() {\n  g(\n    some_argument,\n    another_argument,\n"
        "    third,\n  )\n}\n");
}

TEST_CASE("An injected width decides whether a call wraps") {
  const std::string source = "fn f() { g(first, second) }";
  CHECK(once_at(source, 100) == "fn f() {\n  g(first, second)\n}\n");
  CHECK(once_at(source, 16) ==
        "fn f() {\n  g(\n    first,\n    second,\n"
        "  )\n}\n");
}

TEST_CASE("Struct literals stay inline when they fit") {
  const std::string out = once("fn f() { ret Point { x: 1, y: 2 } }");
  CHECK(out == "fn f() {\n  ret Point { x: 1, y: 2 }\n}\n");
}

TEST_CASE("Struct declarations put one field per line with trailing commas") {
  const std::string out = once("struct Point { x: i32, y: i32 }");
  CHECK(out == "struct Point {\n  x: i32,\n  y: i32,\n}\n");
}

TEST_CASE("Match arms each get their own line") {
  const std::string out = once(
      "fn f(x: i32) -> i32 {\n"
      "  match x { 1 => 2, _ => 3 }\n"
      "}\n");
  CHECK(out ==
        "fn f(x: i32) -> i32 {\n"
        "  match x {\n"
        "    1 => 2,\n"
        "    _ => 3,\n"
        "  }\n"
        "}\n");
}

TEST_CASE("An if chain keeps its blocks on the header lines") {
  const std::string out = once("fn f(x: i32) { if x { g() } else { h() } }");
  CHECK(out ==
        "fn f(x: i32) {\n"
        "  if x {\n"
        "    g()\n"
        "  } else {\n"
        "    h()\n"
        "  }\n"
        "}\n");
}

TEST_CASE("A wide variety of inputs is idempotent and error tolerant") {
  check_corpus("valid", {
                            "use a::b;\n",
                            "static X: [u8; 4] = [0, 0, 0, 0]\n",
                            "enum E { A, B(i32) }\n",
                            "impl Foo {\n  fn a(&self) {}\n  fn b() {}\n}\n",
                            "spec S {\n  fn m(&self) -> i32;\n}\n",
                            "const N: usize = 4\n",
                            "fn f(x: &mut Vec<u8>) -> Option<(i32, i32)> { "
                            "Option::Some((1, 2)) }\n",
                            "fn f() { mut v := Vec::<String>::new(); v }\n",
                        });
  check_corpus("broken", {
                             "",
                             "fn",
                             "fn f(",
                             "fn f() {",
                             "}",
                             ")",
                             "(",
                             "((((((",
                             "}}}}",
                             "\"\"",
                             "'\"\"",
                             "@",
                             "fn f() { x := }",
                             "match {",
                             "struct {",
                             "impl {",
                             "if {",
                             "else {",
                             "fn f() { a := 1 } trailing garbage @@ $$",
                             "fn f() { /* unterminated\n",
                             "fn f() { // no newline at end",
                             "\x00\x01\x02",
                             "fn f() { ret }",
                             "fn f() { 1 + 2 * 3 - 4 }",
                             "a b c d",
                             ",,,,",
                             ";;;;",
                             "....",
                             "<<<<",
                             ">>>>",
                         });
  check_corpus("nesting", {
                              "fn f() { (((((((((1))))))))) }\n",
                              "fn f() { [[[[[[[[1]]]]]]]] }\n",
                              "fn f() { if a { if b { if c { g() } } } }\n",
                          });
}

TEST_CASE("A statement an unterminated comment follows keeps its layout") {
  // The comment the source never closed is a token the lexer reports,
  // so the newline in front of it still terminates the statement: the
  // expression's line reads the same on the pass over its own output.
  // The stray scalar and the reserved hash ride along because the
  // smallest reproducer carried them.
  const std::string source = std::string("+#\xdbvv;/*#");
  FormatConfig narrow;
  narrow.line_length = 24;
  narrow.indent_width = 1;
  CHECK(idempotent(source, default_format_config()));
  CHECK(idempotent(source, narrow));
}

TEST_CASE("A hungry scalar that swallowed a tab keeps the line count") {
  // A UTF-8 lead takes the continuation bytes that follow it and stops
  // at the first byte that is neither one nor part of the promise, so
  // the span never ends in a byte the layout rewrites: the pass over
  // the layout reads the same span the pass over the source did.
  const std::string source = std::string(
      "\xf6\xf6\xff\xff\xff\xff\xff\xff\xffRRR\xff\xff\xf6\xf6\xf6\t\n"
      "\xf6\xf6\xf6\xf6\xf6\xf6\xf6\xf6\xf6\xf6\xf6\xf6\xf6\xf6");
  FormatConfig narrow;
  narrow.line_length = 24;
  narrow.indent_width = 1;
  CHECK(idempotent(source, default_format_config()));
  CHECK(idempotent(source, narrow));
}

TEST_CASE("A gap a dropped semicolon leaves does not break the line") {
  // An error that eats the separator after it takes the chunk boundary
  // with it, so the pass over the layout reads one chunk where the
  // source had two, and the pinned gap between the error and the code
  // carries newlines into the middle of a flow. A group measured across
  // those newlines broke where the source's own pass fit; the measure
  // stops at the break, and the line the source fit stays put.
  const std::string source = std::string("\x00'\n;0&1+\n\n");
  FormatConfig narrow;
  narrow.line_length = 24;
  narrow.indent_width = 1;
  CHECK(idempotent(source, default_format_config()));
  CHECK(idempotent(source, narrow));
}

TEST_CASE("Code an error precedes across a newline keeps its indent") {
  // The error's span eats the layout's break and indent, so the
  // semicolon the next pass inserts there is not the one this pass saw,
  // and a chunk boundary that rode on it would move with it: the code
  // after the error starts a chunk of its own, and the braces an
  // unclosed delimiter left open hold their line wherever the pass
  // started from.
  const std::string source = std::string(
      "{{\x8aZZ\xf5\n\n\n\n\n\n\n\n\x1a\x00\x00\x00\x00\x00\x00\x00\n\n\n\n{{");
  FormatConfig narrow;
  narrow.line_length = 24;
  narrow.indent_width = 1;
  CHECK(idempotent(source, default_format_config()));
  CHECK(idempotent(source, narrow));
}

TEST_CASE("A carriage return an error span ends in does not survive") {
  // A run of stray scalars sits against carriage returns and newlines
  // the lexer reads as trivia: the layout writes its own breaks there,
  // and the pass over them reads the same runs the pass over the
  // source did.
  const std::string source = std::string(
      "{sp\xf5\xf5\xf5\xf5\xf5\xf5\xf5\xf5\xf5\r\n\n\n\n\n\n\n"
      "\xf5\xf5\xf5\xf5\xf5\xf5\xf5?\xf5\xf5\xf5\xf5\xf5\xf5\xf5\xf5\xf5\xf5"
      "\xf5\xf5\xf5\xf5\xf5\xf5");
  FormatConfig narrow;
  narrow.line_length = 24;
  narrow.indent_width = 1;
  CHECK(idempotent(source, default_format_config()));
  CHECK(idempotent(source, narrow));
}

TEST_CASE("A bracket a line comment hides does not swallow the code") {
  // The comment rides the line it sits on, the semicolon the lexer
  // inserted at the newline stands for the break the comment's own
  // already wrote, and the bytes past it are the next line's indent --
  // the layout writes that indent once, and the error's span reads the
  // same wherever the pass started from.
  const std::string source = std::string("+{*[w//[\n\xeb\xb1");
  FormatConfig narrow;
  narrow.line_length = 24;
  narrow.indent_width = 1;
  CHECK(idempotent(source, default_format_config()));
  CHECK(idempotent(source, narrow));
}

TEST_CASE("Deeply nested input does not exhaust the stack") {
  std::string source;
  for (usize i = 0; i < 600; ++i) {
    source += "fn f() { ret ((((";
  }
  source += '1';
  for (usize i = 0; i < 600; ++i) {
    source += ")))) }";
  }
  const std::string out = once(source);
  CHECK(!out.empty());
  CHECK(idempotent_default(source));
}

TEST_CASE("Every configuration in a range keeps the output idempotent") {
  const std::string source =
      "struct Point { x: i32, y: i32 }\n"
      "fn f() { ret Point { x: 100000000000, y: -200000000000 } }\n";
  for (usize width = 8; width <= 120; width += 8) {
    FormatConfig config;
    config.line_length = width;
    INFO("width: ", width);
    CHECK(idempotent(source, config));
  }
}

}  // namespace alcy_fmt
