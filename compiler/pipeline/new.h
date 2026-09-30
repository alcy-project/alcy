// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "pipeline/pipeline_context.h"
#include "pipeline/vcs.h"

namespace pipeline {

bool valid_package_name(std::string_view name);

// The package name, on success. `init` derives it from the directory
// rather than being given it, so the name is the result and not an
// argument the caller could already have echoed back.
using NewResult = base::Result<std::string, i32>;

NewResult create_new_package(PipelineContext& ctx,
                             std::string_view target_dir,
                             Vcs vcs);

// Creates alcy.toml and main.al inside an existing directory,
// deriving the package name from the directory itself.
NewResult init_package(PipelineContext& ctx,
                       std::string_view target_dir,
                       Vcs vcs);

}  // namespace pipeline
