// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cli/lsp_command.h"

#include "cli/cli_config.h"
#include "cli/output.h"
#include "fpag/base/numeric.h"
#include "lsp/lsp.h"
#include "pipeline/pipeline_context.h"

namespace cli {

i32 run_lsp(const CliConfig&, pipeline::PipelineContext&, Envelope&) {
  return lsp::run(lsp::RunOptions{});
}

}  // namespace cli
