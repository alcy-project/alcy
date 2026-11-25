// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "codegen/riscv64/isa.h"
#include "codegen/riscv64/minst.h"

namespace codegen::riscv64 {

// Emit the lui/addi pair that places a 32-bit-spaced value in `dst`.
void emit_materialize(std::vector<MInst>& insts, Reg dst, i64 value);
void emit_materialize(Body& body, Reg dst, i64 value);

}  // namespace codegen::riscv64
