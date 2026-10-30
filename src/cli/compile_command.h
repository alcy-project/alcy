// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "cli/cli_config.h"
#include "cli/result_code.h"
#include "diag/bag.h"
#include "pipeline/std_select.h"

namespace pipeline {

struct PipelineContext;

}  // namespace pipeline

namespace cli {

struct Envelope;

// Compiles a single source file.
ResultCode run_compile(const CliConfig& config,
                       pipeline::PipelineContext& ctx,
                       Envelope& envelope);

// Shared so other commands (`build --file`) honor `--no-std` and
// `--deps` the same way `compile` does.
base::Result<pipeline::StdSelection, diag::Reported> compile_selection(
    const CliConfig& config,
    pipeline::PipelineContext& ctx);

}  // namespace cli
