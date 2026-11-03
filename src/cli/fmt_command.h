// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "cli/cli_config.h"
#include "cli/result_code.h"

namespace pipeline {

struct PipelineContext;

}  // namespace pipeline

namespace cli {

struct Envelope;

// Whether the source comes from standard input, which only
// `--stdin` asks for.
bool reads_stdin(const CliConfig& config);

// The unified diff of two layouts of one source, as --diff prints it:
// every hunk is a run of changes with the lines it is read in around
// them, and two layouts that agree produce nothing.
std::string render_diff(std::string_view path,
                        std::string_view original,
                        std::string_view formatted);

// Formats the files the command line named, or the package under the
// current directory when it named none. The layout is the
// formatter's own: no flag, file, or environment variable is
// consulted for it.
ResultCode run_fmt(const CliConfig& config,
                   pipeline::PipelineContext& ctx,
                   Envelope& envelope);

}  // namespace cli
