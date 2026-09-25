// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "fpag/base/numeric.h"

namespace diag {

struct RenderOptions;

}  // namespace diag

namespace cli {

struct CliConfig;

// Builds the target and executes it with inherited stdio, returning
// the program exit code. Runner failures return a negative sentinel.
i32 run_run(const CliConfig& config, const diag::RenderOptions& options);

}  // namespace cli
