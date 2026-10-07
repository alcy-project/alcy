// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include "diag/bag.h"
#include "fpag/base/result.h"
#include "fpag/debug/profiler/profiler.h"
#include "ir/symbol_table.h"
#include "lowering/lowering.h"

namespace borrow {

// Ownership checking over lowered IR: use-after-move, borrow
// exclusivity, assignment to borrowed places, and reference escape
// from returned values. Moves flow forward through the CFG with
// per-block join states and a loop fixed-point; loans expire at
// last use. Interprocedural precision comes from function summaries
// (parameter positions whose loans may reach a return, computed to
// a bounded fixed-point over the call graph) reified at call sites.
// Findings accumulate in the bag; err marks a package that gained
// errors. The cli still gates the exit code on the bag.
base::Result<void, diag::Reported> check_borrows(
    const lowering::LoweredPackage& lowered,
    diag::DiagBag& bag,
    ir::SymbolTable& strings,
    debug::Profiler* profiler = nullptr);

}  // namespace borrow
