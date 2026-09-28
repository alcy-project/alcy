// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace pipeline {

struct PipelineContext;

}  // namespace pipeline

namespace cli {

struct CliConfig;
struct Envelope;

// Builds the target and executes it with inherited stdio. The envelope
// carries whatever the build collected; the program's own output is not
// part of it, since the program writes to the same terminal. Returns the
// program exit code, or the run failure as a negative.
i32 run_run(const CliConfig& config,
            pipeline::PipelineContext& ctx,
            Envelope& envelope);

}  // namespace cli
