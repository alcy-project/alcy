// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parser/attribute.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/ast.h"
#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "lexer/lexer.h"
#include "lexer/token.h"
#include "parser/parser.h"
#include "source/source.h"

namespace {

constexpr parser::BuildFacts LINUX_X86_64{.os = "linux",
                                          .arch = "x86_64",
                                          .freestanding = false};
constexpr parser::BuildFacts LINUX_AARCH64{.os = "linux",
                                           .arch = "aarch64",
                                           .freestanding = false};
constexpr parser::BuildFacts WINDOWS_X86_64{.os = "windows",
                                            .arch = "x86_64",
                                            .freestanding = false};
constexpr parser::BuildFacts WASI_WASM32{.os = "wasi",
                                         .arch = "wasm32",
                                         .freestanding = false};
constexpr parser::BuildFacts LINUX_FREESTANDING{.os = "linux",
                                                .arch = "x86_64",
                                                .freestanding = true};

struct Fixture {
  mem::Arena arena;
  ast::AstArena ast;
  diag::DiagBag bag{arena, i18n::Language::EnUs};

  Fixture() { arena.reserve(1u << 20); }
};

struct ParseResult {
  std::span<const ast::ItemIdx> items;
  bool ok;
};

ParseResult parse_with(std::string_view bytes,
                       parser::BuildFacts facts,
                       Fixture& f) {
  lexer::Lexer lexer(bytes, source::UNKNOWN_FILE, f.bag);
  std::vector<lexer::Token> tokens;
  lexer.tokenize(tokens);
  parser::Parser parser(
      std::span<const lexer::Token>(tokens.data(), tokens.size()), bytes,
      source::UNKNOWN_FILE, f.ast, f.bag, facts);
  base::Result<std::span<const ast::ItemIdx>, diag::Reported> parsed =
      parser.parse();
  if (parsed.is_err()) {
    return {{}, false};
  }
  auto items = std::move(parsed).unwrap();
  return {items, !f.bag.has_errors()};
}

bool has_message(const diag::DiagBag& bag, std::string_view message) {
  for (u32 i = 0; i < bag.size(); ++i) {
    const diag::Diagnostic* const diagnostic = bag.at(i);
    if (diagnostic != nullptr && diagnostic->message == message) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("A when gate matching the target keeps the item") {
  Fixture f;
  ParseResult r =
      parse_with("#[when(target.os == \"linux\")]\nfn x() -> i32 { ret 1 }\n",
                 LINUX_X86_64, f);
  CHECK(r.ok);
  CHECK(r.items.size() == 1);
}

TEST_CASE("A when gate not matching the target drops the item") {
  Fixture f;
  ParseResult r =
      parse_with("#[when(target.os == \"windows\")]\nfn x() -> i32 { ret 1 }\n",
                 LINUX_X86_64, f);
  CHECK(r.items.size() == 0);
}

TEST_CASE("The arch axis selects on the target's architecture") {
  Fixture f;
  ParseResult r = parse_with(
      "#[when(target.arch == \"wasm32\")]\nfn x() -> i32 { ret 1 }\n",
      LINUX_X86_64, f);
  CHECK(r.items.size() == 0);
  Fixture g;
  ParseResult s = parse_with(
      "#[when(target.arch == \"wasm32\")]\nfn x() -> i32 { ret 1 }\n",
      WASI_WASM32, g);
  CHECK(s.items.size() == 1);
}

TEST_CASE("The freestanding axis selects on the build flag") {
  Fixture host;
  ParseResult r =
      parse_with("#[when(target.freestanding)]\nfn x() -> i32 { ret 1 }\n",
                 LINUX_X86_64, host);
  CHECK(r.items.size() == 0);
  Fixture fs;
  ParseResult s =
      parse_with("#[when(target.freestanding)]\nfn x() -> i32 { ret 1 }\n",
                 LINUX_FREESTANDING, fs);
  CHECK(s.items.size() == 1);
  Fixture fs2;
  ParseResult t =
      parse_with("#[when(!target.freestanding)]\nfn x() -> i32 { ret 1 }\n",
                 LINUX_FREESTANDING, fs2);
  CHECK(t.items.size() == 0);
}

TEST_CASE("&& binds tighter than ||") {
  Fixture f;
  ParseResult r = parse_with(
      "#[when(target.os == \"linux\" || target.os == \"windows\" && "
      "target.arch == \"wasm32\")]\nfn x() -> i32 { ret 1 }\n",
      LINUX_X86_64, f);
  CHECK(r.items.size() == 1);
}

TEST_CASE("Parentheses group the operands they hold") {
  Fixture f;
  ParseResult r = parse_with(
      "#[when((target.os == \"linux\" || target.os == \"darwin\") && "
      "target.arch == \"wasm32\")]\nfn x() -> i32 { ret 1 }\n",
      LINUX_X86_64, f);
  CHECK(r.items.size() == 0);
}

TEST_CASE("! negates the operand it sits on") {
  Fixture f;
  ParseResult r = parse_with(
      "#[when(!(target.os == \"linux\"))]\nfn x() -> i32 { ret 1 }\n",
      LINUX_X86_64, f);
  CHECK(r.items.size() == 0);
  Fixture g;
  ParseResult s = parse_with(
      "#[when(!(target.os == \"linux\"))]\nfn x() -> i32 { ret 1 }\n",
      WINDOWS_X86_64, g);
  CHECK(s.items.size() == 1);
}

TEST_CASE("Multiple when attributes are conjunctive") {
  Fixture f;
  ParseResult r = parse_with(
      "#[when(target.os == \"linux\")]\n#[when(target.arch == "
      "\"aarch64\")]\nfn x() -> i32 { ret 1 }\n",
      LINUX_X86_64, f);
  CHECK(r.items.size() == 0);
  Fixture g;
  ParseResult s = parse_with(
      "#[when(target.os == \"linux\")]\n#[when(target.arch == "
      "\"aarch64\")]\nfn x() -> i32 { ret 1 }\n",
      LINUX_AARCH64, g);
  CHECK(s.items.size() == 1);
}

TEST_CASE("An unknown attribute is refused with guidance") {
  Fixture f;
  ParseResult r =
      parse_with("#[blorch(target.os == \"linux\")]\nfn x() -> i32 { ret 1 }\n",
                 LINUX_X86_64, f);
  (void)r;
  CHECK(has_message(f.bag,
                    "Unknown attribute `blorch`; the known attributes are "
                    "`when`, `repr`, `layout`, and `test`"));
}

TEST_CASE("An unknown value for a known key is refused with the known set") {
  Fixture f;
  ParseResult r =
      parse_with("#[when(target.os == \"amigaos\")]\nfn x() -> i32 { ret 1 }\n",
                 LINUX_X86_64, f);
  (void)r;
  CHECK(has_message(f.bag,
                    "Unknown value `amigaos` for `target.os`; the known values "
                    "are linux, darwin, windows, wasi, unknown"));
}

TEST_CASE("An unknown target key is refused with the known keys") {
  Fixture f;
  ParseResult r =
      parse_with("#[when(target.moon == \"linux\")]\nfn x() -> i32 { ret 1 }\n",
                 LINUX_X86_64, f);
  (void)r;
  CHECK(has_message(f.bag,
                    "Unknown `target` argument `moon`; the known arguments are "
                    "`os`, `arch`, and `freestanding`"));
}

TEST_CASE("A non-boolean condition is refused") {
  Fixture f;
  ParseResult r = parse_with("#[when(\"linux\")]\nfn x() -> i32 { ret 1 }\n",
                             LINUX_X86_64, f);
  (void)r;
  CHECK(has_message(f.bag, "`when` expects a boolean expression"));
}

TEST_CASE("An empty when() is refused") {
  Fixture f;
  ParseResult r =
      parse_with("#[when()]\nfn x() -> i32 { ret 1 }\n", LINUX_X86_64, f);
  (void)r;
  CHECK(f.bag.has_errors());
}

TEST_CASE("A method gate removes the method from the block") {
  Fixture f;
  ParseResult r = parse_with(
      "impl i32 {\n#[when(target.os == \"windows\")]\nfn m() -> i32 { ret 1 "
      "}\n#[when(target.os == \"linux\")]\nfn m() -> i32 { ret 2 }\n}\nfn "
      "main() -> i32 { ret 0 }\n",
      LINUX_X86_64, f);
  CHECK(r.ok);
  CHECK(r.items.size() == 2);
  CHECK(r.items[0].is_valid());
  if (!r.items[0].is_valid()) {
    return;
  }
  const ast::ItemNode& impl = f.ast.items[r.items[0]];
  CHECK(impl.kind == ast::ItemKind::Impl);
  const ast::ItemImpl& payload = impl.payload.get<ast::ItemImpl>();
  CHECK(payload.methods.size() == 1);
}

TEST_CASE("Two arms for one name do not collide") {
  Fixture f;
  ParseResult r = parse_with(
      "#[when(target.os == \"linux\")]\nfn x() -> i32 { ret 1 "
      "}\n#[when(target.os == \"windows\")]\nfn x() -> i32 { ret 2 }\nfn "
      "main() -> i32 { ret x() }\n",
      LINUX_X86_64, f);
  CHECK(r.ok);
  CHECK(r.items.size() == 2);
}
TEST_CASE("A repr(C) struct records the marker") {
  Fixture f;
  ParseResult r = parse_with(
      "#[repr(C)]\nstruct Pair { a: u8, b: i32 }\nfn main() -> i32 { ret 0 }\n",
      LINUX_X86_64, f);
  CHECK(r.ok);
  const ast::ItemNode& item = f.ast.items[r.items[0]];
  CHECK(item.repr_c);
}

TEST_CASE("An unknown repr is refused with the known repr named") {
  Fixture f;
  ParseResult r = parse_with(
      "#[repr(D)]\nstruct Pair { a: i32 }\nfn main() -> i32 { ret 0 }\n",
      LINUX_X86_64, f);
  (void)r;
  CHECK(has_message(f.bag, "Unknown repr `D`; the known repr is `C`"));
}

TEST_CASE("repr(C) on a function is refused") {
  Fixture f;
  ParseResult r =
      parse_with("#[repr(C)]\nfn main() -> i32 { ret 0 }\n", LINUX_X86_64, f);
  (void)r;
  CHECK(has_message(f.bag, "`repr(C)` applies to structs"));
}

TEST_CASE("repr(C) on a struct with no fields is refused") {
  Fixture f;
  ParseResult r =
      parse_with("#[repr(C)]\nstruct Empty {}\nfn main() -> i32 { ret 0 }\n",
                 LINUX_X86_64, f);
  (void)r;
  CHECK(has_message(f.bag,
                    "`repr(C)` does not apply to a struct with no fields"));
}

TEST_CASE("repr(C) and a measuring gate conjunct on one item") {
  Fixture f;
  ParseResult r = parse_with(
      "#[repr(C)]\n#[when(target.os == \"linux\")]\nstruct Pair { a: u8, b: "
      "i32 }\nfn main() -> i32 { ret 0 }\n",
      LINUX_X86_64, f);
  CHECK(r.ok);
  const ast::ItemNode& item = f.ast.items[r.items[0]];
  CHECK(item.repr_c);
  CHECK(!item.when_false);
}
TEST_CASE("The testing fact keeps and drops a gated test function") {
  constexpr std::string_view SOURCE =
      "#[when(testing)]\n#[test] fn kept() {\n}\n"
      "#[when(!testing)]\n#[test] fn dropped() {\n}\n";
  parser::BuildFacts testing = LINUX_X86_64;
  testing.testing = true;
  Fixture in_test;
  const ParseResult kept = parse_with(SOURCE, testing, in_test);
  CHECK(kept.ok);
  CHECK(kept.items.size() == 1);
  CHECK(in_test.ast.items[kept.items[0]].payload.get<ast::ItemFn>().name.name ==
        "kept");

  Fixture in_build;
  const ParseResult dropped = parse_with(SOURCE, LINUX_X86_64, in_build);
  CHECK(dropped.ok);
  CHECK(dropped.items.size() == 1);
  CHECK(in_build.ast.items[dropped.items[0]]
            .payload.get<ast::ItemFn>()
            .name.name == "dropped");
}

TEST_CASE("A boolean target compared to a string literal is named as boolean") {
  Fixture f;
  ParseResult r = parse_with(
      "#[when(target.freestanding == \"true\")]\nfn x() -> i32 { ret 1 }\n",
      LINUX_X86_64, f);
  (void)r;
  CHECK(has_message(
      f.bag,
      "`target.freestanding` is a boolean; compare it to `true` or `false`"));
}

TEST_CASE("The testing fact compared to a string literal is named as testing") {
  Fixture f;
  ParseResult r =
      parse_with("#[when(testing == \"yes\")]\nfn x() -> i32 { ret 1 }\n",
                 LINUX_X86_64, f);
  (void)r;
  CHECK(has_message(f.bag,
                    "`testing` is a boolean; compare it to `true` or `false`"));
}

TEST_CASE("A string target compared to a boolean literal is named as string") {
  Fixture f;
  ParseResult r = parse_with(
      "#[when(target.os == true)]\nfn x() -> i32 { ret 1 }\n", LINUX_X86_64, f);
  (void)r;
  CHECK(has_message(
      f.bag, "`target.os` is a string; compare it to one of the known values"));
}
TEST_CASE("The wasi os value is a recognized arm") {
  constexpr parser::BuildFacts kWasi{
      .os = "wasi", .arch = "wasm32", .freestanding = false};
  Fixture f;
  ParseResult r = parse_with(
      "#[when(target.os == \"wasi\")]\nfn x() -> i32 { ret 1 }\n", kWasi, f);
  CHECK(r.ok);
  CHECK(r.items.size() == 1);
}

TEST_CASE("A layout attribute records its settings") {
  Fixture f;
  ParseResult r = parse_with(
      "#[layout(min_align = 16, max_align = 16)]\n"
      "struct Wide { x: u8 }\n"
      "#[layout(tag_size = 1)]\n"
      "enum Flag { Off, On }\n"
      "fn main() -> i32 { ret 0 }\n",
      LINUX_X86_64, f);
  CHECK(r.ok);
  CHECK(r.items.size() >= 2);
  const ast::ItemNode& wide = f.ast.items[r.items[0]];
  const ast::ItemNode& flag = f.ast.items[r.items[1]];
  CHECK(wide.payload.get<ast::ItemStruct>().layout.min_align == 16);
  CHECK(wide.payload.get<ast::ItemStruct>().layout.max_align == 16);
  CHECK(flag.payload.get<ast::ItemEnum>().layout.tag_size == 1);
}

TEST_CASE("A field layout attribute records its setting") {
  Fixture f;
  ParseResult r = parse_with(
      "struct Packed {\n"
      "  a: u8,\n"
      "  #[layout(max_align = 1)] tail: [u16; 0],\n"
      "}\n"
      "fn main() -> i32 { ret 0 }\n",
      LINUX_X86_64, f);
  CHECK(r.ok);
  CHECK(r.items.size() >= 1);
  const auto& fields =
      f.ast.items[r.items[0]].payload.get<ast::ItemStruct>().fields;
  CHECK(fields.size() == 2);
  CHECK(fields[1].layout.max_align == 1);
}

TEST_CASE("Layout settings are validated") {
  struct Case {
    std::string_view source;
    std::string_view message;
  };
  const Case cases[] = {
      {"#[layout(nope = 2)]\nstruct S { a: u8 }\n",
       "Unknown layout setting `nope`; the known settings are `min_align`, "
       "`max_align`, and `tag_size`"},
      {"#[layout(min_align = 3)]\nstruct S { a: u8 }\n",
       "A layout size must be a power of two"},
      {"#[layout(tag_size = 0)]\nenum E { A, B }\n",
       "A layout size must be a power of two"},
      {"#[layout(tag_size = 3)]\nenum E { A, B }\n",
       "A layout size must be a power of two"},
      {"#[layout(tag_size = 16)]\nenum E { A, B }\n",
       "`tag_size` must be 1, 2, 4, or 8"},
      {"#[layout(tag_size = 1)]\nstruct S { a: u8 }\n",
       "`tag_size` applies to enums"},
      {"#[layout(min_align = 16, max_align = 1)]\nstruct S { a: u8 }\n",
       "`min_align` cannot exceed `max_align`"},
  };
  for (const Case& c : cases) {
    Fixture f;
    ParseResult r =
        parse_with(std::string(c.source) + "fn main() -> i32 { ret 0 }\n",
                   LINUX_X86_64, f);
    (void)r;
    CHECK(has_message(f.bag, c.message));
  }
}

TEST_CASE("Layout values and targets are validated") {
  struct Case {
    std::string_view source;
    std::string_view message;
  };
  const Case cases[] = {
      {"#[layout(tag_size = 8)]\nenum E { A, B }\n", ""},
      {"#[layout(min_align = 32)]\nstruct S { a: u8 }\n",
       "An alignment must be a power of two from 1 to 16"},
      {"#[layout(min_align = 8)]\nfn f() {}\n",
       "Layout settings apply to structs and enums"},
      {"struct S { #[when(false)] a: u8 }\n",
       "Attribute `when` does not apply to a field"},
      {"#[layout(min_align = 256)]\nstruct S { a: u8 }\n",
       "An alignment must be a power of two from 1 to 16"},
      {"#[layout(min_align = 0)]\nstruct S { a: u8 }\n",
       "A layout size must be a power of two"},
  };
  for (const Case& c : cases) {
    Fixture f;
    ParseResult r =
        parse_with(std::string(c.source) + "fn main() -> i32 { ret 0 }\n",
                   LINUX_X86_64, f);
    if (c.message.empty()) {
      CHECK(r.ok);
      CHECK(!f.bag.has_errors());
    } else {
      (void)r;
      CHECK(has_message(f.bag, c.message));
    }
  }
}
