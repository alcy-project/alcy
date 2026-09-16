// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

namespace ir {

// Bit packed 1 B struct for instruction.
struct InstructionFlags {
  // TODO
  bool some_flag : 1 = false;
};

}  // namespace ir
