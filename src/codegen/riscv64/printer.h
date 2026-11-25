// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string>
#include <vector>

#include "codegen/object/elf64.h"
#include "codegen/riscv64/minst.h"
#include "fpag/base/numeric.h"

namespace codegen::riscv64 {

// One function's allocated body, with the symbol its bytes carry and
// the slice of the text section those bytes occupy.
struct PrintedBody {
  std::string symbol;
  Body body;
  object::SymbolBind bind = object::SymbolBind::Local;
  u64 offset = 0;
  u64 size = 0;
};

// Render the object as one GNU-as RISC-V source. Data the backend placed
// in .text is emitted as bytes; each body's instructions are printed in
// their place. RISC-V has one assembly spelling, so `asm-syntax` has no
// effect here.
[[nodiscard]] std::string print_assembly(const object::Object& object,
                                         std::span<const PrintedBody> bodies);

}  // namespace codegen::riscv64
