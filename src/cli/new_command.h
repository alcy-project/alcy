// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "cli/result_code.h"

namespace diag {

struct RenderOptions;

}  // namespace diag

namespace cli {

// Scaffolds a new package directory.
ResultCode run_new(std::string_view target_dir,
                   const diag::RenderOptions& options);

}  // namespace cli
