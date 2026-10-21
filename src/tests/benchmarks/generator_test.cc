// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/generator.h"

#include <string>

#include "benchmarks/fixture.h"
#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
#include "i18n/language.h"
#include "ir/verifier.h"
#include "pipeline/check.h"
#include "pipeline/pipeline_context.h"

namespace bench {

TEST_CASE("Generated source is the same bytes for the same dimensions") {
  const SourceSpec spec{.functions = 8, .statements = 3, .depth = 2};
  const std::string first = generate_source(spec);
  const std::string second = generate_source(spec);
  CHECK(first == second);
  CHECK(!first.empty());
  CHECK(source_digest(first) == source_digest(second));
}

TEST_CASE("Each dimension changes the source and its digest") {
  const SourceSpec base{.functions = 8, .statements = 3, .depth = 2};
  const std::string digest = source_digest(generate_source(base));

  SourceSpec more_functions = base;
  more_functions.functions = 9;
  CHECK(source_digest(generate_source(more_functions)) != digest);

  SourceSpec more_statements = base;
  more_statements.statements = 4;
  CHECK(source_digest(generate_source(more_statements)) != digest);

  SourceSpec deeper = base;
  deeper.depth = 3;
  CHECK(source_digest(generate_source(deeper)) != digest);
}

TEST_CASE("A generated source is a program the compiler accepts") {
  // The micro cases time the lexer and the parser over this text. If it
  // did not parse, the numbers would be error recovery, and every case
  // built on it would be quietly measuring the wrong thing. The width
  // crosses the point where a bare index stops being an identifier.
  pipeline::PipelineContext ctx{i18n::Language::EnUs};
  const std::string source =
      generate_source(SourceSpec{.functions = 40, .statements = 2, .depth = 2});
  base::Result<pipeline::CheckOutcome, diag::Reported> checked =
      pipeline::check_source(ctx, "bench.al", source);
  CHECK(checked.is_ok());
  CHECK(!ctx.bag.has_errors());
}

TEST_CASE("The fixture carries a compile through every stage") {
  // Every case in the pipeline group is a figure about this fixture, so
  // a stage that quietly failed would make all of them about the wrong
  // work. Each stage is checked, and the storage is checked with the
  // verifier rather than taken on trust.
  CompilerFixture fixture(
      SourceSpec{.functions = 8, .statements = 2, .depth = 2});
  fixture.resolve();
  CHECK(fixture.ok());
  fixture.analyze();
  CHECK(fixture.ok());
  fixture.lower();
  CHECK(fixture.ok());
  // The lowerer is what hands the verifier a proof; asking the verifier
  // directly is what a `verify-ir` case does.
  CHECK(ir::verify_storage(*fixture.lowered().storage).is_ok());
}

TEST_CASE("The fixture says so when a stage reports") {
  // A stage that fails must latch rather than hand the next one nothing.
  // A case that measured a stage over absent state would report a cost
  // near zero, and a near-zero figure reads as a result.
  CompilerFixture fixture("fn main() -> i32 {\n  ret \"not an i32\"\n}\n");
  fixture.resolve();
  fixture.analyze();
  fixture.lower();
  CHECK(!fixture.ok());
}

}  // namespace bench
