// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "benchmarks/clock.h"
#include "benchmarks/generator.h"
#include "benchmarks/runner.h"

namespace bench {

struct Emitter;

// The cases for turning lowered IR into something a target can run.
//
// Emitting consumes the storage it is given and a module can only be
// built once, so a fresh context and module are part of the setup for
// every sample. The object case is skipped where there is no backend to
// emit for, rather than measured and found to be zero.
void run_codegen_cases(Runner<SteadyClock>& runner,
                       const SourceSpec& spec,
                       const CaseFilter& filter,
                       Emitter& emit);

}  // namespace bench
