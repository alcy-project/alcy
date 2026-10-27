// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "cli/cli_config.h"
#include "cli/output.h"
#include "fpag/base/numeric.h"
#include "pipeline/pipeline_context.h"

namespace cli {

// Serves the language server over the process's standard streams. The
// protocol owns standard output, so no envelope is reported; the exit
// code the LSP contract gives is this command's result.
i32 run_lsp(const CliConfig& config,
            pipeline::PipelineContext& ctx,
            Envelope& envelope);

}  // namespace cli
