// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "benchmarks/clock.h"
#include "benchmarks/generator.h"
#include "benchmarks/runner.h"

namespace bench {

struct Emitter;

// The cases for the stages between a module tree and lowered IR. Each is
// named after the `--time-trace` phase it measures, and each is the call
// that phase's scope wraps in `pipeline/frontend.cc`, so the two are
// measuring the same work at different scopes.
void run_pipeline_cases(Runner<SteadyClock>& runner,
                        const SourceSpec& spec,
                        const CaseFilter& filter,
                        Emitter& emit);

}  // namespace bench
