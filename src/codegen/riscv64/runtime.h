// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string_view>

#include "codegen/riscv64/minst.h"
#include "codegen/riscv64/rodata.h"

namespace codegen::riscv64::runtime {

// Whether this runtime has a body for the named external function.
[[nodiscard]] bool defines(std::string_view name);

// The body for one name. The caller checks `defines` first.
[[nodiscard]] Body build(std::string_view name,
                         RoData& rodata,
                         bool backtrace = false);

}  // namespace codegen::riscv64::runtime
