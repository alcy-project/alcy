// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>

#include "analyzer/resolve.h"
#include "pipeline/pipeline_context.h"

namespace pipeline {

// Stages the embedded standard library once per context and returns
// its prelude inputs (currently the core member). Empty when staging
// or loading fails; the failure lands in the bag, which callers
// already consult after resolution.
std::span<const analyzer::ModuleInput> std_prelude(PipelineContext& ctx);

// Staged file names, relative to the staging directory.
std::string_view std_core_name();

}  // namespace pipeline
