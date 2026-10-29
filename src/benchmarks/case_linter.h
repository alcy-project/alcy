// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmarks/clock.h"
#include "benchmarks/generator.h"
#include "benchmarks/runner.h"

namespace bench {

struct Emitter;

// The linter cases: the rules over a checked package. The frontend
// stages before the rules are the prepare, exactly as the borrow case's
// are; the whole of lint is measured, from the binding table to the
// findings the bag holds.
void run_linter_cases(Runner<SteadyClock>& runner,
                      const SourceSpec& spec,
                      const CaseFilter& filter,
                      Emitter& emit);

}  // namespace bench
