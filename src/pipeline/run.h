// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>

#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

// Outcome of a successful `run`: the executed program's exit code,
// which is program output, not a failure. A failed build, link, or
// execution is diag::Reported with diagnostics in the bag. The program
// inherits stdio, so its output streams straight to the terminal.
struct RunOutcome {
  i32 exit_code = 0;
};

base::Result<RunOutcome, diag::Reported> run_single_file(
    PipelineContext& ctx,
    std::string_view target,
    bool optimize,
    std::string_view linker,
    std::span<const std::string_view> args);

base::Result<RunOutcome, diag::Reported> run_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    bool optimize,
    std::string_view linker,
    std::span<const std::string_view> args);

}  // namespace pipeline
