// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "fpag/base/numeric.h"

namespace app {

bool valid_package_name(std::string_view name);

// Scaffolds a new package directory. Returns the process exit code.
i32 run_new(std::string_view target_dir);

}  // namespace app
