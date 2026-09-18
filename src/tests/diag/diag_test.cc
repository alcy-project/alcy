// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#include <string>
#include <string_view>

#include "diag/bag.h"
#include "diag/diagnostic.h"
#include "diag/render.h"
#include "diag/span.h"
#include "doctest/doctest.h"
#include "fmt/format.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "ir/verifier.h"

namespace diag {

namespace {

constexpr std::string_view kSrc = "let x = foo(1, 2);\nlet y = 2;\n";

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

TEST_CASE("DiagBag counts and iteration") {
  BagFixture f;
  CHECK(!f.bag.has_errors());
  CHECK(f.bag.size() == 0);

  const u32 e = f.bag.emit(Severity::Error, 7, "broken {}", "thing");
  const u32 w = f.bag.emit(Severity::Warning, 8, "shaky");
  f.bag.emit(Severity::Note, 9, "fyi");

  CHECK(f.bag.size() == 3);
  CHECK(f.bag.has_errors());
  CHECK(f.bag.error_count() == 1);
  CHECK(f.bag.warning_count() == 1);
  CHECK(f.bag.at(e).code == 7);
  CHECK(f.bag.at(e).message == "broken thing");
  CHECK(f.bag.at(w).severity == Severity::Warning);

  u32 seen = 0;
  f.bag.for_each([&](const Diagnostic&) { ++seen; });
  CHECK(seen == 3);
}

TEST_CASE("DiagBag spans and labels") {
  BagFixture f;
  const Span span{.file = 3, .offset = 8, .length = 3};
  const u32 i =
      f.bag.emit(Severity::Error, 1, span, "bad call from {}", "here");
  CHECK(f.bag.at(i).has_primary_span);
  CHECK(f.bag.at(i).primary_span.offset == 8);
  CHECK(f.bag.at(i).label_count == 0);

  f.bag.label(i, {{{.file = 3, .offset = 23, .length = 1}, "used here"}});
  CHECK(f.bag.at(i).label_count == 1);
  CHECK(f.bag.at(i).labels[0].message == "used here");
}

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
      f.bag.emit(Severity::Error, 1, Span{.file = 3, .offset = 8, .length = 3},
                 "bad call");
  CHECK(render_str(f.bag.at(i)) ==
        "error[E1]: bad call\n"
        " --> main.al:1:9\n"
        "  |\n"
        "1 | let x = foo(1, 2);\n"
        "  |         ^^^\n");
}

TEST_CASE("Render clips multi-line spans and clamps offsets") {
  BagFixture f;
  // Length runs past the newline; only the first line is underlined.
  const u32 i =
      f.bag.emit(Severity::Error, 1, Span{.file = 3, .offset = 8, .length = 40},
                 "bad call");
  CHECK(render_str(f.bag.at(i)) ==
        "error[E1]: bad call\n"
        " --> main.al:1:9\n"
        "  |\n"
        "1 | let x = foo(1, 2);\n"
        "  |         ^^^^^^^^^^\n");

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
      f.bag.emit(Severity::Error, 1, Span{.file = 3, .offset = 8, .length = 3},
                 "bad call");
  f.bag.label(i, {{{.file = 3, .offset = 23, .length = 1}, "used here"}});
  CHECK(render_str(f.bag.at(i)) ==
        "error[E1]: bad call\n"
        " --> main.al:1:9\n"
        "  |\n"
        "1 | let x = foo(1, 2);\n"
        "  |         ^^^\n"
        " = note: used here --> main.al:2:5\n");
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

TEST_CASE("VerifyError converts to diagnostic") {
  const ir::VerifyError error{.kind = ir::VerifyErrorKind::UndefinedRegister,
                              .index = 5};
  const Diagnostic diag = ir::to_diagnostic(error);
  CHECK(diag.severity == Severity::Error);
  CHECK(diag.code >= 1000);
  CHECK(diag.code < 2000);
  CHECK(diag.message == "UndefinedRegister");
  CHECK(!diag.has_primary_span);

  // Codes are stable per kind.
  const ir::VerifyError other{.kind = ir::VerifyErrorKind::UnterminatedBlock,
                              .index = 0};
  CHECK(ir::to_diagnostic(other).code != diag.code);
}

TEST_CASE("Fallible result smoke test") {
  Fallible<int> ok = base::make_ok(3);
  CHECK(ok.is_ok());
  CHECK(!ok.is_err());

  Fallible<int> err = base::make_err(Fatal{});
  CHECK(err.is_err());
  CHECK(!err.is_ok());
}

}  // namespace diag
