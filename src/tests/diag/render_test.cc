// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/render.h"

#include <optional>
#include <string>
#include <string_view>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "doctest/doctest.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"
#include "fpag/term/style.h"

namespace diag {

namespace {

constexpr std::string_view SRC = "x := foo(1, 2)\ny := 2\n";

std::optional<SourceText> fetch_source(u32 file, const void*) {
  if (file == 3) {
    return SourceText{"main.al", SRC};
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
  DiagBag bag{arena};

  BagFixture() { arena.reserve(1u << 20); }
};

}  // namespace

TEST_CASE("Render without source") {
  BagFixture f;
  const u32 i = f.bag.emit(Severity::Error, 7, "broken {}", "thing");
  CHECK(render_str(*f.bag.at(i)) == "error[E7]: broken thing\n");

  const u32 j = f.bag.emit(Severity::Warning, 8, "shaky");
  CHECK(render_str(*f.bag.at(j)) == "warning[W8]: shaky\n");
}

TEST_CASE("Render with source snippet") {
  BagFixture f;
  const u32 i =
      f.bag.emit(Severity::Error, 1, Span{.file = 3, .offset = 5, .length = 3},
                 "bad call");
  CHECK(render_str(*f.bag.at(i)) ==
        "error[E1]: bad call\n"
        " --> main.al:1:6\n"
        "  |\n"
        "1 | x := foo(1, 2)\n"
        "  |      ^^^\n");
}

TEST_CASE("Render clips multi-line spans and rejects out-of-range offsets") {
  BagFixture f;
  // Length runs past the newline; only the first line is underlined.
  const u32 i =
      f.bag.emit(Severity::Error, 1, Span{.file = 3, .offset = 5, .length = 9},
                 "bad call");
  CHECK(render_str(*f.bag.at(i)) ==
        "error[E1]: bad call\n"
        " --> main.al:1:6\n"
        "  |\n"
        "1 | x := foo(1, 2)\n"
        "  |      ^^^^^^^^^\n");

  // An offset past the end of the file is an invalid span: render the
  // raw offset rather than a fabricated line/column.
  const u32 j =
      f.bag.emit(Severity::Error, 2,
                 Span{.file = 3, .offset = 1000, .length = 2}, "past the end");
  const std::string rendered = render_str(*f.bag.at(j));
  CHECK(rendered.find(" --> main.al:1000\n") != std::string::npos);
  CHECK(rendered.find("\n1 | ") == std::string::npos);
}

TEST_CASE("Render secondary labels") {
  BagFixture f;
  const u32 i =
      f.bag.emit(Severity::Error, 1, Span{.file = 3, .offset = 5, .length = 3},
                 "bad call");
  CHECK(f.bag.label(i, {{{.file = 3, .offset = 15, .length = 1}, "used here"}})
            .is_ok());
  CHECK(render_str(*f.bag.at(i)) ==
        "error[E1]: bad call\n"
        " --> main.al:1:6\n"
        "  |\n"
        "1 | x := foo(1, 2)\n"
        "  |      ^^^\n"
        " = note: used here --> main.al:2:1\n");
}

TEST_CASE("Label rejects an index that names no diagnostic") {
  BagFixture f;
  const u32 i = f.bag.emit(Severity::Error, 1, "broken");
  CHECK(f.bag.label(i, {}).is_ok());
  CHECK(f.bag.label(i + 1, {{{.file = 3, .offset = 1, .length = 1}, "nope"}})
            .is_err());
  CHECK(f.bag.at(i + 1) == nullptr);
}

TEST_CASE("Render unknown file") {
  BagFixture f;
  const u32 i =
      f.bag.emit(Severity::Error, 1, Span{.file = 42, .offset = 8, .length = 3},
                 "bad call");
  CHECK(render_str(*f.bag.at(i)) ==
        "error[E1]: bad call\n"
        " --> [unknown file]:8\n");
}

TEST_CASE("Render colorizes diagnostic elements") {
  BagFixture f;
  const u32 i =
      f.bag.emit(Severity::Error, 1, Span{.file = 3, .offset = 5, .length = 3},
                 "bad call");
  CHECK(f.bag.label(i, {{{.file = 3, .offset = 14, .length = 1}, "used here"}})
            .is_ok());

  const std::string colored = render_str(*f.bag.at(i), {.color = true});
  CHECK(colored.find(term::kBold) != std::string::npos);
  CHECK(colored.find(term::kRed) != std::string::npos);
  CHECK(colored.find(term::kCyan) != std::string::npos);
  CHECK(colored.find(term::kBlue) != std::string::npos);
  CHECK(colored.find("\x1b[38;2;") == std::string::npos);
  CHECK(colored.find("bad call\n") != std::string::npos);
  CHECK(colored.find(" = " + std::string(term::kBold) +
                     std::string(term::kCyan) + "note" +
                     std::string(term::kReset)) != std::string::npos);
}

TEST_CASE("Render uses severity-specific colors") {
  BagFixture f;
  const u32 error = f.bag.emit(Severity::Error, 1, "error");
  const u32 warning = f.bag.emit(Severity::Warning, 2, "warning");
  const u32 note = f.bag.emit(Severity::Note, 3, "note");

  const std::string error_text = render_str(*f.bag.at(error), {.color = true});
  const std::string warning_text =
      render_str(*f.bag.at(warning), {.color = true});
  const std::string note_text = render_str(*f.bag.at(note), {.color = true});
  CHECK(error_text.find(term::kRed) != std::string::npos);
  CHECK(warning_text.find(term::kYellow) != std::string::npos);
  CHECK(note_text.find(term::kCyan) != std::string::npos);
  CHECK(error_text.find("\x1b[38;5;") == std::string::npos);
  CHECK(warning_text.find("\x1b[38;5;") == std::string::npos);
  CHECK(note_text.find("\x1b[38;5;") == std::string::npos);
}

}  // namespace diag
