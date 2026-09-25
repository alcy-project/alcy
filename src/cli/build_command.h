// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "cli/cli_config.h"
#include "cli/result_code.h"

namespace diag {

struct RenderOptions;

}  // namespace diag

namespace cli {

ResultCode run_build(const CliConfig& config,
                     const diag::RenderOptions& options);

}  // namespace cli

