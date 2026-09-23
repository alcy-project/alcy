// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "analyzer/resolve.h"
#include "fpag/base/numeric.h"
#include "path/path.h"
#include "pipeline/pipeline_context.h"
#include "source/source.h"

namespace pipeline {

struct CheckResult {
  usize file_count;
  usize module_count;
  usize function_count;
  bool success;
};

CheckResult finish_check(PipelineContext& ctx,
                         analyzer::ModuleTree tree,
                         usize file_count);

CheckResult check_package(PipelineContext& ctx,
                          const path::Path& root,
                          source::FileId manifest_file,
                          std::string_view manifest_name);

}  // namespace pipeline
