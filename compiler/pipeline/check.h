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

struct CheckOutcome {
  usize file_count;
  usize module_count;
  usize function_count;
};

// Success is the stats payload itself; failure is diag::Reported with
// diagnostics in the bag. There is no success flag inside the payload.
base::Result<CheckOutcome, diag::Reported>
finish_check(PipelineContext& ctx, analyzer::ModuleTree tree, usize file_count);

base::Result<CheckOutcome, diag::Reported> check_single_file(
    PipelineContext& ctx,
    std::string_view target);

// Checks a single source held in memory. `name` is what diagnostics and
// module naming see and need not be a path, so a caller with text but no
// file can go through the whole front end without touching the
// filesystem. This is check_single_file without the load, and it is what
// a test wants.
base::Result<CheckOutcome, diag::Reported> check_source(PipelineContext& ctx,
                                                        std::string_view name,
                                                        std::string_view bytes);

// The shared tail of the two entry points above: everything from the
// root file's module input onwards, which does not care where the bytes
// came from.
base::Result<CheckOutcome, diag::Reported> check_root(PipelineContext& ctx,
                                                      source::FileId root);

base::Result<CheckOutcome, diag::Reported> check_package(
    PipelineContext& ctx,
    const path::Path& root,
    source::FileId manifest_file,
    std::string_view manifest_name);

}  // namespace pipeline
