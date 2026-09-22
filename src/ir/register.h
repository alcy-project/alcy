// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "ir/common.h"

namespace ir {

// Single static assignment register
struct Register {
  TypeIdx type;
  InstructionIdx def_idx;
};

}  // namespace ir
