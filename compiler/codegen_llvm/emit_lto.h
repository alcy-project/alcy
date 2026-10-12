// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <memory>
#include <vector>

#include "codegen/backend.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"

namespace llvm {
class Module;
}  // namespace llvm

namespace codegen_llvm {

// The parts of one module, optimized and written by the ThinLTO backend: the
// thin link resolves one prevailing copy of everything the parts share, and
// each part's backend imports what the others define and runs the middle end
// and the code generator on its own thread. The parts must be linkable
// partitions of one module - `SplitModule`'s output - and they are consumed.
base::Result<std::vector<std::vector<u8>>, codegen::EmitError>
optimize_in_parts(std::vector<std::unique_ptr<llvm::Module>>& split,
                  u32 workers);

}  // namespace codegen_llvm
