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

// Called with the bin target's name immediately before the program is
// executed, and not at all if the build or the link failed. A function
// pointer and a context rather than a callback object, as the
// diagnostic renderer's source lookup, so that neither side has to
// name the other's type.
//
// This exists because only the pipeline knows when the exec is
// imminent. A caller that announced earlier would be announcing a
// build, and a caller that announced later would be announcing
// something the reader has already watched happen.
using AnnounceExec = void (*)(std::string_view target, const void* ctx);

// Outcome of a successful `run`: the exit code, which is program
// output rather than a failure. A failed build, link, or execution is
// diag::Reported with diagnostics in the bag.
struct RunOutcome {
  i32 exit_code = 0;
};

base::Result<RunOutcome, diag::Reported> run_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name,
    bool optimize,
    std::string_view linker,
    std::span<const std::string_view> args,
    AnnounceExec announce = nullptr,
    const void* announce_ctx = nullptr);

}  // namespace pipeline
