// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "diag/render.h"

#include <string>
#include <string_view>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/span.h"
#include "doctest/doctest.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/mem/arena.h"

namespace diag {

namespace {

constexpr std::string_view kSrc = "x := foo(1, 2)\ny := 2\n";

SourceText fetch_source(u32 file, const void*) {
  if (file == 3) {
    return {"main.al", kSrc};
  }
  return {};
}

std::string render_str(const Diagnostic& diag,
                       SourceFetch fetch = fetch_source) {
  fmt::memory_buffer out;
  render(diag, out, {}, fetch, nullptr);
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
  CHECK(render_str(f.bag.at(i)) == "error[E7]: broken thing\n");

  const u32 j = f.bag.emit(Severity::Warning, 8, "shaky");
  CHECK(render_str(f.bag.at(j)) == "warning[W8]: shaky\n");
}

TEST_CASE("Render with source snippet") {
  BagFixture f;
  const u32 i =
      f.bag.emit(Severity::Error, 1, Span{.file = 3, .offset = 5, .length = 3},
                 "bad call");
  CHECK(render_str(f.bag.at(i)) ==
        "error[E1]: bad call\n"
        " --> main.al:1:6\n"
        "  |\n"
        "1 | x := foo(1, 2)\n"
        "  |      ^^^\n");
}

TEST_CASE("Render clips multi-line spans and clamps offsets") {
  BagFixture f;
  // Length runs past the newline; only the first line is underlined.
  const u32 i =
      f.bag.emit(Severity::Error, 1, Span{.file = 3, .offset = 5, .length = 9},
                 "bad call");
  CHECK(render_str(f.bag.at(i)) ==
        "error[E1]: bad call\n"
        " --> main.al:1:6\n"
        "  |\n"
        "1 | x := foo(1, 2)\n"
        "  |      ^^^^^^^^^\n");

  // Out-of-range offset clamps to the end of the buffer.
  const u32 j =
      f.bag.emit(Severity::Error, 2,
                 Span{.file = 3, .offset = 1000, .length = 2}, "past the end");
  const std::string rendered = render_str(f.bag.at(j));
  CHECK(rendered.find(" --> main.al:3:1\n") != std::string::npos);
}

TEST_CASE("Render secondary labels") {
  BagFixture f;
  const u32 i =
      f.bag.emit(Severity::Error, 1, Span{.file = 3, .offset = 5, .length = 3},
                 "bad call");
  f.bag.label(i, {{{.file = 3, .offset = 23, .length = 1}, "used here"}});
  CHECK(render_str(f.bag.at(i)) ==
        "error[E1]: bad call\n"
        " --> main.al:1:6\n"
        "  |\n"
        "1 | x := foo(1, 2)\n"
        "  |      ^^^\n"
        " = note: used here --> main.al:3:1\n");
}

TEST_CASE("Render unknown file") {
  BagFixture f;
  const u32 i =
      f.bag.emit(Severity::Error, 1, Span{.file = 42, .offset = 8, .length = 3},
                 "bad call");
  CHECK(render_str(f.bag.at(i)) ==
        "error[E1]: bad call\n"
        " --> [unknown file]:8\n");
}

}  // namespace diag
