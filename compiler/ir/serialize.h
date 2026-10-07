// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <vector>

#include "fpag/base/numeric.h"
#include "ir/write_input.h"

namespace ir {

// The lowered package in the binary form `--emit=ir-bc` writes, defined
// by `compiler/docs/ir-format.md` (ADR-0056). The same input serializes
// to the same bytes, and `deserialize` rebuilds verified IR from them.
[[nodiscard]] std::vector<u8> serialize(const WriteInput& input);

}  // namespace ir
