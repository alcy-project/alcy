// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "pipeline/pipeline_context.h"

namespace pipeline {

bool valid_package_name(std::string_view name);

using NewResult = base::Result<void, i32>;

NewResult create_new_package(PipelineContext& ctx, std::string_view target_dir);

// Creates alcy.toml and main.al inside an existing directory,
// deriving the package name from the directory itself.
NewResult init_package(PipelineContext& ctx, std::string_view target_dir);

}  // namespace pipeline
