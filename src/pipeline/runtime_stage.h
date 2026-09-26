// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "diag/bag.h"
#include "fpag/base/result.h"

namespace io {
class TempDir;
}  // namespace io

namespace pipeline {

// Writes the embedded runtime sources into dir for the system compiler.
// Write failures land in the bag and are reported as a failed Result.
base::Result<void, diag::Reported> stage_runtime(io::TempDir& dir,
                                                 diag::DiagBag& bag);

// Staged file names, relative to the staging directory.
std::string_view runtime_header_name();
std::string_view runtime_source_name();

}  // namespace pipeline
