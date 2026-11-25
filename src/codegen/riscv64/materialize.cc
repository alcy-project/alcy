// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen/riscv64/materialize.h"

#include <vector>

#include "codegen/riscv64/isa.h"
#include "codegen/riscv64/minst.h"

namespace codegen::riscv64 {

void emit_materialize(std::vector<MInst>& insts,
                      const Reg dst,
                      const i64 value) {
  const i64 hi = upper20(value);
  const i64 lo = lower12(value);
  insts.push_back(make_inst(MOp::Lui, 8, Operand{dst}, Operand{Imm{hi << 12}}));
  insts.push_back(
      make_inst(MOp::Addi, 8, Operand{dst}, Operand{dst}, Operand{Imm{lo}}));
}

void emit_materialize(Body& body, const Reg dst, const i64 value) {
  emit_materialize(body.insts, dst, value);
}

}  // namespace codegen::riscv64
