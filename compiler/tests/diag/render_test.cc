// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/render.h"

#include <optional>
#include <string>
#include <string_view>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "diag/stage.h"
#include "doctest/doctest.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "fpag/term/style.h"
#include "i18n/language.h"

namespace diag {

namespace {

constexpr std::string_view SRC = "x := foo(1, 2)\ny := 2\n";
// Two wide lines for the column tests: a multi-byte character and a
// tab, each of which a byte column would misplace.
constexpr std::string_view WIDE_SRC = "  s := \"\xe6\x97\xa5\xe6\x9c\xac\" @";
constexpr std::string_view TABBED_SRC = "fn f() {\n\tx := @\n}\n";
// A line long enough that printing it whole would bury the message it
// belongs to; the `@` sits four hundred characters from either end. A
// function-local static keeps the view it is rendered through valid.
std::string_view long_src() {
  static std::string text =
      "x = " + std::string(400, 'a') + " @" + std::string(400, 'b') + "\n";
  return text;
}

std::optional<SourceText> fetch_source(u32 file, const void*) {
  if (file == 3) {
    return SourceText{"main.al", SRC};
  }
  if (file == 4) {
    return SourceText{"wide.al", WIDE_SRC};
  }
  if (file == 5) {
    return SourceText{"tabbed.al", TABBED_SRC};
  }
  if (file == 6) {
    return SourceText{"long.al", long_src()};
  }
  return std::nullopt;
}

std::string render_str(const Diagnostic& diag,
                       const RenderOptions& options = {},
                       SourceFetch fetch = fetch_source) {
  fmt::memory_buffer out;
  render(diag, out, options, fetch, nullptr);
  return std::string(out.data(), out.size());
}

struct BagFixture {
  mem::Arena arena;
  DiagBag bag{arena, i18n::Language::EnUs};

  BagFixture() { arena.reserve(1u << 20); }
};

}  // namespace

TEST_CASE("Render without source") {
  BagFixture f;
  const u32 i = f.bag.emit_untranslated(Severity::Error, Stage::Lexer, 7,
                                        "broken {}", "thing");
  CHECK(render_str(*f.bag.at(i)) == "error[EA007]: broken thing\n");

  const u32 j =
      f.bag.emit_untranslated(Severity::Warning, Stage::Lexer, 8, "shaky");
  CHECK(render_str(*f.bag.at(j)) == "warning[WA008]: shaky\n");
}

// A message from outside a check area has no code to show, so the marker
// is the word alone. Every severity takes the same shape.
TEST_CASE("Render without a code omits the bracket") {
  const Diagnostic coded{.severity = Severity::Error,
                         .code = Code{Stage::Lexer, 7},
                         .message = "has a code",
                         .primary_span = {}};
  CHECK(render_str(coded) == "error[EA007]: has a code\n");

  const Diagnostic uncoded{.severity = Severity::Error,
                           .code = std::nullopt,
                           .message = "has none",
                           .primary_span = {}};
  CHECK(render_str(uncoded) == "error: has none\n");

  const Diagnostic warned{.severity = Severity::Warning,
                          .code = std::nullopt,
                          .message = "hmm",
                          .primary_span = {}};
  CHECK(render_str(warned) == "warning: hmm\n");

  const Diagnostic noted{.severity = Severity::Note,
                         .code = std::nullopt,
                         .message = "fyi",
                         .primary_span = {}};
  CHECK(render_str(noted) == "note: fyi\n");
}

TEST_CASE("Render with source snippet") {
  BagFixture f;
  const u32 i = f.bag.emit_untranslated(
      Severity::Error, Stage::Lexer, 1,
      Span{.file = 3, .offset = 5, .length = 3}, "bad call");
  CHECK(render_str(*f.bag.at(i)) ==
        "error[EA001]: bad call\n"
        " --> main.al:1:6\n"
        "  |\n"
        "1 | x := foo(1, 2)\n"
        "  |      ^^^\n");
}

TEST_CASE("Render clips multi-line spans and rejects out-of-range offsets") {
  BagFixture f;
  // The span ends at the line break, so only line 1 is underlined.
  const u32 i = f.bag.emit_untranslated(
      Severity::Error, Stage::Lexer, 1,
      Span{.file = 3, .offset = 5, .length = 9}, "bad call");
  CHECK(render_str(*f.bag.at(i)) ==
        "error[EA001]: bad call\n"
        " --> main.al:1:6\n"
        "  |\n"
        "1 | x := foo(1, 2)\n"
        "  |      ^^^^^^^^^\n");

  // An offset past the end of the file is an invalid span: render the
  // raw offset rather than a fabricated line/column.
  const u32 j = f.bag.emit_untranslated(
      Severity::Error, Stage::Lexer, 2,
      Span{.file = 3, .offset = 1000, .length = 2}, "past the end");
  const std::string rendered = render_str(*f.bag.at(j));
  CHECK(rendered.find(" --> main.al:1000\n") != std::string::npos);
  CHECK(rendered.find("\n1 | ") == std::string::npos);
}

TEST_CASE("Render secondary labels") {
  BagFixture f;
  const u32 i = f.bag.emit_untranslated(
      Severity::Error, Stage::Lexer, 1,
      Span{.file = 3, .offset = 5, .length = 3}, "bad call");
  CHECK(f.bag.label(i, {{{.file = 3, .offset = 15, .length = 1}, "used here"}})
            .is_ok());
  CHECK(render_str(*f.bag.at(i)) ==
        "error[EA001]: bad call\n"
        " --> main.al:1:6\n"
        "  |\n"
        "1 | x := foo(1, 2)\n"
        "  |      ^^^\n"
        " = note: used here --> main.al:2:1\n");
}

TEST_CASE("Label rejects an index that names no diagnostic") {
  BagFixture f;
  const u32 i =
      f.bag.emit_untranslated(Severity::Error, Stage::Lexer, 1, "broken");
  CHECK(f.bag.label(i, {}).is_ok());
  CHECK(f.bag.label(i + 1, {{{.file = 3, .offset = 1, .length = 1}, "nope"}})
            .is_err());
  CHECK(f.bag.at(i + 1) == nullptr);
}

TEST_CASE("Render unknown file") {
  BagFixture f;
  const u32 i = f.bag.emit_untranslated(
      Severity::Error, Stage::Lexer, 1,
      Span{.file = 42, .offset = 8, .length = 3}, "bad call");
  CHECK(render_str(*f.bag.at(i)) ==
        "error[EA001]: bad call\n"
        " --> [unknown file]:8\n");
}

TEST_CASE("Render colorizes diagnostic elements") {
  BagFixture f;
  const u32 i = f.bag.emit_untranslated(
      Severity::Error, Stage::Lexer, 1,
      Span{.file = 3, .offset = 5, .length = 3}, "bad call");
  CHECK(f.bag.label(i, {{{.file = 3, .offset = 14, .length = 1}, "used here"}})
            .is_ok());

  const std::string colored = render_str(*f.bag.at(i), {.color = true});
  CHECK(colored.find(term::BOLD) != std::string::npos);
  CHECK(colored.find(term::FG_RED) != std::string::npos);
  CHECK(colored.find(term::FG_CYAN) != std::string::npos);
  CHECK(colored.find(term::FG_BLUE) != std::string::npos);
  CHECK(colored.find("\x1b[38;2;") == std::string::npos);
  CHECK(colored.find("bad call\n") != std::string::npos);
  CHECK(colored.find(" = " + std::string(term::BOLD) +
                     std::string(term::FG_CYAN) + "note" +
                     std::string(term::RESET)) != std::string::npos);
}

TEST_CASE("Render uses severity-specific colors") {
  BagFixture f;
  const u32 error =
      f.bag.emit_untranslated(Severity::Error, Stage::Lexer, 1, "error");
  const u32 warning =
      f.bag.emit_untranslated(Severity::Warning, Stage::Lexer, 2, "warning");
  const u32 note =
      f.bag.emit_untranslated(Severity::Note, Stage::Lexer, 3, "note");

  const std::string error_text = render_str(*f.bag.at(error), {.color = true});
  const std::string warning_text =
      render_str(*f.bag.at(warning), {.color = true});
  const std::string note_text = render_str(*f.bag.at(note), {.color = true});
  CHECK(error_text.find(term::FG_RED) != std::string::npos);
  CHECK(warning_text.find(term::FG_YELLOW) != std::string::npos);
  CHECK(note_text.find(term::FG_CYAN) != std::string::npos);
  CHECK(error_text.find("\x1b[38;5;") == std::string::npos);
  CHECK(warning_text.find("\x1b[38;5;") == std::string::npos);
  CHECK(note_text.find("\x1b[38;5;") == std::string::npos);
}

TEST_CASE("Render counts columns in characters, not bytes") {
  // The `@` is display column 13 but byte offset 16, so a byte column
  // would put the caret three columns to its right.
  BagFixture f;
  const u32 i = f.bag.emit_untranslated(
      Severity::Error, Stage::Lexer, 1,
      Span{.file = 4, .offset = 16, .length = 1}, "bad");
  CHECK(render_str(*f.bag.at(i)) ==
        "error[EA001]: bad\n"
        " --> wide.al:1:13\n"
        "  |\n"
        "1 |   s := \"\xe6\x97\xa5\xe6\x9c\xac\" @\n"
        "  |             ^\n");
}

TEST_CASE("Render expands tabs so the caret lands under its character") {
  // The leading tab becomes four spaces, so the `@` is column 10.
  BagFixture f;
  const u32 i = f.bag.emit_untranslated(
      Severity::Error, Stage::Lexer, 1,
      Span{.file = 5, .offset = 15, .length = 1}, "bad");
  CHECK(render_str(*f.bag.at(i)) ==
        "error[EA001]: bad\n"
        " --> tabbed.al:2:10\n"
        "  |\n"
        "2 |     x := @\n"
        "  |          ^\n");
}

TEST_CASE("Render windows a line too long to print whole") {
  // The snippet shows part of the line, cut on both sides, and the
  // caret still lands under its character.
  BagFixture f;
  const u32 i = f.bag.emit_untranslated(
      Severity::Error, Stage::Lexer, 1,
      Span{.file = 6, .offset = 405, .length = 1}, "bad");
  const std::string rendered = render_str(*f.bag.at(i));
  const usize snippet_at = rendered.find("1 | ");
  const usize caret_at = rendered.rfind("  | ");
  CHECK(snippet_at != std::string::npos);
  CHECK(caret_at != std::string::npos);
  if (snippet_at == std::string::npos || caret_at == std::string::npos) {
    return;
  }
  const usize at_col = rendered.find('@', snippet_at) - snippet_at;
  const usize caret_col = rendered.find('^', caret_at) - caret_at;
  CHECK(at_col == caret_col);
  // Cut on both sides: an ellipsis before the caret and the line
  // bounded rather than the four hundred characters either way.
  CHECK(rendered.find("...", snippet_at) < snippet_at + at_col);
  CHECK(rendered.size() < 400);
}

}  // namespace diag
