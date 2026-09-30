// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/case_pipeline.h"

#include <memory>

#include "benchmarks/clock.h"
#include "benchmarks/fixture.h"
#include "benchmarks/generator.h"
#include "benchmarks/runner.h"
#include "benchmarks/sink.h"
#include "borrow/borrow.h"
#include "ir/verifier.h"

namespace bench {

namespace {

// A whole compile is milliseconds, so the duration floor is a second
// rather than a moment, and a batch would hide the cost per compile that
// is the figure worth having.
constexpr MeasurementPolicy COMPILE = {
    .warmup = 3,
    .samples = 0,
    .min_duration_ns = 1000ull * 1000 * 1000,
    .min_batch_ns = 0,
};

// Everything up to and including lowering. It is the setup for the two
// cases that read the lowered package and cannot rebuild it per sample,
// and the whole of the setup for the ones that can.
void rebuild_through_lower(CompilerFixture& fixture) {
  fixture.resolve();
  fixture.analyze();
  fixture.lower();
}

}  // namespace

void run_pipeline_cases(Runner<SteadyClock>& runner,
                        const SourceSpec& spec,
                        const CaseFilter& filter,
                        Emitter& emit) {
  // The stages before a case's own are rebuilt in prepare and dropped
  // with it: the analyzer and the lowerer consume what they are handed,
  // so a package cannot be checked or lowered twice.
  // A pointer rather than an optional: `rebuild` is what puts it
  // there, and a unique_ptr reads without the question.
  std::unique_ptr<CompilerFixture> fixture;

  if (filter.wants(CaseId{"frontend", "resolve"})) {
    emit(
        CaseId{"frontend", "resolve"}, COMPILE,
        runner.measure(
            COMPILE, [&] { fixture = std::make_unique<CompilerFixture>(spec); },
            [&] { fixture->resolve(); }));
  }

  if (filter.wants(CaseId{"frontend", "analyze"})) {
    emit(CaseId{"frontend", "analyze"}, COMPILE,
         runner.measure(
             COMPILE,
             [&] {
               fixture = std::make_unique<CompilerFixture>(spec);
               fixture->resolve();
             },
             [&] { fixture->analyze(); }));
  }

  if (filter.wants(CaseId{"frontend", "lower"})) {
    emit(CaseId{"frontend", "lower"}, COMPILE,
         runner.measure(
             COMPILE,
             [&] {
               fixture = std::make_unique<CompilerFixture>(spec);
               fixture->resolve();
               fixture->analyze();
             },
             [&] { fixture->lower(); }));
  }

  // Borrowing takes the package by reference and does not consume it,
  // so one package serves every sample and only the check is timed. The
  // bag is the fixture's own: a clean program emits nothing, so nothing
  // accumulates across samples.
  if (filter.wants(CaseId{"frontend", "borrow"})) {
    emit(CaseId{"frontend", "borrow"}, COMPILE,
         runner.measure(
             COMPILE,
             [&] {
               fixture = std::make_unique<CompilerFixture>(spec);
               rebuild_through_lower(*fixture);
             },
             [&] {
               static_cast<void>(
                   borrow::check_borrows(fixture->lowered(), fixture->bag()));
             }));
  }

  // Verifying IR is a pure walk, so the same storage is verified every
  // sample and no package is rebuilt. The verifier uses the global
  // profiler rather than an injected one, which is why this case calls
  // it directly: the engine runs no profiler.
  if (filter.wants(CaseId{"ir", "verify-ir"})) {
    emit(CaseId{"ir", "verify-ir"}, COMPILE,
         runner.measure(
             COMPILE,
             [&] {
               fixture = std::make_unique<CompilerFixture>(spec);
               rebuild_through_lower(*fixture);
             },
             [&] {
               static_cast<void>(
                   ir::verify_storage(*fixture->lowered().storage));
             }));
  }
}

}  // namespace bench
