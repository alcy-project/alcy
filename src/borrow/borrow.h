// Copyright 2026 The Alcy Project Authors
// This source code is licensed under the Apache License, Version 2.0 with LLVM
// Exceptions which can be found in the LICENSE file.

#pragma once

#include "diag/bag.h"
#include "lower/lower.h"

namespace borrow {

// Ownership checking over lowered IR: use-after-move, borrow
// exclusivity, and reference escape from returned values. Straight-line
// code is analyzed with linear passes (liveness from last use,
// invalidation by moves and exclusive borrows); control flow extends
// the engine with block joins. Diagnostics only; the driver gates the
// exit code on the bag.
void check_borrows(const lower::LoweredPackage& lowered, diag::DiagBag& bag);

}  // namespace borrow
