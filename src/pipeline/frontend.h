// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "analyzer/resolve.h"
#include "analyzer/types.h"
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

// The analyzer half of the frontend: name resolution and type checking
// over a resolved tree. Nothing is lowered, so a reader that only wants
// the checked package - the linter - stops here instead of paying for
// IR construction it will not read. The tree is borrowed; the returned
// package's views point into the context's arena and intern table, which
// outlive it.
base::Result<analyzer::CheckedPackage, diag::Reported> analyze_tree(
    PipelineContext& ctx,
    const analyzer::ModuleTree& tree);

// The lowering and borrow half, over an already-checked package. This is
// the tail of run_frontend, split out so a caller that analyzed the tree
// itself can finish the pipeline without checking twice.
base::Result<FrontendOutput, diag::Reported> finish_frontend(
    PipelineContext& ctx,
    analyzer::CheckedPackage package);

// Whether the root module declares the free `main` a binary links.
// Check and build both reject a binary target without one.
bool has_free_main(const PipelineContext& ctx,
                   const analyzer::ModuleTree& tree);

// Prelude plus module resolution for one root file. The three
// single-file entries share it; packages resolve through their
// manifest instead.
base::Result<analyzer::ModuleTree, diag::Reported> front_end_root(
    PipelineContext& ctx,
    source::FileId root,
    const StdSelection& selection);

}  // namespace pipeline
