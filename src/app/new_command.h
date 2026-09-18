// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include <string_view>

#include "fpag/base/numeric.h"

namespace app {

bool valid_package_name(std::string_view name);

// Scaffolds a new package directory. Returns the process exit code.
i32 run_new(std::string_view target_dir);

}  // namespace app
