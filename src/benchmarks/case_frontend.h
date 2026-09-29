// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "benchmarks/clock.h"
#include "benchmarks/generator.h"
#include "benchmarks/runner.h"

namespace bench {

struct Emitter;

// The frontend cases: the lexer and the parser over a generated source.
// Named after the `--time-trace` phases they measure, so the two can be
// put side by side. Each calls the module directly rather than the
// pipeline, which is what keeps the file read out of the measurement.
//
// The clock is concrete here rather than a parameter: a case is what the
// engine actually runs, and the engine's own machinery is exercised
// against a scripted clock by the unit tests instead.
void run_frontend_cases(Runner<SteadyClock>& runner,
                        const SourceSpec& spec,
                        Emitter emit);

}  // namespace bench
