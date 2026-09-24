// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

using BuildResult = base::Result<void, i32>;

BuildResult build_single_file(PipelineContext& ctx,
                              std::string_view target,
                              std::string_view output,
                              bool optimize,
                              std::string_view linker);

BuildResult build_package(PipelineContext& ctx,
                          const path::Path& root,
                          source::FileId manifest_file,
                          std::string_view manifest_name,
                          std::string_view output,
                          bool optimize,
                          std::string_view linker);

}  // namespace pipeline
