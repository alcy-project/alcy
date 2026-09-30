// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "ir/common.h"

namespace ir {

struct Block {
  InstructionIdxRange instrs;
  BlockParamIdxRange block_params;
};

}  // namespace ir
