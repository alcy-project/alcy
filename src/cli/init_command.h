// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "cli/result_code.h"

namespace diag {

struct RenderOptions;

}  // namespace diag

namespace cli {

struct CliConfig;

ResultCode run_init(const CliConfig& config,
                    const diag::RenderOptions& options);

}  // namespace cli
