// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/std_select.h"

namespace pipeline {

// Stages the selected members of the embedded standard library and
// returns their prelude inputs: one facade per member plus the modules
// beside it. Only selected members are staged, so a program sees
// exactly what its manifest names. Restaged when the selection changes;
// failure lands in the bag and is reported as a failed Result.
base::Result<std::span<const analyzer::ModuleInput>, diag::Reported>
std_prelude(PipelineContext& ctx, const StdSelection& selection);

}  // namespace pipeline
