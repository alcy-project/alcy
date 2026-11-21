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
                    "`when` and `repr`"));
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
