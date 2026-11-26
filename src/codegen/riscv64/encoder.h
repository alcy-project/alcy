// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <vector>

#include "codegen/riscv64/minst.h"
#include "fpag/base/numeric.h"

namespace codegen::riscv64 {

// One body's bytes and the label offsets they carry. Label operands
// are patched to their offsets in this pass.
struct Encoded {
  std::vector<u8> bytes;
  std::vector<u64> label_offsets;
  struct Patch {
    u64 offset = 0;
    // Label patches are resolved locally; symbol patches survive as ELF
    // relocations for names another object carries.
    u32 label = 0;
    std::string symbol;
  };
  std::vector<Patch> patches;
};

[[nodiscard]] Encoded encode(const Body& body);

}  // namespace codegen::riscv64
