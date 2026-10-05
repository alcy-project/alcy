// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "ast/lane.h"

namespace ast {

// Outside a stage that spreads its work, every thread appends to the one lane
// a table without lanes answers for.
thread_local u32 current_lane_storage = 0;

}  // namespace ast
