// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/case_linter.h"

#include <memory>
#include <utility>

#include "benchmarks/clock.h"
#include "benchmarks/fixture.h"
#include "benchmarks/generator.h"
#include "benchmarks/runner.h"
#include "benchmarks/sink.h"
#include "diag/bag.h"
#include "linter/config.h"
#include "linter/engine.h"
#include "source/source.h"

namespace bench {

namespace {

// A lint over one module is milliseconds, so the same floor the
// pipeline cases take keeps the median from wandering.
constexpr MeasurementPolicy LINT = {
    .warmup = 3,
    .samples = 0,
    .min_duration_ns = 1000ull * 1000 * 1000,
    .min_batch_ns = 0,
};

}  // namespace

void run_linter_cases(Runner<SteadyClock>& runner,
                      const SourceSpec& spec,
                      const CaseFilter& filter,
                      Emitter& emit) {
  if (!filter.wants(CaseId{"linter", "lint"})) {
    return;
  }
  // A pointer rather than an optional, as the pipeline cases declare
  // theirs: the prepare is what puts it there.
  std::unique_ptr<CompilerFixture> fixture;

  emit(CaseId{"linter", "lint"}, LINT,
       runner.measure(
           LINT,
           [&] {
             // The fixture owns the context the findings are written
             // into, so it is rebuilt per sample with the package.
             fixture = std::make_unique<CompilerFixture>(spec);
             fixture->resolve();
             fixture->analyze();
           },
           [&] {
             base::Result<linter::LintConfig, diag::Reported> config =
                 linter::parse_lint_config("", "lint.toml",
                                           source::UNKNOWN_FILE, fixture->bag(),
                                           fixture->context().arena);
             if (config.is_ok()) {
               static_cast<void>(linter::run_lints(fixture->context(),
                                                   fixture->checked(),
                                                   std::move(config).unwrap()));
             }
           }));
}

}  // namespace bench
