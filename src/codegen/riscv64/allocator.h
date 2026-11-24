// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "codegen/riscv64/frame.h"
#include "codegen/riscv64/minst.h"

namespace codegen::riscv64 {

void allocate(Body& body, const Frame& frame);

}  // namespace codegen::riscv64
