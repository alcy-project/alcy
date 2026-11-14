// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <algorithm>
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
    // A checked source operation overflowed (ADR-0065): the program
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

// Every wording a refusal (`IrRunError::Kind::Unsupported`) can carry,
// declared in one place. What the engine does not carry is a decision
// (ADR-0054), so a case that refuses points at a wording this list owns,
// and a debug build refuses to report one the list does not declare.
inline constexpr std::string_view UNSUPPORTED_REFUSALS[] = {
    "aggregate argument in comp evaluation",
    "aggregate extraction in comp evaluation",
    "aggregate insertion in comp evaluation",
    "aggregate load in comp evaluation",
    "aggregate result in comp evaluation",
    "aggregate store in comp evaluation",
    "aggregate value in comp evaluation",
    "allocation in comp evaluation",
    "arithmetic on a non-integer in comp evaluation",
    "atomic operation in comp evaluation",
    "bit reverse on a non-integer in comp evaluation",
    "bitwise not on a non-integer in comp evaluation",
    "call to a function that is not lowered yet",
    "call to a runtime function in comp evaluation",
    "call to panic in comp evaluation",
    "call to print in comp evaluation",
    "cast between a reference and an integer in comp evaluation",
    "cast between these types in comp evaluation",
    "cast of a fat reference in comp evaluation",
    "comp argument is not an integer",
    "comparison of a non-integer in comp evaluation",
    "deallocation in comp evaluation",
    "division by zero in comp evaluation",
    "element offset in comp evaluation",
    "fat reference load in comp evaluation",
    "fat reference store in comp evaluation",
    "fence in comp evaluation",
    "float argument in comp evaluation",
    "float arithmetic in comp evaluation",
    "float cast in comp evaluation",
    "float comparison in comp evaluation",
    "float load in comp evaluation",
    "float result in comp evaluation",
    "float store in comp evaluation",
    "float value in comp evaluation",
    "function that is not lowered yet",
    "getelementptr through a non-aggregate in comp evaluation",
    "indirect call in comp evaluation",
    "non-integer constant in comp evaluation",
    "ordering of references in comp evaluation",
    "reference argument in comp evaluation",
    "reference result in comp evaluation",
    "switch in comp evaluation",
    "type query in comp evaluation",
};

inline bool declared_refusal(std::string_view what) {
  return std::find(std::begin(UNSUPPORTED_REFUSALS),
                   std::end(UNSUPPORTED_REFUSALS),
                   what) != std::end(UNSUPPORTED_REFUSALS);
}

std::span<const std::string_view> unsupported_refusals();

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
