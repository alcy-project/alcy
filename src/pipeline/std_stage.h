// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <vector>

#include "analyzer/resolve.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"

namespace pipeline {

// Returns the selected members of the embedded standard library as the
// prelude inputs: one facade per member plus the modules beside it. Only
// selected members are returned, so a program sees exactly what its
// manifest names. Each is a virtual source named by its path within the
// suite, so nothing is written to disk and two compilations cannot read
// each other's prelude. Kept for the command and restaged when a later
// call selects different members.
std::span<const analyzer::ModuleInput> std_prelude(
    PipelineContext& ctx,
    const StdSelection& selection);

// The spec policy rows of the selected members (ADR-0053), each in the
// embedded suite and carrying the seals its manifest declares. A
// package build reads these from the loaded manifests; a single-file
// compilation has none, so the rows come from the embedded table.
std::vector<analyzer::PackagePolicy> std_package_policies(
    const StdSelection& selection);

}  // namespace pipeline
