// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "benchmarks/clock.h"
#include "benchmarks/generator.h"
#include "benchmarks/runner.h"
#include "benchmarks/sink.h"

namespace bench {

// The editor's request paths against one checked reading: the rebuild an
// edit costs, the document update itself, and the requests an editor
// sends while the reading is warm. The transport and the server loop are
// outside the number; the cases drive the same entry points the server
// calls, over one generated source.
void run_lsp_cases(Runner<SteadyClock>& runner,
                   const SourceSpec& spec,
                   const CaseFilter& filter,
                   Emitter& emit);

}  // namespace bench
