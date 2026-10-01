// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "lowering/lowering.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"
#include "source/source.h"

namespace pipeline {

// One run of the shared frontend: type checking, lowering, and borrow
// checking over a resolved tree, plus the counts check reports. Build
// and check lower identical IR through this, so check is a projection
// of build rather than a second implementation.
struct FrontendOutput {
  lowering::LoweredPackage package;
  usize module_count = 0;
  usize function_count = 0;
};

base::Result<FrontendOutput, diag::Reported> run_frontend(
    PipelineContext& ctx,
    analyzer::ModuleTree tree);

// Prelude plus module resolution for one root file. The three
// single-file entries share it; packages resolve through their
// manifest instead.
base::Result<analyzer::ModuleTree, diag::Reported> front_end_root(
    PipelineContext& ctx,
    source::FileId root,
    const StdSelection& selection);

}  // namespace pipeline
