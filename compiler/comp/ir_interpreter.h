// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <span>
#include <string_view>

#include "comp/comp_value.h"
#include "fpag/base/numeric.h"
#include "fpag/base/result.h"
#include "ir/common.h"
#include "ir/storage.h"
#include "ir/symbol_table.h"
#include "ir/type.h"

namespace comp {

// The second engine of compile-time evaluation (ADR-0054): runs a
// lowered function's verified IR to a value. It is a differential
// oracle for the AST engine first; nothing in the pipeline calls it yet.
//
// A call gets a byte-addressable frame. Registers are tagged slots -
// scalar bits or the address of a memory object - and aggregates live
// in the frame through Alloca, GetElementPtr, Load, and Store, which is
// the shape lowering emits. A construct outside the subset the engine
// carries is refused loudly rather than approximated.
struct IrRunError {
  enum class Kind : u8 {
    // A construct the engine does not carry yet: the program is
    // rejected rather than misread.
    Unsupported,
    // Verified IR that violates an invariant the engine relies on: a
    // compiler bug, not a program property.
    Internal,
    // The run spent the control-flow budget in `quota`.
    Quota,
    // The call stack reached COMP_MAX_CALL_DEPTH.
    Depth,
    // A checked source operation overflowed (ADR-0062): the program
    // is refused at compile time the way a debug build refuses it at
    // runtime. The wrap operations are the program's own choice and
    // never take this path.
    Overflow,
  };

  Kind kind = Kind::Internal;
  // The instruction the failure came from; invalid when the failure is
  // a budget or an argument list that never reached one.
  ir::InstructionIdx instr = ir::InstructionIdx(base::INVALID_IDX);
  // Static wording. The engine composes no message of its own, so a
  // caller may report this view as it is.
  std::string_view what;
};

// The control-flow budget of compile-time evaluation, shared with the
// AST engine; the differential test pins the two together. One call and
// one loop back edge spend one unit.
inline constexpr u64 COMP_BRANCH_QUOTA = 1'000'000;
// Recursion beyond this depth is refused before it can exhaust the
// machine stack, which the quota alone would not prevent.
inline constexpr u32 COMP_MAX_CALL_DEPTH = 64;

// Runs `function` with `args` bound to its entry parameters. `quota`
// and `depth` are the run's budget, spent in place: a caller seeds them
// (COMP_BRANCH_QUOTA and zero for a fresh top-level call) and can read
// back what the run spent. `width` selects the layout the frame uses,
// as every other consumer of IR takes it explicitly.
//
// The state-based form serves a caller whose storage is still being
// built - the lowering, asking mid-pass - while the wrapper form is
// what a finished Storage offers. A call whose target is not
// materialized yet is refused as unsupported, so a caller can fall
// back rather than read past the table.
base::Result<CompVal, IrRunError> interpret_function(
    const ir::StorageState& state,
    const ir::SymbolTable& strings,
    ir::PointerWidth width,
    ir::FunctionIdx function,
    std::span<const CompVal> args,
    u64& quota,
    u32& depth);

inline base::Result<CompVal, IrRunError> interpret_function(
    const ir::Storage& storage,
    const ir::SymbolTable& strings,
    ir::PointerWidth width,
    ir::FunctionIdx function,
    std::span<const CompVal> args,
    u64& quota,
    u32& depth) {
  return interpret_function(storage.state(), strings, width, function, args,
                            quota, depth);
}

}  // namespace comp
