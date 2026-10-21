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

// Formats the files the command line named, or standard input when it
// named none. The layout is the formatter's own: no flag, file, or
// environment variable is consulted for it.
ResultCode run_fmt(const CliConfig& config,
                   pipeline::PipelineContext& ctx,
                   Envelope& envelope);

}  // namespace cli
