// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <string>
#include <vector>

#include "codegen/object/elf64.h"
#include "codegen/x86_64/minst.h"
#include "fpag/base/numeric.h"

namespace codegen::x86_64 {

// One body's bytes, plus the patches the references in it ask for.
struct Encoded {
  std::vector<u8> bytes;
  struct Patch {
    // Where in `bytes` the four bytes the patch fills start.
    u64 offset = 0;
    object::RelocKind kind = object::RelocKind::Pc32;
    // The name the patch reaches.
    std::string symbol;
    i64 addend = 0;
  };
  std::vector<Patch> patches;
};

// Encodes an allocated body. Every vreg must be gone; the encoder reads
// registers, immediates, memory, and the names of things it cannot see.
// Branches to labels inside the body are patched here, so a body's bytes
// are final and only its symbol references reach the object.
[[nodiscard]] Encoded encode(const Body& body);

}  // namespace codegen::x86_64
