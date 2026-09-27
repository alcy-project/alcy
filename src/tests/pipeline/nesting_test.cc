// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "base/nesting.h"

#include <string>
#include <string_view>

#include "config/build_config.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/io/temp_dir.h"
#include "pipeline/check.h"
#include "pipeline/pipeline_context.h"

namespace pipeline {

namespace {

struct CheckOutcome {
  bool ran = false;
  bool rejected = false;
  bool too_deep = false;
};

// Runs a whole check and reports what came back. The point of every case
// here is that the compiler answers with a diagnostic; a crash would end
// the process rather than return, so a surviving call is the assertion.
CheckOutcome run_check(const std::string& source) {
  io::TempDir dir = io::TempDir::create_unique("alcy_nesting_test_");
  CheckOutcome outcome;
  if (!dir.write_file("main.al", source)) {
    return outcome;
  }
  PipelineContext ctx;
  const base::Result<CheckResult, diag::Reported> result =
      check_single_file(ctx, dir.join("main.al"));
  outcome.ran = true;
  outcome.rejected = ctx.bag.has_errors();
  for (u32 i = 0; i < ctx.bag.size(); ++i) {
    const u32 code = ctx.bag.at(i)->code;
    if (code == 3004 || code == 4050 || code == 5005) {
      outcome.too_deep = true;
    }
  }
  return outcome;
}

// Repeats one opener/closer past the budget, which is the shape that used
// to exhaust the stack instead of reporting.
std::string nested(const std::string_view& open,
                   const std::string_view& close,
                   usize depth,
                   const std::string_view& body) {
  std::string out;
  out.reserve(static_cast<usize>(depth) * (open.size() + close.size()) +
              body.size());
  for (usize i = 0; i < depth; ++i) {
    out += open;
  }
  out += body;
  for (usize i = 0; i < depth; ++i) {
    out += close;
  }
  return out;
}

// Far enough past the budget that a guard which merely off-by-one still
// rejects, while staying small enough to build quickly.
constexpr usize WAY_PAST = static_cast<usize>(base::MAX_NESTING) * 8;

}  // namespace

#if !BUILD_FLAG(IS_OS_ASMJS)

// Well under the budget, so these must still compile: a guard that
// rejected ordinary nesting would be worse than the crash it prevents.
TEST_CASE("Nesting within the budget still checks") {
  const CheckOutcome parens = run_check("fn main() -> i32 {\n  ret " +
                                        nested("(", ")", 8, "1") + "\n}\n");
  CHECK(parens.ran);
  CHECK(!parens.rejected);
  CHECK(!parens.too_deep);

  const CheckOutcome blocks =
      run_check("fn main() " + nested("{", "}", 8, "") + "\n");
  CHECK(blocks.ran);
  CHECK(!blocks.rejected);
  CHECK(!blocks.too_deep);
}

// Each shape below reached the stack limit before the budget existed.
// The parser bounds the descent; the analyzer bounds the tree, which a
// long operator chain builds in a loop and so the parser never sees as
// deep.
TEST_CASE("Deep nesting is a diagnostic, not a crash") {
  const CheckOutcome parens = run_check(
      "fn main() -> i32 {\n  ret " + nested("(", ")", WAY_PAST, "1") + "\n}\n");
  CHECK(parens.ran);
  CHECK(parens.rejected);
  CHECK(parens.too_deep);

  const CheckOutcome blocks =
      run_check("fn main() " + nested("{", "}", WAY_PAST, "") + "\n");
  CHECK(blocks.ran);
  CHECK(blocks.rejected);
  CHECK(blocks.too_deep);

  const CheckOutcome unary =
      run_check("fn main() {\n  x := " + std::string(WAY_PAST, '!') +
                "true\n  _ := x\n}\n");
  CHECK(unary.ran);
  CHECK(unary.rejected);
  CHECK(unary.too_deep);

  // A long operator chain is a shallow parse and a deep tree, so only
  // the analyzer's own budget rejects it.
  std::string chain = "fn main() {\n  x := ";
  for (usize i = 0; i < WAY_PAST; ++i) {
    chain += "1+";
  }
  chain += "1\n  _ := x\n}\n";
  const CheckOutcome operators = run_check(chain);
  CHECK(operators.ran);
  CHECK(operators.rejected);
  CHECK(operators.too_deep);
}

TEST_CASE("Deeply nested types are a diagnostic") {
  const CheckOutcome types = run_check(
      "fn f() -> " + nested("[", "]", WAY_PAST, "u8") + " {\n  x\n}\n");
  CHECK(types.ran);
  CHECK(types.rejected);
  CHECK(types.too_deep);
}

TEST_CASE("Deeply nested tuple patterns are a diagnostic") {
  // A tuple pattern needs two elements, so this nests for real where
  // `(((x)))` would collapse back to a single binding.
  std::string pattern;
  for (usize i = 0; i < WAY_PAST; ++i) {
    pattern += "(";
  }
  pattern += "x, y";
  for (usize i = 0; i < WAY_PAST; ++i) {
    pattern += ")";
  }
  const CheckOutcome patterns =
      run_check("fn main() {\n  " + pattern + " := ()\n}\n");
  CHECK(patterns.ran);
  CHECK(patterns.rejected);
  CHECK(patterns.too_deep);
}

#endif  // !BUILD_FLAG(IS_OS_ASMJS)

}  // namespace pipeline
