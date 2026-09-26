// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

struct CheckResult {
  usize file_count;
  usize module_count;
  usize function_count;
};

// Each entry validates its raw request, then reports success as the
// stats payload and failure as diag::Reported with diagnostics in the
// bag. There is no success flag inside the payload.
base::Result<CheckResult, diag::Reported>
finish_check(PipelineContext& ctx, analyzer::ModuleTree tree, usize file_count);

base::Result<CheckResult, diag::Reported> check_single_file(
    PipelineContext& ctx,
    std::string_view target);

base::Result<CheckResult, diag::Reported> check_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name);

}  // namespace pipeline
