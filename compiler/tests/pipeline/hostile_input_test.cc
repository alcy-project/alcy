// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string>
#include <string_view>
#include <utility>

#include "config/build_config.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "i18n/language.h"
#include "path/path.h"
#include "pipeline/check.h"
#include "pipeline/pipeline_context.h"

namespace pipeline {

#if !BUILD_FLAG(IS_OS_ASMJS)

namespace {

// A deterministic generator, so a failure reproduces exactly. The
// libFuzzer targets under fuzz/ explore the same ground interactively;
// this runs in the normal suite on every commit with no extra tooling.
//
// The oracle is deliberately weak: it cannot tell a *correct* answer
// from a merely non-crashing one. What it does guarantee is the
// invariant `docs/adr/0015-boundary-validation.md` states and nothing
// enforced until now - invalid
// source produces diagnostics, never a signal or an assertion. A wrong
// diagnostic is a bug, but it is a bug that answers, so it belongs to
// the property tests rather than here.
class Rng {
 public:
  explicit Rng(u64 seed) : state_(seed | 1) {}

  u64 next() {
    // xorshift64*: no platform-dependent width, no library state.
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1Dull;
  }

  usize below(usize bound) {
    return bound == 0 ? 0 : static_cast<usize>(next() % bound);
  }

 private:
  u64 state_;
};

// Fragments chosen to reach every lexer's branch and the parser's
// recovery paths, which is where malformed input is likeliest to be
// mishandled.
constexpr std::string_view FRAGMENTS[] = {
    "fn ",
    "main",
    "(",
    ")",
    "{",
    "}",
    "[",
    "]",
    "<",
    ">",
    ",",
    ";",
    ":",
    "::",
    "=",
    "==",
    ":=",
    "+",
    "-",
    "*",
    "/",
    "%",
    "&&",
    "||",
    "!",
    "~",
    "&",
    "&mut",
    "..",
    "...",
    "->",
    "=>",
    "|",
    "^",
    "<<",
    ">>",
    "if",
    "else",
    "while",
    "loop",
    "match",
    "in",
    "ret",
    "break",
    "cont",
    "let",
    "mut",
    "const",
    "static",
    "struct",
    "enum",
    "impl",
    "pub",
    "use",
    "as",
    "where",
    "self",
    "Self",
    "0",
    "1",
    "42",
    "0xFF",
    "0b1010",
    "0o17",
    "1_000",
    "1.5",
    "1e10",
    "1E-3",
    "42i32",
    "1.5f64",
    "18446744073709551616",
    "1_",
    "0x",
    "0b",
    "0o",
    "1..2",
    "\"s\"",
    "\"unterminated",
    "\"\\u{41}\"",
    "\"\\q\"",
    "'c'",
    "'\\u{1F600}'",
    "'",
    "//c\n",
    "/*n/*e*/s*/",
    "///d\n",
    "@",
    "$",
    "\\",
    "`",
    "?",
    "#",
    "\x01",
    "\x7f",
    "\xff",
    "\xc3\xa9",
};

// One generated program. Half are whole files, so the parser's item
// recovery is exercised rather than only expression fragments.
std::string generate(Rng& rng) {
  std::string text;
  const bool whole = rng.below(2) == 0;
  if (whole) {
    text += "fn main() ";
    text += (rng.below(2) == 0) ? "{\n" : "-> i32 {\n";
  }
  const usize length = 1 + rng.below(96);
  for (usize i = 0; i < length; ++i) {
    text += FRAGMENTS[rng.below(sizeof(FRAGMENTS) / sizeof(FRAGMENTS[0]))];
  }
  if (whole) {
    text += "\n}\n";
  }
  return text;
}

}  // namespace

TEST_CASE("Hostile input never crashes the checker") {
  // Enough shapes to reach every branch, still fast enough for the
  // normal suite. Each seed is a separate case name, so a failure names
  // the input that produced it.
  constexpr usize CASES = 400;
  Rng rng(0x9E3779B97F4A7C15ull);
  for (usize i = 0; i < CASES; ++i) {
    std::string source = generate(rng);
    // Half the cases are cut short: a prefix is where an index-based
    // reader runs off the end, and where recovery has to resynchronize.
    const bool truncate = rng.below(2) == 0;
    if (truncate) {
      source = source.substr(0, source.size() / 2);
    }
    const std::string name =
        "hostile/" + std::to_string(i) + (truncate ? " truncated" : "");

    INFO("case: " << name << " source: " << source);
    PipelineContext ctx{i18n::Language::EnUs};
    // The call returning at all is the assertion: a crash would take
    // the process with it. The source is held in memory, so a case costs
    // a string rather than a file.
    const base::Result<CheckOutcome, diag::Reported> result =
        check_source(ctx, "main.al", source);
    (void)result;
  }
}

TEST_CASE("Hostile input never crashes on raw bytes") {
  // Nothing the generator produces is valid UTF-8, so this covers the
  // lexer's byte-level paths and the renderer's column arithmetic.
  Rng rng(0xD1B54A32D192ED03ull);
  for (usize i = 0; i < 200; ++i) {
    std::string source;
    const usize length = rng.below(512);
    source.reserve(length);
    for (usize j = 0; j < length; ++j) {
      // Any byte at all, including the ones a text editor cannot type.
      source.push_back(static_cast<char>(rng.next() & 0xFF));
    }
    INFO("case: " << i);

    PipelineContext ctx{i18n::Language::EnUs};
    const base::Result<CheckOutcome, diag::Reported> result =
        check_source(ctx, "main.al", source);
    (void)result;
  }
}

TEST_CASE("Hostile path strings never crash") {
  // path::Path is a public boundary that takes a raw string, and a
  // package layout decides what arrives.
  Rng rng(0x2545F4914F6CDD1Dull);
  for (usize i = 0; i < 400; ++i) {
    std::string raw;
    const usize length = rng.below(24);
    for (usize j = 0; j < length; ++j) {
      raw.push_back(static_cast<char>("/\\.:.a..\0x*?"[rng.below(11)]));
    }
    INFO("case: " << i << " raw: " << raw);
    base::Result<path::Path, path::PathError> first =
        path::Path::from_native(raw);
    if (first.is_ok()) {
      const path::Path joined = std::move(first).unwrap().join("../x/./y");
      (void)joined.is_absolute();
      (void)joined.parent().as_view();
    }
  }
}
#endif  // !BUILD_FLAG(IS_OS_ASMJS)

}  // namespace pipeline
