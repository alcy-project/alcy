// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/generator.h"

#include <string>

#include "diag/bag.h"
#include "doctest/doctest.h"
#include "fpag/base/result.h"
#include "fpag/mem/arena.h"
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
  pipeline::PipelineContext ctx;
  const std::string source =
      generate_source(SourceSpec{.functions = 40, .statements = 2, .depth = 2});
  base::Result<pipeline::CheckResult, diag::Reported> checked =
      pipeline::check_source(ctx, "bench.al", source);
  CHECK(checked.is_ok());
  CHECK(!ctx.bag.has_errors());
}

}  // namespace bench
