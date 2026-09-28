// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>

#include "analyzer/resolve.h"
#include "diag/bag.h"
#include "fpag/base/result.h"
#include "pipeline/pipeline_context.h"

namespace pipeline {

// Stages the embedded standard library once per context and returns its
// prelude inputs. Failure lands in the bag and is reported as a failed
// Result.
base::Result<std::span<const analyzer::ModuleInput>, diag::Reported>
std_prelude(PipelineContext& ctx);

}  // namespace pipeline
