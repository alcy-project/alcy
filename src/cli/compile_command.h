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

// Compiles a single source file.
ResultCode run_compile(const CliConfig& config,
                       pipeline::PipelineContext& ctx,
                       Envelope& envelope);

}  // namespace cli
