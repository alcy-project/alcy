// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "cli/cli_config.h"
#include "cli/result_code.h"

namespace pipeline {

struct PipelineContext;

}  // namespace pipeline

namespace cli {

struct Envelope;

// Checks a package or a single file without emitting code.
ResultCode run_check(const CliConfig& config,
                     pipeline::PipelineContext& ctx,
                     Envelope& envelope);

}  // namespace cli
