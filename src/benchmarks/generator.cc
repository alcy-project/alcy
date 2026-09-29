// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/generator.h"

#include <iterator>
#include <string>
#include <string_view>

#include "fmt/core.h"
#include "fpag/base/numeric.h"

namespace bench {

namespace {

// FNV-1a, so the digest needs no dependency and no second reader. It is
// a label for "were these the same bytes", not a defense against anyone
// choosing them.
constexpr u64 FNV_OFFSET = 14695981039346656037ull;
constexpr u64 FNV_PRIME = 1099511628211ull;

std::string hex16(u64 value) {
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string out(16, '0');
  for (usize i = 0; i < 16; ++i) {
    out[15 - i] = DIGITS[value & 0xF];
    value >>= 4;
  }
  return out;
}

// One `if` nested `depth` deep, as a statement rather than a tail: an
// `if` without `else` yields `()`, so it cannot be the last thing in a
// function returning a value. Nesting is the dimension that changes the
// descent's shape rather than its width, and it stays inside the
// language's own budget.
void append_block(std::string& out, u32 depth, u32 counter) {
  if (depth == 0) {
    fmt::format_to(std::back_inserter(out), "    _ := s\n");
    return;
  }
  out += "  if s > ";
  fmt::format_to(std::back_inserter(out), "{}i32 {{\n", counter);
  append_block(out, depth - 1, counter + 1);
  out += "  }\n";
}

}  // namespace

std::string generate_source(SourceSpec spec) {
  std::string out;
  // A function per unit of work, each with a body that costs roughly the
  // same, so a case's cost tracks the dimensions rather than a few
  // outliers. `mut` is what makes the accumulating statements possible:
  // a binding is immutable otherwise, and an assignment to one is a
  // diagnostic rather than code.
  for (u32 f = 0; f < spec.functions; ++f) {
    // The index is separated by an underscore because a trailing digit
    // run is a number: `f32` names a literal, not a function, and the
    // generated source would not parse.
    fmt::format_to(std::back_inserter(out),
                   "fn f_{}(a: i32, b: i32) -> i32 {{\n"
                   "  mut s := a\n"
                   "  s += b\n",
                   f);
    for (u32 s = 0; s < spec.statements; ++s) {
      out += "  s += 1i32\n";
    }
    append_block(out, spec.depth, f);
    out += "  ret s\n}\n\n";
  }
  out += "fn main() {\n  _ := f_0(1i32, 2i32)\n}\n";
  return out;
}

std::string source_digest(std::string_view source) {
  u64 hash = FNV_OFFSET;
  for (const char c : source) {
    hash ^= static_cast<u64>(static_cast<unsigned char>(c));
    hash *= FNV_PRIME;
  }
  return hex16(hash);
}

}  // namespace bench
