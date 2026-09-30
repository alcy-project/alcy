// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>

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

}  // namespace pipeline
